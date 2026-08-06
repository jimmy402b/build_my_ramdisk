#include "test_framework.h"

#include "src/persistence/commit_resolver.h"
#include "tests/support/fault_file.h"

#include <array>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::persistence {
namespace {

ManifestSlot active_manifest() {
    ManifestSlot slot{};
    slot.slot_index = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        slot.image_uuid.bytes[index] =
            static_cast<std::byte>(0x10U + index);
        slot.base_identity.file_write_guid[index] =
            static_cast<std::byte>(0x20U + index);
        slot.base_identity.data_write_guid[index] =
            static_cast<std::byte>(0x30U + index);
        slot.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x40U + index);
    }
    slot.generation.checkpoint_generation = 5;
    slot.generation.base_generation = 4;
    slot.generation.journal.epoch = 1;
    slot.generation.journal.committed_length = 0x6000;
    slot.generation.journal.commit = {0x5000, 0x1000, 0x12345678, {}};
    slot.geometry = {64ull * 1024 * 1024, 512, 4096};
    slot.base_relative_name = "base-0004.vhdx";
    slot.base_expected_length = 8ull * 1024 * 1024;
    slot.journal_relative_name = "journal-e0001.rdj";
    slot.feature_flags = 1;
    slot.created_time_100ns = 100;
    slot.committed_time_100ns = 200;
    return slot;
}

PreparedCheckpoint prepared_checkpoint() {
    PreparedCheckpoint checkpoint{};
    checkpoint.generation = 6;
    checkpoint.root = {0x7000, 0x1000, 0x89ABCDEF};
    checkpoint.commit = {0x8000, 0x1000, 0x10203040, {}};
    for (std::size_t index = 0; index < checkpoint.chain_digest.size();
         ++index) {
        checkpoint.chain_digest[index] =
            static_cast<std::byte>(0x50U + index);
    }
    checkpoint.commit.chain_digest = checkpoint.chain_digest;
    checkpoint.committed_length = 0x9000;
    return checkpoint;
}

ManifestSlot attempted_manifest() {
    const auto prepared = prepare_manifest_candidate(
        active_manifest(), prepared_checkpoint(), 300);
    REQUIRE(prepared.ok());
    return prepared.value().slot;
}

PersistenceFailure indeterminate_failure(const std::uint64_t operation_id) {
    PersistenceErrorContext context{};
    context.operation = "checkpoint.publish";
    context.code = PersistenceErrorCode::CommitIndeterminate;
    context.phase = SessionPhase::Checkpointing;
    context.native_code = 1117;
    context.operation_id = operation_id;
    context.image_uuid = active_manifest().image_uuid;
    context.generation = 6;
    context.disposition = CommitDisposition::CommitIndeterminate;
    context.safe_context = "manifest publication outcome unknown";
    return {{ErrorDomain::persistence,
             static_cast<std::uint32_t>(
                 PersistenceErrorCode::CommitIndeterminate),
             L"checkpoint.publish", L"injected first failure", 1117},
            std::move(context)};
}

PublishOperationLease enter_resolution(
    ImageSession& session,
    const std::uint64_t operation_id) {
    auto checkpoint = session.begin_checkpoint();
    REQUIRE(checkpoint.ok());
    REQUIRE(checkpoint.value().note_publish_attempt_started().ok());
    REQUIRE(checkpoint.value()
                .finish(CommitDisposition::CommitIndeterminate,
                        indeterminate_failure(operation_id))
                .ok());
    auto resolver = session.begin_commit_resolution();
    REQUIRE(resolver.ok());
    return resolver.move_value();
}

