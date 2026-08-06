#include "src/persistence/checkpoint_coordinator.h"

#include "src/storage/block_constants.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stop_token>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <typename T>
Result<T> checkpoint_failure(
    std::wstring_view message,
    PersistenceErrorCode code = PersistenceErrorCode::InvalidFormat) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"CheckpointCoordinator::prepare",
        std::wstring{message},
        0});
}

std::optional<JournalRecordType> journal_type(
    const DirtyRecordKind kind) noexcept {
    switch (kind) {
    case DirtyRecordKind::Data: return JournalRecordType::Data;
    case DirtyRecordKind::Zero: return JournalRecordType::Zero;
    case DirtyRecordKind::Discard: return JournalRecordType::Discard;
    }
    return std::nullopt;
}

PersistenceErrorCode persistence_code_for(const Error& error) noexcept {
    if (error.domain == ErrorDomain::persistence) {
        const auto decoded = decode_persistence_error_code(error.code);
        if (decoded.has_value()) return *decoded;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::cancelled)) {
        return PersistenceErrorCode::Cancelled;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::timed_out)) {
        return PersistenceErrorCode::DeadlineExceeded;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::memory_hard_limit)) {
        return PersistenceErrorCode::SnapshotResourceExhausted;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::internal_invariant) ||
        error.domain == ErrorDomain::internal) {
        return PersistenceErrorCode::FatalInternalInvariant;
    }
    return PersistenceErrorCode::IoError;
}

CheckpointPrepareFailure prepare_failure(
    const CheckpointPrepareRequest& request,
    const SessionPhase phase,
    const Error& source) {
    const auto code = persistence_code_for(source);
    auto context = request.correlation_context.value();
    // This public operation owns the failure identity even when the caller's
    // otherwise-complete correlation context fails request validation.
    context.operation = "checkpoint.prepare";
    context.code = code;
    context.phase = phase;
    context.native_code = source.native_code;
    context.disposition = CommitDisposition::NotCommitted;
    context.safe_context = "pre-manifest checkpoint preparation failed";

    Error normalized{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"checkpoint.prepare",
        source.message,
        source.native_code};
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::NotCommitted;
    outcome.generation = context.generation;
    outcome.error = normalized;
    outcome.error_context = context;
    return {std::move(outcome)};
}

void finish_not_committed(
    PublishOperationLease& lease,
    const CheckpointPrepareFailure& failure) noexcept {
    try {
        PersistenceFailure first{
            *failure.outcome.error,
            *failure.outcome.error_context};
        const auto finished = lease.finish(
            CommitDisposition::NotCommitted, std::move(first));
        if (!finished.ok()) (void)lease.abort();
    } catch (...) {
        try {
            (void)lease.abort();
        } catch (...) {
        }
    }
}

void abort_snapshot(
    DynamicBlockStore& store,
    const ConsistentSnapshotToken& token) noexcept {
    try {
        (void)store.abort_consistent_snapshot(token);
    } catch (...) {
    }
}

class SnapshotAbortGuard final {
public:
    SnapshotAbortGuard(
        DynamicBlockStore& store,
        ConsistentSnapshotToken token) noexcept
        : store_(&store), token_(token) {}
    SnapshotAbortGuard(const SnapshotAbortGuard&) = delete;
    SnapshotAbortGuard& operator=(const SnapshotAbortGuard&) = delete;
    ~SnapshotAbortGuard() noexcept { abort_now(); }

    void abort_now() noexcept {
        if (store_ != nullptr) {
            abort_snapshot(*store_, token_);
            store_ = nullptr;
        }
    }
    void release() noexcept { store_ = nullptr; }

private:
    DynamicBlockStore* store_{};
    ConsistentSnapshotToken token_{};
};

}  // namespace

PreparedCheckpointTransaction::PreparedCheckpointTransaction(
    DynamicBlockStore& store,
    ConsistentSnapshotToken snapshot_token,
    PublishOperationLease publish_lease,
    PreparedCheckpoint checkpoint,
    const VolumeBarrierKind volume_barrier,
    CheckpointPrepareMetrics metrics) noexcept
    : store_(&store),
      snapshot_token_(snapshot_token),
      publish_lease_(std::move(publish_lease)),
      checkpoint_(std::move(checkpoint)),
      volume_barrier_(volume_barrier),
      metrics_(metrics) {}

