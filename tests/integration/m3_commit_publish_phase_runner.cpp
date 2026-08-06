#include "src/persistence/commit_resolver.h"
#include "tests/support/fault_file.h"

#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace myrd::persistence {
namespace {

ManifestSlot active_manifest() {
    ManifestSlot slot{};
    slot.slot_index = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        slot.image_uuid.bytes[index] = static_cast<std::byte>(0x10U + index);
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

PreparedCheckpoint checkpoint() {
    PreparedCheckpoint value{};
    value.generation = 6;
    value.root = {0x7000, 0x1000, 0x89ABCDEF};
    value.commit = {0x8000, 0x1000, 0x10203040, {}};
    for (std::size_t index = 0; index < value.chain_digest.size(); ++index) {
        value.chain_digest[index] = static_cast<std::byte>(0x50U + index);
    }
    value.commit.chain_digest = value.chain_digest;
    value.committed_length = 0x9000;
    return value;
}

struct Run final {
    ManifestPublishAttemptOutcome outcome{};
    std::shared_ptr<test::FaultFileState> state{};
    CommitDisposition session_disposition{CommitDisposition::NotCommitted};
};

Run execute(const std::vector<test::FaultFileRule>& rules,
            const bool cancel_before = false,
            const std::optional<ManifestPublishTestPhase> fail_phase =
                std::nullopt) {
    const auto active = active_manifest();
    auto candidate = prepare_manifest_candidate(active, checkpoint(), 300);
    if (!candidate.ok()) return {};
    auto fault = test::make_fault_file(kManifestFileBytes, rules);
    const auto active_bytes = encode_manifest_slot(active);
    if (!active_bytes.ok() ||
        !fault.file->write_exact(0, active_bytes.value()).ok()) {
        return {};
    }
    auto state = fault.state;
    auto file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    ManifestPublishRequest request{};
    request.manifest_file = std::move(file);
    request.active_slot = active;
    request.candidate = candidate.move_value();
    request.test_validate_candidate =
        [](const ManifestSlot&, std::span<const std::byte>) {
            return Result<void>::success();
        };
    request.test_phase_hook =
        [fail_phase](const ManifestPublishTestPhase phase) {
            if (phase != fail_phase) return Result<void>::success();
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"m3_commit_publish_phase_runner",
                L"injected phase boundary", 0});
        };
    ImageSession session = ImageSession::ready();
    auto lease_result = session.begin_checkpoint();
    if (!lease_result.ok()) return {};
    auto lease = lease_result.move_value();
    std::stop_source stop;
    if (cancel_before) stop.request_stop();
    auto outcome = attempt_manifest_publication(
        OperationContext{stop.get_token()}, lease, request);
    return {std::move(outcome), std::move(state),
            session.snapshot().disposition};
}

bool verify_phase(const std::string_view phase) {
    if (phase == "manifest.slot_write.before") {
        const auto run = execute({}, true);
        return run.outcome.boundary ==
                   ManifestPublishBoundary::BeforeSlotWriteAttempt &&
               classify_manifest_publish_failure(run.outcome.boundary) ==
                   CommitDisposition::NotCommitted &&
               run.state->calls(test::FaultFilePhase::Write) == 1;
    }
    if (phase == "manifest.slot_write.short") {
        const auto run = execute({
            {test::FaultFilePhase::Write, 2,
             static_cast<std::uint64_t>(kManifestSlotBytes + 2048),
             test::FaultFileAction::ShortWrite, 0},
            {test::FaultFilePhase::Write, 3, std::nullopt,
             test::FaultFileAction::ZeroProgress, 0}});
        return run.outcome.boundary ==
                   ManifestPublishBoundary::SlotWriteAttempted &&
               classify_manifest_publish_failure(run.outcome.boundary) ==
                   CommitDisposition::CommitIndeterminate;
    }
    if (phase == "manifest.slot_write.torn") {
        const auto run = execute({
            {test::FaultFilePhase::Write, 2,
             static_cast<std::uint64_t>(kManifestSlotBytes + 2048),
             test::FaultFileAction::TornWrite, 29}});
        return run.outcome.boundary ==
                   ManifestPublishBoundary::SlotWriteAttempted &&
               run.state->calls(test::FaultFilePhase::Flush) == 0;
    }
    if (phase == "manifest.slot_write.after") {
        const auto run = execute(
            {}, false, ManifestPublishTestPhase::AfterSlotWrite);
        return run.outcome.boundary ==
                   ManifestPublishBoundary::SlotWriteAttempted &&
               run.state->calls(test::FaultFilePhase::Write) == 2 &&
               run.state->calls(test::FaultFilePhase::Flush) == 0;
    }
    if (phase == "manifest.flush.before") {
        const auto run = execute(
            {}, false, ManifestPublishTestPhase::BeforeFlush);
        return run.outcome.boundary ==
                   ManifestPublishBoundary::ManifestFlushAttempted &&
               run.state->calls(test::FaultFilePhase::Flush) == 0 &&
               run.session_disposition ==
                   CommitDisposition::CommitIndeterminate;
    }
    if (phase == "manifest.flush.error") {
        const auto run = execute({
            {test::FaultFilePhase::Flush, 1, std::nullopt,
             test::FaultFileAction::FlushError, 1117}});
        return run.outcome.boundary ==
                   ManifestPublishBoundary::ManifestFlushAttempted &&
               run.state->calls(test::FaultFilePhase::Write) == 2 &&
               classify_manifest_publish_failure(run.outcome.boundary) ==
                   CommitDisposition::CommitIndeterminate;
    }
    if (phase == "manifest.flush.after") {
        const auto run = execute(
            {}, false, ManifestPublishTestPhase::AfterFlush);
        return run.outcome.boundary ==
                   ManifestPublishBoundary::ManifestFlushSucceeded &&
               manifest_commit_proven(run.outcome.boundary) &&
               run.state->calls(test::FaultFilePhase::Flush) == 1 &&
               run.state->calls(test::FaultFilePhase::Read) == 1 &&
               run.session_disposition == CommitDisposition::Committed;
    }
    if (phase == "manifest.readback.before") {
        const auto run = execute({
            {test::FaultFilePhase::Read, 2,
             static_cast<std::uint64_t>(kManifestSlotBytes + 2048),
             test::FaultFileAction::ShortRead, 0},
            {test::FaultFilePhase::Read, 3, std::nullopt,
             test::FaultFileAction::ZeroProgress, 0}});
        return run.outcome.boundary ==
                   ManifestPublishBoundary::PostCommitReadback &&
               manifest_commit_proven(run.outcome.boundary) &&
               run.state->calls(test::FaultFilePhase::Read) == 3;
    }
    if (phase == "manifest.readback.corrupt") {
        const auto run = execute({
            {test::FaultFilePhase::Read, 2,
             static_cast<std::uint64_t>(kManifestSlotBytes + 128),
             test::FaultFileAction::ReadbackCorruption, 0}});
        return run.outcome.boundary ==
                   ManifestPublishBoundary::PostCommitReadback &&
               classify_manifest_publish_failure(run.outcome.boundary) ==
                   CommitDisposition::CommittedNeedsReconciliation;
    }
    if (phase == "manifest.readback.after") {
        const auto run = execute({});
        return run.outcome.ok() && run.outcome.candidate_verified;
    }
    return false;
}

}  // namespace
}  // namespace myrd::persistence

int main(const int argc, const char* const* argv) {
    if (argc != 2 || !myrd::persistence::verify_phase(argv[1])) {
        std::cerr << "manifest phase verification failed" << std::endl;
        return 1;
    }
    return 0;
}
