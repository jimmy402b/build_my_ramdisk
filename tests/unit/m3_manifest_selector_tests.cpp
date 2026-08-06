#include "test_framework.h"

#include "src/persistence/manifest_selector.h"
#include "tests/support/m3_reference_model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace myrd::persistence {
namespace {

ManifestSlot sample_slot(const std::uint8_t index,
                         const CheckpointGeneration generation) {
    ManifestSlot slot{};
    slot.slot_index = index;
    slot.image_uuid.bytes[0] = std::byte{0x11};
    slot.generation.checkpoint_generation = generation;
    slot.generation.base_generation = generation == 0 ? 0 : generation - 1;
    slot.generation.journal.epoch = 2;
    slot.generation.journal.committed_length = 64 * 1024;
    slot.generation.journal.commit = {60 * 1024, 4096, 0x1234, {}};
    slot.generation.journal.commit.chain_digest[0] = std::byte{0x22};
    slot.generation.journal.root = {56 * 1024, 4096, 0x5678};
    slot.geometry = {8 * 1024 * 1024, 512, 4096};
    slot.base_relative_name = "base.vhdx";
    slot.base_identity.file_write_guid[0] = std::byte{0x31};
    slot.base_identity.data_write_guid[0] = std::byte{0x32};
    slot.base_expected_length = 6 * 1024 * 1024;
    slot.journal_relative_name = "journal.rdj";
    slot.journal_identity.journal_uuid[0] = std::byte{0x41};
    slot.feature_flags = 3;
    slot.created_time_100ns = 100;
    slot.committed_time_100ns = 200 + generation;
    return slot;
}

ManifestCandidateEvidence complete_candidate(
    const std::uint8_t index, const CheckpointGeneration generation) {
    ManifestCandidateEvidence candidate{};
    candidate.slot_index = index;
    candidate.slot = sample_slot(index, generation);
    candidate.trusted_generation = generation;
    for (auto& check : candidate.checks) {
        check.status = ManifestCandidateCheckStatus::Passed;
    }
    return candidate;
}

ManifestCandidateEvidence rejected_candidate(
    const std::uint8_t index, const CheckpointGeneration generation,
    const ManifestCandidateCheck failed,
    const ManifestCandidateCheckStatus status, const Error& error) {
    auto candidate = complete_candidate(index, generation);
    const auto position = static_cast<std::size_t>(failed);
    candidate.checks[position].status = status;
    candidate.checks[position].provenance = error;
    if (position <= static_cast<std::size_t>(
            ManifestCandidateCheck::SlotSemantics)) {
        candidate.trusted_generation = std::nullopt;
    }
    for (std::size_t later = position + 1;
         later < candidate.checks.size(); ++later) {
        candidate.checks[later].status =
            ManifestCandidateCheckStatus::NotRun;
    }
    return candidate;
}

const ManifestCandidateDiagnostic& diagnostic_for(
    const ManifestSelectionOutcome& outcome, const std::uint8_t slot) {
    for (const auto& diagnostic : outcome.diagnostics) {
        if (diagnostic.slot_index == slot) return diagnostic;
    }
    REQUIRE(false);
    return outcome.diagnostics[0];
}

ManifestCandidateCheckStatus product_status(
    const test::ReferenceManifestStatus status) {
    using R = test::ReferenceManifestStatus;
    switch (status) {
    case R::NotRun: return ManifestCandidateCheckStatus::NotRun;
    case R::Passed: return ManifestCandidateCheckStatus::Passed;
    case R::Missing: return ManifestCandidateCheckStatus::Missing;
    case R::Corrupt: return ManifestCandidateCheckStatus::Corrupt;
    case R::Unsupported: return ManifestCandidateCheckStatus::Unsupported;
    case R::AccessDenied: return ManifestCandidateCheckStatus::AccessDenied;
    case R::IoError: return ManifestCandidateCheckStatus::IoError;
    case R::Cancelled: return ManifestCandidateCheckStatus::Cancelled;
    case R::DeadlineExceeded: return ManifestCandidateCheckStatus::DeadlineExceeded;
    case R::Conflict: return ManifestCandidateCheckStatus::Conflict;
    }
    return ManifestCandidateCheckStatus::NotRun;
}

TEST_CASE(manifest_selector_selects_highest_fully_valid_generation) {
    const std::array candidates{
        complete_candidate(0, 7), complete_candidate(1, 9)};
    const auto outcome = select_complete_manifest_generation(candidates);
    REQUIRE(outcome.ok());
    REQUIRE_EQ(outcome.selection->slot_index, std::uint8_t{1});
    REQUIRE_EQ(outcome.selection->slot.generation.checkpoint_generation, 9ULL);
    REQUIRE(!outcome.selection->fallback);
    REQUIRE(diagnostic_for(outcome, 1).selected);
}

TEST_CASE(manifest_selector_falls_back_with_stable_error_provenance) {
    const Error corruption{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::BackingDataCorrupt),
        L"validate.base", L"identity mismatch", ERROR_FILE_CORRUPT};
    const std::array candidates{
        complete_candidate(0, 7),
        rejected_candidate(1, 9, ManifestCandidateCheck::BaseIdentity,
                           ManifestCandidateCheckStatus::Corrupt,
                           corruption)};
    const auto outcome = select_complete_manifest_generation(candidates);
    REQUIRE(outcome.ok());
    REQUIRE_EQ(outcome.selection->slot_index, std::uint8_t{0});
    REQUIRE(outcome.selection->fallback);
    const auto& rejected = diagnostic_for(outcome, 1);
    REQUIRE_EQ(rejected.first_failed_check,
               std::optional{ManifestCandidateCheck::BaseIdentity});
    REQUIRE_EQ(rejected.status, ManifestCandidateCheckStatus::Corrupt);
    REQUIRE_EQ(rejected.provenance->operation, corruption.operation);
    REQUIRE_EQ(rejected.provenance->native_code, corruption.native_code);
}