PreparedCheckpointTransaction::PreparedCheckpointTransaction(
    PreparedCheckpointTransaction&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)),
      snapshot_token_(std::move(other.snapshot_token_)),
      publish_lease_(std::move(other.publish_lease_)),
      checkpoint_(std::move(other.checkpoint_)),
      volume_barrier_(other.volume_barrier_),
      metrics_(other.metrics_) {
    other.snapshot_token_.reset();
    other.publish_lease_.reset();
}

PreparedCheckpointTransaction& PreparedCheckpointTransaction::operator=(
    PreparedCheckpointTransaction&& other) noexcept {
    if (this == &other) return *this;
    abandon_noexcept();
    store_ = std::exchange(other.store_, nullptr);
    snapshot_token_ = std::move(other.snapshot_token_);
    publish_lease_ = std::move(other.publish_lease_);
    checkpoint_ = std::move(other.checkpoint_);
    volume_barrier_ = other.volume_barrier_;
    metrics_ = other.metrics_;
    other.snapshot_token_.reset();
    other.publish_lease_.reset();
    return *this;
}

PreparedCheckpointTransaction::~PreparedCheckpointTransaction() {
    abandon_noexcept();
}

Result<void> PreparedCheckpointTransaction::abort() {
    std::optional<Error> first_error;
    if (store_ != nullptr && snapshot_token_.has_value()) {
        auto aborted = store_->abort_consistent_snapshot(*snapshot_token_);
        if (!aborted.ok()) first_error = aborted.error();
        snapshot_token_.reset();
    }
    if (publish_lease_.has_value()) {
        auto aborted = publish_lease_->abort();
        if (!aborted.ok() && !first_error.has_value()) {
            first_error = aborted.error();
        }
        publish_lease_.reset();
    }
    store_ = nullptr;
    if (first_error.has_value()) {
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

void PreparedCheckpointTransaction::abandon_noexcept() noexcept {
    try {
        (void)abort();
    } catch (...) {
        snapshot_token_.reset();
        publish_lease_.reset();
        store_ = nullptr;
    }
}

CheckpointPrepareOutcome CheckpointCoordinator::prepare(
    const OperationContext& context,
    const CheckpointPrepareRequest& request) {
    // The barrier is deliberately the first validation: an omitted or forged
    // marker cannot begin either the session operation or the M2 snapshot.
    if (!is_valid_barrier_for_checkpoint(request.volume_barrier)) {
        const auto error = checkpoint_failure<void>(
            L"volume barrier evidence is missing or unknown").error();
        return CheckpointPrepareOutcome{
            prepare_failure(request, session_->phase(), error)};
    }
    auto expected_target_generation = request.journal.previous_generation;
    const bool target_generation_valid =
        expected_target_generation < kInvalidGeneration - 1;
    if (target_generation_valid) ++expected_target_generation;
    const auto& correlation = request.correlation_context.value();
    if (!request.journal.blocks.empty() ||
        correlation.operation != "checkpoint.prepare" ||
        correlation.operation_id != request.journal.operation_id ||
        correlation.image_uuid != request.journal.header.image_uuid ||
        !target_generation_valid ||
        correlation.generation != expected_target_generation ||
        correlation.disposition != CommitDisposition::NotCommitted ||
        request.snapshot_batch_bytes < kBlockSizeBytes ||
        request.snapshot_batch_bytes >
            store_->options().max_snapshot_batch_bytes ||
        request.journal.header.geometry.virtual_bytes !=
            store_->size_bytes() ||
        request.journal.header.geometry.logical_sector_size !=
            store_->sector_size() ||
        request.journal.previous_generation !=
            store_->backing_generation()) {
        const auto error = checkpoint_failure<void>(
            L"checkpoint request does not match the live block store")
                               .error();
        return CheckpointPrepareOutcome{
            prepare_failure(request, session_->phase(), error)};
    }

    auto lease_result = session_->begin_checkpoint();
    if (!lease_result.ok()) {
        return CheckpointPrepareOutcome{prepare_failure(
            request, session_->phase(), lease_result.error())};
    }
    auto lease = lease_result.move_value();

    std::stop_source combined_stop;
    const auto request_stop = [&combined_stop]() noexcept {
        combined_stop.request_stop();
    };
    std::stop_callback caller_stop(context.stop_token, request_stop);
    std::stop_callback session_stop(lease.stop_token(), request_stop);
    OperationContext operation_context{
        combined_stop.get_token(), context.deadline};

    auto token_result =
        store_->begin_consistent_snapshot(operation_context);
    if (!token_result.ok()) {
        auto failure = prepare_failure(
            request, SessionPhase::Checkpointing, token_result.error());
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }
    const auto token = token_result.value();
    // From this point until ownership is transferred into the prepared
    // transaction, every return and every C++ exception must retire the cut.
    SnapshotAbortGuard snapshot_guard{*store_, token};
    if (token.source_backing_generation !=
        request.journal.previous_generation) {
        const auto error = checkpoint_failure<void>(
            L"snapshot source generation changed before the consistency cut",
            PersistenceErrorCode::GenerationConflict).error();
        auto failure = prepare_failure(
            request, SessionPhase::Checkpointing, error);
        snapshot_guard.abort_now();
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }

    CheckpointPrepareMetrics metrics{};
    DirtyScanCursor cursor{};
    JournalCheckpointBatchProducer producer =
        [&](const OperationContext& producer_context,
            const JournalCheckpointBatchConsumer& consume) -> Result<void> {
        while (cursor.next_block_index < store_->total_blocks()) {
            const auto before = cursor.next_block_index;
            auto copied = store_->copy_consistent_snapshot_batch(
                producer_context, token, cursor,
                request.snapshot_batch_bytes);
            if (!copied.ok()) {
                return Result<void>::failure(copied.error());
            }
            ++metrics.snapshot_batches;
            metrics.scanned_blocks = cursor.next_block_index;
            if (copied.value().empty()) {
                if (cursor.next_block_index <= before) {
                    return checkpoint_failure<void>(
                        L"snapshot scan made no progress",
                        PersistenceErrorCode::FatalInternalInvariant);
                }
                continue;
            }

            std::vector<JournalCheckpointBlockInput> inputs;
            std::uint64_t payload_bytes = 0;
            try {
                inputs.reserve(copied.value().size());
                for (const auto& block : copied.value()) {
                    const auto type = journal_type(block.kind);
                    if (!type.has_value() || block.version == 0 ||
                        block.dirty_epoch == 0 ||
                        block.dirty_epoch > token.cutoff_dirty_epoch ||
                        block.base_generation >
                            token.source_backing_generation) {
                        return checkpoint_failure<void>(
                            L"snapshot record identity is invalid",
                            PersistenceErrorCode::FatalInternalInvariant);
                    }
                    if (block.data.size() >
                        (std::numeric_limits<std::uint64_t>::max)() -
                            payload_bytes) {
                        return checkpoint_failure<void>(
                            L"snapshot payload accounting overflowed",
                            PersistenceErrorCode::FatalInternalInvariant);
                    }
                    payload_bytes += block.data.size();
                    inputs.push_back({
                        *type,
                        block.block_index,
                        block.version,
                        block.base_generation,
                        block.dirty_epoch,
                        block.data,
                    });
                }
            } catch (const std::bad_alloc&) {
                return checkpoint_failure<void>(
                    L"allocating checkpoint batch mapping failed",
                    PersistenceErrorCode::SnapshotResourceExhausted);
            }
            if (payload_bytes > request.snapshot_batch_bytes) {
                return checkpoint_failure<void>(
                    L"snapshot batch exceeded its payload limit",
                    PersistenceErrorCode::FatalInternalInvariant);
            }
            metrics.snapshot_records += inputs.size();
            metrics.peak_batch_records = (std::max)(
                metrics.peak_batch_records,
                static_cast<std::uint64_t>(inputs.size()));
            metrics.peak_batch_payload_bytes = (std::max)(
                metrics.peak_batch_payload_bytes, payload_bytes);
            auto consumed = consume(inputs);
            if (!consumed.ok()) return consumed;
            // copied/inputs are iteration-local. Their payloads cease to be
            // retained immediately after consume() returns.
        }
        return Result<void>::success();
    };

    auto prepared = journal_writer_->prepare_streaming(
        operation_context, request.journal, producer);
    if (!prepared.ok()) {
        auto failure = prepare_failure(
            request, SessionPhase::Checkpointing, prepared.error());
        snapshot_guard.abort_now();
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }
    if (store_->backing_generation() != token.source_backing_generation) {
        const auto error = checkpoint_failure<void>(
            L"backing generation changed while preparing the checkpoint",
            PersistenceErrorCode::GenerationConflict).error();
        auto failure = prepare_failure(
            request, SessionPhase::Checkpointing, error);
        snapshot_guard.abort_now();
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }

    PreparedCheckpointTransaction transaction{
        *store_, token, std::move(lease), prepared.move_value(),
        *request.volume_barrier, metrics};
    snapshot_guard.release();
    return CheckpointPrepareOutcome{std::move(transaction)};
}

}  // namespace myrd::persistence