TEST_CASE(commit_publish_registry_freezes_all_manifest_boundaries) {
    std::ifstream input{MYRD_M3_FAULT_PHASE_REGISTRY_PATH,
                        std::ios::binary};
    REQUIRE(input.good());
    const std::string json{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    constexpr std::array required_ids{
        "manifest.slot_write.before",
        "manifest.slot_write.short",
        "manifest.slot_write.torn",
        "manifest.slot_write.after",
        "manifest.flush.before",
        "manifest.flush.error",
        "manifest.flush.after",
        "manifest.readback.before",
        "manifest.readback.corrupt",
        "manifest.readback.after",
    };
    for (const std::string_view id : required_ids) {
        const std::string needle =
            "\"id\": \"" + std::string{id} + "\"";
        REQUIRE(json.find(needle) != std::string::npos);
        REQUIRE_EQ(json.find(needle), json.rfind(needle));
    }
}

TEST_CASE(commit_publish_classification_never_downgrades_commit_knowledge) {
    struct Case final {
        ManifestPublishBoundary boundary;
        CommitDisposition failure_disposition;
        bool commit_proven;
        bool cancellation_permitted;
    };
    constexpr std::array cases{
        Case{ManifestPublishBoundary::BeforeSlotWriteAttempt,
             CommitDisposition::NotCommitted, false, true},
        Case{ManifestPublishBoundary::SlotWriteAttempted,
             CommitDisposition::CommitIndeterminate, false, false},
        Case{ManifestPublishBoundary::ManifestFlushAttempted,
             CommitDisposition::CommitIndeterminate, false, false},
        Case{ManifestPublishBoundary::ManifestFlushSucceeded,
             CommitDisposition::CommittedNeedsReconciliation, true, false},
        Case{ManifestPublishBoundary::PostCommitReadback,
             CommitDisposition::CommittedNeedsReconciliation, true, false},
        Case{ManifestPublishBoundary::PostCommitValidation,
             CommitDisposition::CommittedNeedsReconciliation, true, false},
    };
    for (const auto& item : cases) {
        REQUIRE_EQ(classify_manifest_publish_failure(item.boundary),
                   item.failure_disposition);
        REQUIRE_EQ(manifest_commit_proven(item.boundary),
                   item.commit_proven);
        REQUIRE_EQ(manifest_publish_cancellation_permitted(item.boundary),
                   item.cancellation_permitted);
    }
}

TEST_CASE(commit_publish_crash_model_detects_phase_and_disposition_mutations) {
    struct CrashCut final {
        ManifestPublishBoundary boundary;
        ManifestKillRecovery allowed;
        CommitDisposition failure;
    };
    constexpr std::array oracle{
        CrashCut{ManifestPublishBoundary::BeforeSlotWriteAttempt,
                 ManifestKillRecovery::OldCompleteGenerationOnly,
                 CommitDisposition::NotCommitted},
        CrashCut{ManifestPublishBoundary::SlotWriteAttempted,
                 ManifestKillRecovery::OldOrNewCompleteGeneration,
                 CommitDisposition::CommitIndeterminate},
        CrashCut{ManifestPublishBoundary::ManifestFlushAttempted,
                 ManifestKillRecovery::OldOrNewCompleteGeneration,
                 CommitDisposition::CommitIndeterminate},
        CrashCut{ManifestPublishBoundary::ManifestFlushSucceeded,
                 ManifestKillRecovery::NewCompleteGenerationOnly,
                 CommitDisposition::CommittedNeedsReconciliation},
        CrashCut{ManifestPublishBoundary::PostCommitReadback,
                 ManifestKillRecovery::NewCompleteGenerationOnly,
                 CommitDisposition::CommittedNeedsReconciliation},
        CrashCut{ManifestPublishBoundary::PostCommitValidation,
                 ManifestKillRecovery::NewCompleteGenerationOnly,
                 CommitDisposition::CommittedNeedsReconciliation},
    };
    const auto matches_oracle = [](const auto& table) {
        for (const auto& cut : table) {
            if (manifest_kill_recovery_set(cut.boundary) != cut.allowed ||
                classify_manifest_publish_failure(cut.boundary) !=
                    cut.failure) {
                return false;
            }
        }
        return true;
    };
    REQUIRE(matches_oracle(oracle));

    auto swapped_write_and_flush = oracle;
    std::swap(swapped_write_and_flush[1].boundary,
              swapped_write_and_flush[3].boundary);
    REQUIRE(!matches_oracle(swapped_write_and_flush));
    auto swapped_flush_and_readback = oracle;
    std::swap(swapped_flush_and_readback[2].boundary,
              swapped_flush_and_readback[4].boundary);
    REQUIRE(!matches_oracle(swapped_flush_and_readback));
    for (std::size_t index = 0; index < oracle.size(); ++index) {
        auto wrong_disposition = oracle;
        wrong_disposition[index].failure =
            wrong_disposition[index].failure ==
                    CommitDisposition::NotCommitted
                ? CommitDisposition::CommittedNeedsReconciliation
                : CommitDisposition::NotCommitted;
        REQUIRE(!matches_oracle(wrong_disposition));
    }
}

TEST_CASE(commit_publish_prepares_exact_inactive_slot_without_file_io) {
    const auto active = active_manifest();
    const auto checkpoint = prepared_checkpoint();
    const auto prepared =
        prepare_manifest_candidate(active, checkpoint, 300);
    REQUIRE(prepared.ok());
    REQUIRE_EQ(prepared.value().file_offset,
               static_cast<std::uint64_t>(kManifestSlotBytes));
    REQUIRE_EQ(prepared.value().slot.slot_index, 1u);
    REQUIRE_EQ(prepared.value().slot.generation.checkpoint_generation, 6ull);
    REQUIRE_EQ(prepared.value().slot.generation.base_generation,
               active.generation.base_generation);
    REQUIRE_EQ(prepared.value().slot.generation.journal.epoch,
               active.generation.journal.epoch);
    REQUIRE_EQ(prepared.value().slot.generation.journal.committed_length,
               checkpoint.committed_length);
    REQUIRE_EQ(prepared.value().slot.generation.journal.commit,
               checkpoint.commit);
    REQUIRE_EQ(prepared.value().slot.generation.journal.root,
               checkpoint.root);
    const auto decoded = decode_manifest_slot(prepared.value().bytes);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), prepared.value().slot);

    auto wrong_generation = checkpoint;
    wrong_generation.generation = 7;
    REQUIRE(!prepare_manifest_candidate(
        active, wrong_generation, 300).ok());
}