TEST_CASE(manifest_selector_requires_every_validation_check) {
    for (std::size_t missing = 0;
         missing < kManifestCandidateCheckCount; ++missing) {
        auto higher = complete_candidate(1, 9);
        higher.checks[missing].status =
            ManifestCandidateCheckStatus::NotRun;
        if (missing <= static_cast<std::size_t>(
                ManifestCandidateCheck::SlotSemantics)) {
            higher.trusted_generation = std::nullopt;
        }
        const std::array candidates{
            complete_candidate(0, 7), std::move(higher)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(outcome.ok());
        REQUIRE_EQ(outcome.selection->slot_index, std::uint8_t{0});
        REQUIRE_EQ(outcome.selection->fallback,
            missing > static_cast<std::size_t>(
                ManifestCandidateCheck::SlotSemantics));
        REQUIRE_EQ(diagnostic_for(outcome, 1).first_failed_check,
                   std::optional{
                       static_cast<ManifestCandidateCheck>(missing)});
    }
}

TEST_CASE(manifest_selector_uses_first_check_and_distinguishes_failure_kind) {
    auto higher = complete_candidate(1, 9);
    higher.checks[static_cast<std::size_t>(
        ManifestCandidateCheck::BasePath)].status =
            ManifestCandidateCheckStatus::Corrupt;
    higher.checks[static_cast<std::size_t>(
        ManifestCandidateCheck::JournalDigest)].status =
            ManifestCandidateCheckStatus::Missing;
    const std::array candidates{
        complete_candidate(0, 7), std::move(higher)};
    const auto outcome = select_complete_manifest_generation(candidates);
    REQUIRE(outcome.ok());
    const auto& diagnostic = diagnostic_for(outcome, 1);
    REQUIRE_EQ(diagnostic.first_failed_check,
               std::optional{ManifestCandidateCheck::BasePath});
    REQUIRE_EQ(diagnostic.status, ManifestCandidateCheckStatus::Corrupt);
}

TEST_CASE(manifest_selector_reports_both_invalid_without_losing_reasons) {
    const Error missing{ErrorDomain::win32, ERROR_FILE_NOT_FOUND,
                        L"open.base", L"missing", ERROR_FILE_NOT_FOUND};
    const Error corrupt{ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::JournalCorrupt),
        L"read.journal", L"corrupt", ERROR_CRC};
    const std::array candidates{
        rejected_candidate(0, 7, ManifestCandidateCheck::BasePresence,
                           ManifestCandidateCheckStatus::Missing, missing),
        rejected_candidate(1, 9, ManifestCandidateCheck::JournalCommit,
                           ManifestCandidateCheckStatus::Corrupt, corrupt)};
    const auto outcome = select_complete_manifest_generation(candidates);
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
        PersistenceErrorCode::ManifestCorrupt));
    REQUIRE_EQ(diagnostic_for(outcome, 0).status,
               ManifestCandidateCheckStatus::Missing);
    REQUIRE_EQ(diagnostic_for(outcome, 1).status,
               ManifestCandidateCheckStatus::Corrupt);
}

