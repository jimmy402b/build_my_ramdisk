#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/commit_resolver.h"
#include "src/persistence/image_session.h"
#include "src/persistence/journal_writer.h"
#include "src/storage/dynamic_block_store.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace myrd::persistence {

struct CheckpointPrepareRequest final {
    explicit CheckpointPrepareRequest(
        CompletePersistenceErrorContext correlation) noexcept
        : correlation_context(std::move(correlation)) {}

    // Independent validated correlation identity. Failure reporting never
    // derives mandatory context from the journal payload being validated.
    CompletePersistenceErrorContext correlation_context;
    // Evidence supplied by the caller after it completed the corresponding
    // volume barrier. Missing/unknown values fail before snapshot begin.
    std::optional<VolumeBarrierKind> volume_barrier{};
    JournalCheckpointRequest journal{};
    std::size_t snapshot_batch_bytes{0};
};

struct CheckpointPrepareMetrics final {
    std::uint64_t snapshot_batches{0};
    std::uint64_t snapshot_records{0};
    std::uint64_t scanned_blocks{0};
    std::uint64_t peak_batch_records{0};
    std::uint64_t peak_batch_payload_bytes{0};
};

// Move-only handoff between Task30 (journal Prepared), Task31 (manifest
// publication), and Task32 (install/ack/finish). Until a later task completes
// it, destruction conservatively aborts the M2 snapshot and session operation.
// The referenced DynamicBlockStore and ImageSession must outlive this object.
// Lifecycle shutdown must abort or hand off an active transaction before the
// same thread waits in ImageSession::close().
class PreparedCheckpointTransaction final {
public:
    PreparedCheckpointTransaction() = delete;
    PreparedCheckpointTransaction(
        const PreparedCheckpointTransaction&) = delete;
    PreparedCheckpointTransaction& operator=(
        const PreparedCheckpointTransaction&) = delete;
    PreparedCheckpointTransaction(PreparedCheckpointTransaction&&) noexcept;
    PreparedCheckpointTransaction& operator=(
        PreparedCheckpointTransaction&&) noexcept;
    ~PreparedCheckpointTransaction();

    [[nodiscard]] const PreparedCheckpoint& checkpoint() const noexcept {
        return checkpoint_;
    }
    [[nodiscard]] bool active() const noexcept {
        return store_ != nullptr && snapshot_token_.has_value();
    }
    [[nodiscard]] VolumeBarrierKind volume_barrier() const noexcept {
        return volume_barrier_;
    }
    [[nodiscard]] bool filesystem_durability_claimed() const noexcept {
        return permits_filesystem_durability_claim(volume_barrier_);
    }
    [[nodiscard]] CommitDisposition disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] const CheckpointPrepareMetrics& metrics() const noexcept {
        return metrics_;
    }
    Result<void> abort();
    [[nodiscard]] CheckpointTransactionOutcome publish_manifest(
        const OperationContext& context,
        CheckpointManifestPublishRequest request);
    [[nodiscard]] CheckpointTransactionOutcome resolve_commit(
        const OperationContext& context,
        const CommitResolutionRequest& request);

private:
    friend class CheckpointCoordinator;
    PreparedCheckpointTransaction(
        ImageSession& session,
        DynamicBlockStore& store,
        ConsistentSnapshotToken snapshot_token,
        PublishOperationLease publish_lease,
        PreparedCheckpoint checkpoint,
        CompletePersistenceErrorContext correlation_context,
        VolumeBarrierKind volume_barrier,
        CheckpointPrepareMetrics metrics) noexcept;

    void abandon_noexcept() noexcept;

    DynamicBlockStore* store_{nullptr};
    ImageSession* session_{nullptr};
    std::optional<ConsistentSnapshotToken> snapshot_token_{};
    std::optional<PublishOperationLease> publish_lease_{};
    std::shared_ptr<IPersistenceFile> manifest_file_{};
    PreparedCheckpoint checkpoint_{};
    CompletePersistenceErrorContext correlation_context_;
    CommitDisposition disposition_{CommitDisposition::NotCommitted};
    bool manifest_candidate_verified_{};
    std::optional<FrozenPublicationIdentity> publication_identity_{};
    VolumeBarrierKind volume_barrier_{
        VolumeBarrierKind::RawBlockTestNoFilesystemCache};
    CheckpointPrepareMetrics metrics_{};
};

struct CheckpointPrepareFailure final {
    // Task30 failures are always explicit NotCommitted outcomes with a frozen
    // operation-scoped first error and complete persistence context.
    CheckpointOutcome outcome{};
};

class CheckpointPrepareOutcome final {
public:
    CheckpointPrepareOutcome(const CheckpointPrepareOutcome&) = delete;
    CheckpointPrepareOutcome& operator=(const CheckpointPrepareOutcome&) =
        delete;
    CheckpointPrepareOutcome(CheckpointPrepareOutcome&&) noexcept = default;
    CheckpointPrepareOutcome& operator=(CheckpointPrepareOutcome&&) noexcept =
        default;

    [[nodiscard]] bool prepared() const noexcept {
        return std::holds_alternative<PreparedCheckpointTransaction>(result_);
    }
    [[nodiscard]] bool ok() const noexcept { return prepared(); }
    [[nodiscard]] CommitDisposition disposition() const noexcept {
        return prepared()
            ? CommitDisposition::NotCommitted
            : std::get<CheckpointPrepareFailure>(result_)
                  .outcome.disposition;
    }
    [[nodiscard]] const PreparedCheckpointTransaction* transaction()
        const noexcept {
        return std::get_if<PreparedCheckpointTransaction>(&result_);
    }
    PreparedCheckpointTransaction&& move_transaction() {
        return std::move(
            std::get<PreparedCheckpointTransaction>(result_));
    }
    [[nodiscard]] const CheckpointPrepareFailure* failure() const noexcept {
        return std::get_if<CheckpointPrepareFailure>(&result_);
    }
    [[nodiscard]] const Error* error() const noexcept {
        const auto* failed = failure();
        return failed != nullptr && failed->outcome.error.has_value()
            ? &*failed->outcome.error : nullptr;
    }

private:
    friend class CheckpointCoordinator;
    explicit CheckpointPrepareOutcome(
        PreparedCheckpointTransaction transaction) noexcept
        : result_(std::in_place_type<PreparedCheckpointTransaction>,
                  std::move(transaction)) {}
    explicit CheckpointPrepareOutcome(
        CheckpointPrepareFailure failure) noexcept
        : result_(std::in_place_type<CheckpointPrepareFailure>,
                  std::move(failure)) {}

    std::variant<CheckpointPrepareFailure,
                 PreparedCheckpointTransaction> result_;
};

class CheckpointCoordinator final {
public:
    CheckpointCoordinator(ImageSession& session,
                          DynamicBlockStore& store,
                          JournalWriter& journal_writer) noexcept
        : session_(&session), store_(&store), journal_writer_(&journal_writer) {}

    CheckpointCoordinator(const CheckpointCoordinator&) = delete;
    CheckpointCoordinator& operator=(const CheckpointCoordinator&) = delete;

    CheckpointPrepareOutcome prepare(
        const OperationContext& context,
        const CheckpointPrepareRequest& request);

private:
    ImageSession* session_;
    DynamicBlockStore* store_;
    JournalWriter* journal_writer_;
};

}  // namespace myrd::persistence