ManifestPublishAttemptOutcome run_publication_case(
    const std::vector<test::FaultFileRule>& rules,
    const bool reject_validation = false,
    test::FaultFileObserver observer = {},
    const OperationContext& context = OperationContext{}) {
    auto active = active_manifest();
    auto checkpoint = prepared_checkpoint();
    auto candidate = prepare_manifest_candidate(active, checkpoint, 300);
    REQUIRE(candidate.ok());
    auto fault = test::make_fault_file(
        kManifestFileBytes, rules, std::move(observer));
    const auto active_bytes = encode_manifest_slot(active);
    REQUIRE(active_bytes.ok());
    REQUIRE(fault.file->write_exact(0, active_bytes.value()).ok());
    auto manifest = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    ManifestPublishRequest request{};
    request.manifest_file = std::move(manifest);
    request.active_slot = active;
    request.candidate = candidate.move_value();
    request.test_validate_candidate =
        [reject_validation](const ManifestSlot&, std::span<const std::byte>)
            -> Result<void> {
            if (reject_validation) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::VerificationFailed),
                    L"test.validator", L"candidate rejected", 0});
            }
            return Result<void>::success();
        };
    auto session = ImageSession::ready();
    auto lease_result = session.begin_checkpoint();
    REQUIRE(lease_result.ok());
    auto lease = lease_result.move_value();
    return attempt_manifest_publication(context, lease, request);
}