TEST_CASE(manifest_selector_accepts_equivalent_equal_generation_slots) {
    auto first = complete_candidate(0, 8);
    auto second = complete_candidate(1, 8);
    second.slot->created_time_100ns += 10;
    second.slot->committed_time_100ns += 20;
    const std::array candidates{first, second};
    const auto outcome = select_complete_manifest_generation(candidates);
    REQUIRE(outcome.ok());
    REQUIRE_EQ(outcome.selection->slot_index, std::uint8_t{0});
    REQUIRE(!outcome.selection->fallback);
}

TEST_CASE(manifest_selector_rejects_equal_generation_durable_conflicts) {
    using Mutation = void (*)(ManifestSlot&);
    const std::array<Mutation, 21> mutations{
        +[](ManifestSlot& s) { s.image_uuid.bytes[1] = std::byte{1}; },
        +[](ManifestSlot& s) { s.geometry.virtual_bytes += 512; },
        +[](ManifestSlot& s) { s.geometry.logical_sector_size = 4096; },
        +[](ManifestSlot& s) { s.geometry.physical_sector_size = 8192; },
        +[](ManifestSlot& s) { s.generation.base_generation += 1; },
        +[](ManifestSlot& s) { s.generation.journal.epoch += 1; },
        +[](ManifestSlot& s) { s.generation.journal.committed_length += 4096; },
        +[](ManifestSlot& s) { s.generation.journal.commit.offset += 4096; },
        +[](ManifestSlot& s) { s.generation.journal.commit.length += 4096; },
        +[](ManifestSlot& s) { s.generation.journal.commit.crc32c += 1; },
        +[](ManifestSlot& s) { s.generation.journal.commit.chain_digest[1] = std::byte{1}; },
        +[](ManifestSlot& s) { s.generation.journal.root.offset += 4096; },
        +[](ManifestSlot& s) { s.generation.journal.root.length += 4096; },
        +[](ManifestSlot& s) { s.generation.journal.root.crc32c += 1; },
        +[](ManifestSlot& s) { s.base_relative_name = "other.vhdx"; },
        +[](ManifestSlot& s) { s.base_identity.file_write_guid[1] = std::byte{1}; },
        +[](ManifestSlot& s) { s.base_identity.data_write_guid[1] = std::byte{1}; },
        +[](ManifestSlot& s) { s.base_expected_length += 1; },
        +[](ManifestSlot& s) { s.journal_relative_name = "other.rdj"; },
        +[](ManifestSlot& s) { s.journal_identity.journal_uuid[1] = std::byte{1}; },
        +[](ManifestSlot& s) { s.feature_flags += 1; }};
    for (const auto mutation : mutations) {
        auto first = complete_candidate(0, 8);
        auto second = complete_candidate(1, 8);
        mutation(*second.slot);
        const std::array candidates{first, second};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::ManifestCorrupt));
        REQUIRE_EQ(diagnostic_for(outcome, 0).status,
                   ManifestCandidateCheckStatus::Conflict);
        REQUIRE_EQ(diagnostic_for(outcome, 1).status,
                   ManifestCandidateCheckStatus::Conflict);
    }
    const Error rejected{ErrorDomain::persistence, 0x5501,
                         L"oracle", L"later validation failed"};
    auto complete = complete_candidate(0, 8);
    auto rejected_peer = rejected_candidate(
        1, 8, ManifestCandidateCheck::BaseIdentity,
        ManifestCandidateCheckStatus::Corrupt, rejected);
    rejected_peer.slot->base_relative_name = "conflicting.vhdx";
    const std::array mixed{complete, rejected_peer};
    const auto mixed_outcome = select_complete_manifest_generation(mixed);
    REQUIRE(!mixed_outcome.ok());
    REQUIRE_EQ(diagnostic_for(mixed_outcome, 0).status,
               ManifestCandidateCheckStatus::Conflict);
    REQUIRE_EQ(diagnostic_for(mixed_outcome, 1).status,
               ManifestCandidateCheckStatus::Conflict);
    test::ReferenceManifestCandidate reference_complete{};
    reference_complete.slot_index = 0;
    reference_complete.trusted_generation = 8;
    reference_complete.durable_key = "left";
    reference_complete.checks.fill(test::ReferenceManifestStatus::Passed);
    auto reference_rejected = reference_complete;
    reference_rejected.slot_index = 1;
    reference_rejected.durable_key = "right";
    reference_rejected.checks[static_cast<std::size_t>(
        test::ReferenceManifestCheck::BaseIdentity)] =
            test::ReferenceManifestStatus::Corrupt;
    const auto reference_conflict = test::reference_select_manifest(
        std::array{reference_complete, reference_rejected});
    REQUIRE_EQ(reference_conflict.kind,
               test::ReferenceManifestSelectionKind::EqualGenerationConflict);
    REQUIRE_EQ(reference_conflict.diagnostics[0].status,
               test::ReferenceManifestStatus::Conflict);
    REQUIRE_EQ(reference_conflict.diagnostics[1].status,
               test::ReferenceManifestStatus::Conflict);
}

