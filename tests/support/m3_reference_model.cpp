#include "m3_reference_model.h"

namespace myrd::persistence::test {
namespace {

ReferenceManifestSlotVerdict evaluate(
    const ReferenceManifestCandidate& candidate) {
    ReferenceManifestSlotVerdict verdict{};
    verdict.slot_index = candidate.slot_index;
    verdict.trusted_generation = candidate.trusted_generation;
    for (std::size_t i = 0; i < candidate.checks.size(); ++i) {
        if (candidate.checks[i] != ReferenceManifestStatus::Passed) {
            verdict.first_rejection = static_cast<ReferenceManifestCheck>(i);
            verdict.status = candidate.checks[i];
            verdict.provenance = candidate.provenance[i];
            return verdict;
        }
    }
    verdict.complete = true;
    verdict.status = ReferenceManifestStatus::Passed;
    return verdict;
}

}  // namespace

ReferenceManifestSelection reference_select_manifest(
    const std::array<ReferenceManifestCandidate, 2>& candidates) {
    ReferenceManifestSelection result{};
    result.diagnostics = {evaluate(candidates[0]), evaluate(candidates[1])};
    if (candidates[0].malformed != ReferenceMalformedKind::None ||
        candidates[1].malformed != ReferenceMalformedKind::None) {
        result.kind = ReferenceManifestSelectionKind::InvalidEvidence;
        result.malformed = candidates[0].malformed != ReferenceMalformedKind::None
            ? candidates[0].malformed : candidates[1].malformed;
        return result;
    }
    if (candidates[0].slot_index > 1 || candidates[1].slot_index > 1) {
        result.kind = ReferenceManifestSelectionKind::InvalidEvidence;
        result.malformed = ReferenceMalformedKind::SlotOutOfRange;
        return result;
    }
    if (candidates[0].slot_index == candidates[1].slot_index) {
        result.kind = ReferenceManifestSelectionKind::InvalidEvidence;
        result.malformed = ReferenceMalformedKind::DuplicateSlot;
        return result;
    }
    if (candidates[0].trusted_generation.has_value() &&
        candidates[1].trusted_generation.has_value() &&
        candidates[0].trusted_generation == candidates[1].trusted_generation &&
        candidates[0].durable_key != candidates[1].durable_key) {
        result.kind =
            ReferenceManifestSelectionKind::EqualGenerationConflict;
        for (auto& diagnostic : result.diagnostics) {
            diagnostic.complete = false;
            diagnostic.first_rejection = std::nullopt;
            diagnostic.status = ReferenceManifestStatus::Conflict;
            diagnostic.provenance.clear();
        }
        return result;
    }
    const ReferenceManifestCandidate* selected{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!result.diagnostics[i].complete ||
            !candidates[i].trusted_generation.has_value()) continue;
        if (selected == nullptr ||
            *candidates[i].trusted_generation > *selected->trusted_generation ||
            (*candidates[i].trusted_generation == *selected->trusted_generation &&
             candidates[i].slot_index < selected->slot_index)) {
            selected = &candidates[i];
        }
    }
    if (selected == nullptr) {
        result.kind = ReferenceManifestSelectionKind::NoCompleteCandidate;
        return result;
    }
    result.kind = ReferenceManifestSelectionKind::Selected;
    result.selected_slot = selected->slot_index;
    result.selected_generation = selected->trusted_generation;
    for (const auto& candidate : candidates) {
        if (candidate.trusted_generation.has_value() &&
            *candidate.trusted_generation > *selected->trusted_generation &&
            &candidate != selected) result.fallback = true;
    }
    return result;
}

}  // namespace myrd::persistence::test