TEST_CASE(commit_publish_rejects_mismatched_candidate_bytes_before_attempt) {
    auto active = active_manifest();
    auto candidate = prepare_manifest_candidate(
        active, prepared_checkpoint(), 300);
    REQUIRE(candidate.ok());
    candidate.value().bytes[128] ^= std::byte{0x01};

    auto fault = test::make_fault_file(kManifestFileBytes);
    const auto active_bytes = encode_manifest_slot(active);
    REQUIRE(active_bytes.ok());
    REQUIRE(fault.file->write_exact(0, active_bytes.value()).ok());
    const auto writes_before = fault.state->calls(test::FaultFilePhase::Write);
    auto manifest = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    ManifestPublishRequest request{};
    request.manifest_file = std::move(manifest);
    request.active_slot = active;
    request.candidate = candidate.move_value();
    request.test_validate_candidate =
        [](const ManifestSlot&, std::span<const std::byte>) {
            return Result<void>::success();
        };
    auto session = ImageSession::ready();
    auto lease_result = session.begin_checkpoint();
    REQUIRE(lease_result.ok());
    auto lease = lease_result.move_value();

    const auto outcome = attempt_manifest_publication(
        OperationContext{}, lease, request);
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::BeforeSlotWriteAttempt);
    REQUIRE_EQ(fault.state->calls(test::FaultFilePhase::Write),
               writes_before);
    REQUIRE_EQ(session.snapshot().publication_stage,
               PublicationStage::PrePublication);
}

TEST_CASE(commit_publish_torn_write_is_indeterminate) {
    const auto outcome = run_publication_case({
        {test::FaultFilePhase::Write, 2,
         static_cast<std::uint64_t>(kManifestSlotBytes + 2048),
         test::FaultFileAction::TornWrite, 29}});
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::SlotWriteAttempted);
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommitIndeterminate);
}

TEST_CASE(commit_publish_flush_error_is_indeterminate) {
    const auto outcome = run_publication_case({
        {test::FaultFilePhase::Flush, 1, std::nullopt,
         test::FaultFileAction::FlushError, 5}});
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::ManifestFlushAttempted);
    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommitIndeterminate);
}

TEST_CASE(commit_publish_post_commit_validation_error_needs_reconciliation) {
    const auto outcome = run_publication_case({}, true);
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::PostCommitValidation);
    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommittedNeedsReconciliation);
}

TEST_CASE(commit_publish_validator_exception_is_post_commit) {
    auto active = active_manifest();
    auto candidate = prepare_manifest_candidate(
        active, prepared_checkpoint(), 300);
    REQUIRE(candidate.ok());
    auto fault = test::make_fault_file(kManifestFileBytes);
    const auto active_bytes = encode_manifest_slot(active);
    REQUIRE(active_bytes.ok());
    REQUIRE(fault.file->write_exact(0, active_bytes.value()).ok());
    auto manifest = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    ManifestPublishRequest request{};
    request.manifest_file = std::move(manifest);
    request.active_slot = active;
    request.candidate = candidate.move_value();
    request.test_validate_candidate =
        [](const ManifestSlot&, std::span<const std::byte>)
            -> Result<void> {
            throw std::runtime_error("injected validator exception");
        };
    auto session = ImageSession::ready();
    auto lease_result = session.begin_checkpoint();
    REQUIRE(lease_result.ok());
    auto lease = lease_result.move_value();
    const auto outcome = attempt_manifest_publication(
        OperationContext{}, lease, request);
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::PostCommitValidation);
    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommittedNeedsReconciliation);
}

TEST_CASE(commit_publish_readback_io_error_is_post_commit) {
    const auto outcome = run_publication_case({
        {test::FaultFilePhase::Read, 2, std::nullopt,
         test::FaultFileAction::NativeError, 23}});
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::PostCommitReadback);
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommittedNeedsReconciliation);
}