TEST_CASE(manifest_selector_rejects_malformed_evidence_shape) {
    const std::array malformed_kinds{
        test::ReferenceMalformedKind::SlotOutOfRange,
        test::ReferenceMalformedKind::DuplicateSlot,
        test::ReferenceMalformedKind::MissingDecodedSlot,
        test::ReferenceMalformedKind::TrustStateMismatch,
        test::ReferenceMalformedKind::TrustedGenerationMismatch};
    for (const auto malformed : malformed_kinds) {
        test::ReferenceManifestCandidate first{};
        first.slot_index = 0;
        first.trusted_generation = 7;
        first.durable_key = "d7";
        first.checks.fill(test::ReferenceManifestStatus::Passed);
        first.malformed = malformed;
        auto second = first;
        second.slot_index = 1;
        second.trusted_generation = 8;
        second.durable_key = "d8";
        second.malformed = test::ReferenceMalformedKind::None;
        const auto expected = test::reference_select_manifest(
            std::array{first, second});
        REQUIRE_EQ(expected.kind,
                   test::ReferenceManifestSelectionKind::InvalidEvidence);
        REQUIRE_EQ(expected.malformed, malformed);
    }
    auto mismatched = complete_candidate(0, 7);
    mismatched.slot->slot_index = 1;
    {
        const std::array candidates{mismatched, complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        const std::array candidates{
            complete_candidate(0, 7), complete_candidate(0, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        const std::array candidates{
            complete_candidate(2, 7), complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        auto missing_slot = complete_candidate(0, 7);
        missing_slot.slot = std::nullopt;
        const std::array candidates{missing_slot, complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        auto untrusted = complete_candidate(0, 7);
        untrusted.trusted_generation = std::nullopt;
        const std::array candidates{untrusted, complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        auto false_trust = complete_candidate(0, 7);
        false_trust.checks[static_cast<std::size_t>(
            ManifestCandidateCheck::SlotSupport)].status =
                ManifestCandidateCheckStatus::Unsupported;
        const std::array candidates{false_trust, complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
    {
        auto wrong_generation = complete_candidate(0, 7);
        wrong_generation.trusted_generation = 6;
        const std::array candidates{wrong_generation, complete_candidate(1, 8)};
        const auto outcome = select_complete_manifest_generation(candidates);
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.error->code, static_cast<std::uint32_t>(
            PersistenceErrorCode::FatalInternalInvariant));
    }
}

TEST_CASE(manifest_selector_matches_independent_ab_reference_matrix) {
    using Pair = std::pair<test::ReferenceManifestCheck,
                           ManifestCandidateCheck>;
    const std::array<Pair, 19> frozen_order{{
        {test::ReferenceManifestCheck::SlotDecode, ManifestCandidateCheck::SlotDecode},
        {test::ReferenceManifestCheck::SlotSupport, ManifestCandidateCheck::SlotSupport},
        {test::ReferenceManifestCheck::SlotSemantics, ManifestCandidateCheck::SlotSemantics},
        {test::ReferenceManifestCheck::BasePath, ManifestCandidateCheck::BasePath},
        {test::ReferenceManifestCheck::JournalPath, ManifestCandidateCheck::JournalPath},
        {test::ReferenceManifestCheck::BasePresence, ManifestCandidateCheck::BasePresence},
        {test::ReferenceManifestCheck::BaseOpen, ManifestCandidateCheck::BaseOpen},
        {test::ReferenceManifestCheck::BaseIdentity, ManifestCandidateCheck::BaseIdentity},
        {test::ReferenceManifestCheck::BaseExpectedLength, ManifestCandidateCheck::BaseExpectedLength},
        {test::ReferenceManifestCheck::BaseProfile, ManifestCandidateCheck::BaseProfile},
        {test::ReferenceManifestCheck::BaseLayout, ManifestCandidateCheck::BaseLayout},
        {test::ReferenceManifestCheck::JournalPresence, ManifestCandidateCheck::JournalPresence},
        {test::ReferenceManifestCheck::JournalOpen, ManifestCandidateCheck::JournalOpen},
        {test::ReferenceManifestCheck::JournalIdentity, ManifestCandidateCheck::JournalIdentity},
        {test::ReferenceManifestCheck::JournalHeader, ManifestCandidateCheck::JournalHeader},
        {test::ReferenceManifestCheck::JournalCommittedLength, ManifestCandidateCheck::JournalCommittedLength},
        {test::ReferenceManifestCheck::JournalCommit, ManifestCandidateCheck::JournalCommit},
        {test::ReferenceManifestCheck::JournalRoot, ManifestCandidateCheck::JournalRoot},
        {test::ReferenceManifestCheck::JournalDigest, ManifestCandidateCheck::JournalDigest}}};

    for (std::size_t failed = 0; failed < frozen_order.size(); ++failed) {
        test::ReferenceManifestCandidate reference_high{};
        reference_high.slot_index = 1;
        reference_high.trusted_generation = failed <= 2
            ? std::nullopt : std::optional<std::uint64_t>{9};
        reference_high.durable_key = "d9";
        reference_high.provenance[failed] = "oracle";
        reference_high.checks.fill(test::ReferenceManifestStatus::Passed);
        reference_high.checks[failed] = test::ReferenceManifestStatus::Corrupt;
        for (std::size_t later = failed + 1;
             later < reference_high.checks.size(); ++later) {
            reference_high.checks[later] = test::ReferenceManifestStatus::NotRun;
        }
        test::ReferenceManifestCandidate reference_low{};
        reference_low.slot_index = 0;
        reference_low.trusted_generation = 7;
        reference_low.durable_key = "d7";
        reference_low.checks.fill(test::ReferenceManifestStatus::Passed);
        const std::array reference{reference_low, reference_high};
        const auto expected = test::reference_select_manifest(reference);
        const Error failure{ErrorDomain::persistence, 0x5401,
                            L"oracle", L"rejected"};
        const std::array product{
            complete_candidate(0, 7),
            rejected_candidate(1, 9, frozen_order[failed].second,
                ManifestCandidateCheckStatus::Corrupt, failure)};
        const auto actual = select_complete_manifest_generation(product);
        REQUIRE_EQ(expected.kind,
                   test::ReferenceManifestSelectionKind::Selected);
        REQUIRE(actual.ok());
        REQUIRE_EQ(actual.selection->slot_index, *expected.selected_slot);
        REQUIRE_EQ(actual.selection->slot.generation.checkpoint_generation,
                   *expected.selected_generation);
        REQUIRE_EQ(actual.selection->fallback, expected.fallback);
        REQUIRE_EQ(expected.diagnostics[1].first_rejection,
                   std::optional{frozen_order[failed].first});
        REQUIRE_EQ(diagnostic_for(actual, 1).first_failed_check,
                   std::optional{frozen_order[failed].second});
        REQUIRE_EQ(diagnostic_for(actual, 1).status,
                   product_status(expected.diagnostics[1].status));
        REQUIRE_EQ(expected.diagnostics[1].provenance, std::string{"oracle"});
        REQUIRE_EQ(diagnostic_for(actual, 1).provenance->operation,
                   std::wstring{L"oracle"});
    }

    // Every adjacent pair independently freezes relative priority. Swapping
    // any two production enum values now changes the observed first reason.
    for (std::size_t first = 0; first + 1 < frozen_order.size(); ++first) {
        test::ReferenceManifestCandidate reference_high{};
        reference_high.slot_index = 1;
        reference_high.trusted_generation = first <= 2
            ? std::nullopt : std::optional<std::uint64_t>{9};
        reference_high.durable_key = "d9";
        reference_high.checks.fill(test::ReferenceManifestStatus::Passed);
        reference_high.checks[first] = test::ReferenceManifestStatus::Corrupt;
        reference_high.checks[first + 1] = test::ReferenceManifestStatus::Missing;
        test::ReferenceManifestCandidate reference_low{};
        reference_low.slot_index = 0;
        reference_low.trusted_generation = 7;
        reference_low.durable_key = "d7";
        reference_low.checks.fill(test::ReferenceManifestStatus::Passed);
        const auto expected = test::reference_select_manifest(
            std::array{reference_low, reference_high});
        auto high = complete_candidate(1, 9);
        high.checks[static_cast<std::size_t>(frozen_order[first].second)].status =
            ManifestCandidateCheckStatus::Corrupt;
        high.checks[static_cast<std::size_t>(frozen_order[first + 1].second)].status =
            ManifestCandidateCheckStatus::Missing;
        if (first <= 2) high.trusted_generation = std::nullopt;
        const std::array product{complete_candidate(0, 7), high};
        const auto actual = select_complete_manifest_generation(product);
        REQUIRE(actual.ok());
        REQUIRE_EQ(expected.diagnostics[1].first_rejection,
                   std::optional{frozen_order[first].first});
        REQUIRE_EQ(diagnostic_for(actual, 1).first_failed_check,
                   std::optional{frozen_order[first].second});
        REQUIRE_EQ(diagnostic_for(actual, 1).status,
                   product_status(expected.diagnostics[1].status));
    }
}

TEST_CASE(manifest_selector_accepts_structurally_decoded_unsupported_slot) {
    auto unsupported = ManifestCandidateEvidence{};
    unsupported.slot_index = 1;
    unsupported.checks[static_cast<std::size_t>(
        ManifestCandidateCheck::SlotDecode)].status =
            ManifestCandidateCheckStatus::Passed;
    unsupported.checks[static_cast<std::size_t>(
        ManifestCandidateCheck::SlotSupport)].status =
            ManifestCandidateCheckStatus::Unsupported;
    const std::array candidates{complete_candidate(0, 7), unsupported};

    const auto outcome = select_complete_manifest_generation(candidates);

    REQUIRE(outcome.ok());
    REQUIRE_EQ(outcome.selection->slot_index, std::uint8_t{0});
    REQUIRE(!outcome.selection->fallback);
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::SlotSupport});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::Unsupported);
}

TEST_CASE(manifest_selector_matches_symmetric_independent_status_matrix) {
    using R = test::ReferenceManifestStatus;
    const std::array statuses{R::Missing, R::Corrupt, R::Unsupported,
        R::AccessDenied, R::IoError, R::Cancelled, R::DeadlineExceeded};
    for (const auto status : statuses) {
        for (const std::uint8_t rejected_slot : {std::uint8_t{0},
                                                 std::uint8_t{1}}) {
            const auto complete_slot = static_cast<std::uint8_t>(1 - rejected_slot);
            test::ReferenceManifestCandidate rejected{};
            rejected.slot_index = rejected_slot;
            rejected.trusted_generation = 9;
            rejected.durable_key = "d9";
            rejected.checks.fill(R::Passed);
            const auto check = static_cast<std::size_t>(
                test::ReferenceManifestCheck::JournalOpen);
            rejected.checks[check] = status;
            rejected.provenance[check] = "journal-open";
            test::ReferenceManifestCandidate complete{};
            complete.slot_index = complete_slot;
            complete.trusted_generation = 7;
            complete.durable_key = "d7";
            complete.checks.fill(R::Passed);
            std::array reference{complete, rejected};
            if (reference[0].slot_index != 0) std::swap(reference[0], reference[1]);
            const auto expected = test::reference_select_manifest(reference);

            const Error error{ErrorDomain::persistence, 0x5601,
                              L"journal-open", L"rejected"};
            auto product_rejected = rejected_candidate(
                rejected_slot, 9, ManifestCandidateCheck::JournalOpen,
                product_status(status), error);
            auto product_complete = complete_candidate(complete_slot, 7);
            std::array product{product_complete, product_rejected};
            if (product[0].slot_index != 0) std::swap(product[0], product[1]);
            const auto actual = select_complete_manifest_generation(product);

            REQUIRE(actual.ok());
            REQUIRE_EQ(actual.selection->slot_index, *expected.selected_slot);
            REQUIRE_EQ(actual.selection->fallback, expected.fallback);
            REQUIRE_EQ(diagnostic_for(actual, rejected_slot).status,
                       product_status(status));
            REQUIRE_EQ(diagnostic_for(actual, rejected_slot).provenance->operation,
                       std::wstring{L"journal-open"});
        }
    }
}

}  // namespace
}  // namespace myrd::persistence
