#include "src/persistence/manifest_selector.h"

#include "src/persistence/persistence_types.h"

#include <algorithm>
#include <string>

namespace myrd::persistence {
namespace {

Error selector_error(const PersistenceErrorCode code,
                     const wchar_t* message) {
    return Error{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"select_complete_manifest_generation",
        message};
}

ManifestSelectionOutcome malformed_outcome(
    std::array<ManifestCandidateDiagnostic, 2> diagnostics,
    const wchar_t* message) {
    ManifestSelectionOutcome outcome{};
    outcome.diagnostics = std::move(diagnostics);
    outcome.error = selector_error(
        PersistenceErrorCode::FatalInternalInvariant, message);
    return outcome;
}

struct EvaluatedCandidate final {
    const ManifestCandidateEvidence* evidence{};
    ManifestCandidateDiagnostic diagnostic{};
    bool complete{};
};

EvaluatedCandidate evaluate(const ManifestCandidateEvidence& candidate) {
    EvaluatedCandidate result{};
    result.evidence = &candidate;
    result.diagnostic.slot_index = candidate.slot_index;
    result.diagnostic.generation = candidate.trusted_generation;
    for (std::size_t index = 0; index < candidate.checks.size(); ++index) {
        const auto& check = candidate.checks[index];
        if (check.status == ManifestCandidateCheckStatus::Passed) {
            continue;
        }
        result.diagnostic.first_failed_check =
            static_cast<ManifestCandidateCheck>(index);
        result.diagnostic.status = check.status;
        result.diagnostic.provenance = check.provenance;
        return result;
    }
    result.complete = true;
    result.diagnostic.status = ManifestCandidateCheckStatus::Passed;
    return result;
}

bool decoded(const ManifestCandidateEvidence& candidate) noexcept {
    return candidate.slot.has_value() &&
           candidate.checks[static_cast<std::size_t>(
               ManifestCandidateCheck::SlotDecode)].status ==
               ManifestCandidateCheckStatus::Passed &&
           candidate.trusted_generation.has_value();
}

}  // namespace

bool same_durable_manifest(
    const ManifestSlot& left, const ManifestSlot& right) noexcept {
    // Slot location and diagnostic clocks do not participate in generation
    // authority.  Every field that identifies logical durable content does.
    return left.image_uuid == right.image_uuid &&
           left.generation == right.generation &&
           left.geometry.virtual_bytes == right.geometry.virtual_bytes &&
           left.geometry.logical_sector_size ==
               right.geometry.logical_sector_size &&
           left.geometry.physical_sector_size ==
               right.geometry.physical_sector_size &&
           left.base_relative_name == right.base_relative_name &&
           left.base_identity == right.base_identity &&
           left.base_expected_length == right.base_expected_length &&
           left.journal_relative_name == right.journal_relative_name &&
           left.journal_identity == right.journal_identity &&
           left.feature_flags == right.feature_flags;
}

ManifestSelectionOutcome select_complete_manifest_generation(
    const std::array<ManifestCandidateEvidence, 2>& candidates) {
    std::array<EvaluatedCandidate, 2> evaluated{
        evaluate(candidates[0]), evaluate(candidates[1])};
    std::array<ManifestCandidateDiagnostic, 2> diagnostics{
        evaluated[0].diagnostic, evaluated[1].diagnostic};

    if (candidates[0].slot_index > 1 || candidates[1].slot_index > 1 ||
        candidates[0].slot_index == candidates[1].slot_index) {
        return malformed_outcome(
            std::move(diagnostics), L"slot evidence does not contain A and B");
    }
    for (const auto& candidate : candidates) {
        const auto slot_decode = candidate.checks[static_cast<std::size_t>(
            ManifestCandidateCheck::SlotDecode)].status;
        const bool trust_checks_passed =
            slot_decode == ManifestCandidateCheckStatus::Passed &&
            candidate.checks[static_cast<std::size_t>(
                ManifestCandidateCheck::SlotSupport)].status ==
                ManifestCandidateCheckStatus::Passed &&
            candidate.checks[static_cast<std::size_t>(
                ManifestCandidateCheck::SlotSemantics)].status ==
                ManifestCandidateCheckStatus::Passed;
        const auto slot_support = candidate.checks[static_cast<std::size_t>(
            ManifestCandidateCheck::SlotSupport)].status;
        if ((slot_decode == ManifestCandidateCheckStatus::Passed &&
             slot_support == ManifestCandidateCheckStatus::Passed &&
             !candidate.slot.has_value()) ||
            (trust_checks_passed && candidate.slot.has_value() &&
             candidate.slot->slot_index != candidate.slot_index) ||
            (candidate.trusted_generation.has_value() !=
                 (trust_checks_passed && candidate.slot.has_value())) ||
            (candidate.trusted_generation.has_value() &&
             *candidate.trusted_generation !=
                 candidate.slot->generation.checkpoint_generation)) {
            return malformed_outcome(
                std::move(diagnostics),
                L"decoded slot and evidence identity are inconsistent");
        }
    }

    if (decoded(candidates[0]) && decoded(candidates[1]) &&
        candidates[0].trusted_generation ==
            candidates[1].trusted_generation &&
        !same_durable_manifest(*candidates[0].slot, *candidates[1].slot)) {
        for (auto& diagnostic : diagnostics) {
            diagnostic.first_failed_check = std::nullopt;
            diagnostic.status = ManifestCandidateCheckStatus::Conflict;
            diagnostic.provenance = std::nullopt;
            diagnostic.selected = false;
        }
        ManifestSelectionOutcome outcome{};
        outcome.diagnostics = std::move(diagnostics);
        outcome.error = selector_error(
            PersistenceErrorCode::ManifestCorrupt,
            L"equal-generation manifest slots conflict");
        return outcome;
    }

    const EvaluatedCandidate* selected{};
    for (const auto& candidate : evaluated) {
        if (!candidate.complete || !candidate.evidence->slot.has_value()) {
            continue;
        }
        if (selected == nullptr ||
            *candidate.evidence->trusted_generation >
                *selected->evidence->trusted_generation ||
            (*candidate.evidence->trusted_generation ==
                 *selected->evidence->trusted_generation &&
             candidate.evidence->slot_index < selected->evidence->slot_index)) {
            selected = &candidate;
        }
    }

    ManifestSelectionOutcome outcome{};
    outcome.diagnostics = std::move(diagnostics);
    if (selected == nullptr) {
        outcome.error = selector_error(
            PersistenceErrorCode::ManifestCorrupt,
            L"no fully valid manifest generation exists");
        return outcome;
    }

    bool fallback{};
    const auto selected_generation =
        *selected->evidence->trusted_generation;
    for (const auto& candidate : evaluated) {
        if (!candidate.complete &&
            candidate.evidence->trusted_generation.has_value() &&
            *candidate.evidence->trusted_generation >
                selected_generation) {
            fallback = true;
        }
    }
    const auto selected_slot = selected->evidence->slot_index;
    for (auto& diagnostic : outcome.diagnostics) {
        diagnostic.selected = diagnostic.slot_index == selected_slot;
    }
    outcome.selection = ManifestSelection{
        selected_slot, *selected->evidence->slot, fallback};
    return outcome;
}

}  // namespace myrd::persistence