TEST_CASE(commit_publish_readback_corruption_is_post_commit) {
    const auto outcome = run_publication_case({
        {test::FaultFilePhase::Read, 2,
         static_cast<std::uint64_t>(kManifestSlotBytes + 128),
         test::FaultFileAction::ReadbackCorruption, 0}});
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::PostCommitReadback);
    REQUIRE_EQ(classify_manifest_publish_failure(outcome.boundary),
               CommitDisposition::CommittedNeedsReconciliation);
}

TEST_CASE(commit_publish_ignores_ordinary_cancellation_after_attempt) {
    std::stop_source cancelled_after_write;
    const auto outcome = run_publication_case(
        {}, false,
        [&cancelled_after_write](const test::FaultFilePhase phase,
                                 const std::uint64_t call) {
            if (phase == test::FaultFilePhase::Write && call == 2) {
                cancelled_after_write.request_stop();
            }
        },
        OperationContext{cancelled_after_write.get_token()});
    REQUIRE(cancelled_after_write.stop_requested());
    REQUIRE(outcome.ok());
    REQUIRE_EQ(outcome.boundary,
               ManifestPublishBoundary::PostCommitValidation);
    REQUIRE(outcome.candidate_verified);
}

TEST_CASE(commit_resolver_new_complete_reflushes_and_enters_reconciliation) {
    ImageSession session = ImageSession::ready();
    auto lease = enter_resolution(session, 7001);
    std::vector<std::string> calls;
    CommitResolutionAttemptRequest request{
        5,
        6,
        1,
        [&calls](const OperationContext&)
            -> Result<CommitRecoveryEvidence> {
            calls.emplace_back("recover");
            return Result<CommitRecoveryEvidence>::success({
                CommitRecoveryFinding::NewGenerationComplete,
                attempted_manifest(), false});
        },
        [&calls](const OperationContext&, const ManifestSlot&)
            -> Result<void> {
            calls.emplace_back("reflush");
            return Result<void>::success();
        }};
    const auto outcome = resolve_indeterminate_commit(
        OperationContext{}, lease, request);
    REQUIRE_EQ(outcome.disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE(outcome.recovery_evidence_complete);
    REQUIRE(outcome.manifest_reflushed);
    REQUIRE_EQ(calls.size(), std::size_t{2});
    REQUIRE_EQ(calls[0], std::string{"recover"});
    REQUIRE_EQ(calls[1], std::string{"reflush"});
    REQUIRE(lease.finish(outcome.disposition).ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Reconciling);
    REQUIRE(session.snapshot().first_failure.has_value());
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id,
               7001ull);
}

TEST_CASE(commit_resolver_old_only_aborts_as_not_committed) {
    ImageSession session = ImageSession::ready();
    auto lease = enter_resolution(session, 7002);
    bool reflush_called = false;
    CommitResolutionAttemptRequest request{
        5,
        6,
        1,
        [](const OperationContext&) -> Result<CommitRecoveryEvidence> {
            return Result<CommitRecoveryEvidence>::success({
                CommitRecoveryFinding::OldGenerationOnly,
                active_manifest(), true});
        },
        [&reflush_called](const OperationContext&, const ManifestSlot&)
            -> Result<void> {
            reflush_called = true;
            return Result<void>::success();
        }};
    const auto outcome = resolve_indeterminate_commit(
        OperationContext{}, lease, request);
    REQUIRE_EQ(outcome.disposition, CommitDisposition::NotCommitted);
    REQUIRE(!reflush_called);
    REQUIRE(lease.finish(outcome.disposition).ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id,
               7002ull);
}

TEST_CASE(commit_resolver_ambiguous_faults_without_reflush_or_guess) {
    ImageSession session = ImageSession::ready();
    auto lease = enter_resolution(session, 7003);
    bool reflush_called = false;
    CommitResolutionAttemptRequest request{
        5,
        6,
        1,
        [](const OperationContext&) -> Result<CommitRecoveryEvidence> {
            return Result<CommitRecoveryEvidence>::success({
                CommitRecoveryFinding::Ambiguous, std::nullopt, false});
        },
        [&reflush_called](const OperationContext&, const ManifestSlot&)
            -> Result<void> {
            reflush_called = true;
            return Result<void>::success();
        }};
    const auto outcome = resolve_indeterminate_commit(
        OperationContext{}, lease, request);
    REQUIRE_EQ(outcome.disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(outcome.error.has_value());
    REQUIRE(!reflush_called);
    REQUIRE(lease.finish(
        outcome.disposition,
        indeterminate_failure(7999)).ok());
    REQUIRE_EQ(session.phase(), SessionPhase::PersistenceFaulted);
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id,
               7003ull);
    REQUIRE(!session.begin_commit_resolution().ok());
    const auto closed = session.close(OperationContext{});
    REQUIRE(closed.ok());
    REQUIRE_EQ(closed.value().phase, SessionPhase::Closed);
    REQUIRE(closed.value().first_failure.has_value());
    REQUIRE_EQ(closed.value().first_failure->context.operation_id, 7003ull);
}

TEST_CASE(commit_resolver_reflush_failure_preserves_first_failure_and_faults) {
    ImageSession session = ImageSession::ready();
    auto lease = enter_resolution(session, 7004);
    CommitResolutionAttemptRequest request{
        5,
        6,
        1,
        [](const OperationContext&) -> Result<CommitRecoveryEvidence> {
            return Result<CommitRecoveryEvidence>::success({
                CommitRecoveryFinding::NewGenerationComplete,
                attempted_manifest(), false});
        },
        [](const OperationContext&, const ManifestSlot&) -> Result<void> {
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"test.reflush", L"injected reflush failure", 21});
        }};
    const auto outcome = resolve_indeterminate_commit(
        OperationContext{}, lease, request);
    REQUIRE_EQ(outcome.disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(outcome.recovery_evidence_complete);
    REQUIRE(!outcome.manifest_reflushed);
    REQUIRE(outcome.error.has_value());
    REQUIRE(lease.finish(
        outcome.disposition,
        indeterminate_failure(7998)).ok());
    REQUIRE_EQ(session.phase(), SessionPhase::PersistenceFaulted);
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id,
               7004ull);
}

TEST_CASE(commit_resolver_callback_exceptions_do_not_escape) {
    {
        ImageSession session = ImageSession::ready();
        auto lease = enter_resolution(session, 7005);
        CommitResolutionAttemptRequest request{
            5, 6, 1,
            [](const OperationContext&)
                -> Result<CommitRecoveryEvidence> {
                throw std::runtime_error("injected recovery exception");
            },
            [](const OperationContext&, const ManifestSlot&) {
                return Result<void>::success();
            }};
        const auto outcome = resolve_indeterminate_commit(
            OperationContext{}, lease, request);
        REQUIRE_EQ(outcome.disposition,
                   CommitDisposition::CommitIndeterminate);
        REQUIRE(outcome.error.has_value());
    }
    {
        ImageSession session = ImageSession::ready();
        auto lease = enter_resolution(session, 7006);
        CommitResolutionAttemptRequest request{
            5, 6, 1,
            [](const OperationContext&)
                -> Result<CommitRecoveryEvidence> {
                return Result<CommitRecoveryEvidence>::success({
                    CommitRecoveryFinding::NewGenerationComplete,
                    attempted_manifest(), false});
            },
            [](const OperationContext&, const ManifestSlot&)
                -> Result<void> {
                throw std::runtime_error("injected reflush exception");
            }};
        const auto outcome = resolve_indeterminate_commit(
            OperationContext{}, lease, request);
        REQUIRE_EQ(outcome.disposition,
                   CommitDisposition::CommitIndeterminate);
        REQUIRE(outcome.error.has_value());
    }
}

}  // namespace
}  // namespace myrd::persistence
