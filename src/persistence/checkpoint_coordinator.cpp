#include "src/persistence/checkpoint_coordinator.h"

#include "src/persistence/image_paths.h"
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
    ImageSession& session,
    DynamicBlockStore& store,
    ConsistentSnapshotToken snapshot_token,
    PublishOperationLease publish_lease,
    PreparedCheckpoint checkpoint,
    CompletePersistenceErrorContext correlation_context,
    const VolumeBarrierKind volume_barrier,
    CheckpointPrepareMetrics metrics) noexcept
    : store_(&store),
      session_(&session),
      snapshot_token_(snapshot_token),
      publish_lease_(std::move(publish_lease)),
      checkpoint_(std::move(checkpoint)),
      correlation_context_(std::move(correlation_context)),
      volume_barrier_(volume_barrier),
      metrics_(metrics) {}

PreparedCheckpointTransaction::PreparedCheckpointTransaction(
    PreparedCheckpointTransaction&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)),
      session_(std::exchange(other.session_, nullptr)),
      snapshot_token_(std::move(other.snapshot_token_)),
      publish_lease_(std::move(other.publish_lease_)),
      manifest_file_(std::move(other.manifest_file_)),
      checkpoint_(std::move(other.checkpoint_)),
      correlation_context_(std::move(other.correlation_context_)),
      disposition_(other.disposition_),
      manifest_candidate_verified_(other.manifest_candidate_verified_),
      publication_identity_(std::move(other.publication_identity_)),
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
    session_ = std::exchange(other.session_, nullptr);
    snapshot_token_ = std::move(other.snapshot_token_);
    publish_lease_ = std::move(other.publish_lease_);
    manifest_file_ = std::move(other.manifest_file_);
    checkpoint_ = std::move(other.checkpoint_);
    correlation_context_ = std::move(other.correlation_context_);
    disposition_ = other.disposition_;
    manifest_candidate_verified_ = other.manifest_candidate_verified_;
    publication_identity_ = std::move(other.publication_identity_);
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
    if (disposition_ != CommitDisposition::NotCommitted) {
        return checkpoint_failure<void>(
            L"a published transaction requires resolution or reconciliation",
            PersistenceErrorCode::OperationBusy);
    }
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
    session_ = nullptr;
    manifest_file_.reset();
    if (first_error.has_value()) {
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

CheckpointTransactionOutcome PreparedCheckpointTransaction::publish_manifest(
    const OperationContext& context,
    CheckpointManifestPublishRequest request) {
    auto build_outcome = [&](const CommitDisposition disposition,
                             const bool verified,
                             std::optional<Error> source)
        -> CheckpointTransactionOutcome {
        CheckpointTransactionOutcome result{};
        result.disposition = disposition;
        result.generation = checkpoint_.generation;
        result.manifest_candidate_verified = verified;
        if (source.has_value()) {
            auto code =
                disposition == CommitDisposition::CommitIndeterminate
                    ? PersistenceErrorCode::CommitIndeterminate
                    : persistence_code_for(*source);
            if (disposition ==
                    CommitDisposition::CommittedNeedsReconciliation &&
                (code == PersistenceErrorCode::Cancelled ||
                 code == PersistenceErrorCode::DeadlineExceeded)) {
                // Cancellation cannot undo a successful manifest flush.
                code = PersistenceErrorCode::VerificationFailed;
            }
            auto error_context = correlation_context_.value();
            error_context.operation = "checkpoint.publish";
            error_context.code = code;
            error_context.phase =
                disposition == CommitDisposition::CommitIndeterminate
                    ? SessionPhase::ResolvingCommit
                    : disposition ==
                          CommitDisposition::CommittedNeedsReconciliation
                        ? SessionPhase::Reconciling
                        : SessionPhase::Checkpointing;
            error_context.native_code = source->native_code;
            error_context.generation = checkpoint_.generation;
            error_context.disposition = disposition;
            error_context.safe_context =
                "manifest checkpoint publication failed";
            result.error = Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(code),
                L"checkpoint.publish", source->message,
                source->native_code};
            result.error_context = std::move(error_context);
        }
        return result;
    };

    if (store_ == nullptr || session_ == nullptr ||
        !snapshot_token_.has_value() || !publish_lease_.has_value() ||
        disposition_ != CommitDisposition::NotCommitted) {
        return build_outcome(
            disposition_, manifest_candidate_verified_,
            checkpoint_failure<void>(
                L"prepared checkpoint transaction is not publishable",
                PersistenceErrorCode::OperationBusy).error());
    }

    manifest_file_ = std::shared_ptr<IPersistenceFile>(
        std::move(request.manifest_file));
    const auto prepared = prepare_manifest_candidate(
        request.active_slot, checkpoint_, request.committed_time_100ns);
    ManifestPublishAttemptOutcome attempted{};
    if (!prepared.ok()) {
        attempted.error = prepared.error();
    } else {
        publication_identity_ = FrozenPublicationIdentity{
            request.active_slot.generation.checkpoint_generation,
            checkpoint_.generation,
            prepared.value().slot.slot_index};
        ManifestPublishRequest publish_request{};
        publish_request.manifest_file = manifest_file_;
        publish_request.active_slot = request.active_slot;
        publish_request.candidate = prepared.value();
        publish_request.complete_validation = request.complete_validation;
        publish_request.record_durable_commit = [this]() {
            disposition_ = CommitDisposition::Committed;
            return Result<void>::success();
        };
#if defined(MYRD_ENABLE_TEST_SEAMS)
        publish_request.test_validate_candidate =
            std::move(request.test_validate_candidate);
#endif
        attempted = attempt_manifest_publication(
            context, *publish_lease_, publish_request);
    }
    const auto final_disposition = attempted.ok()
        ? CommitDisposition::Committed
        : classify_manifest_publish_failure(attempted.boundary);
    auto result = build_outcome(
        final_disposition, attempted.candidate_verified, attempted.error);

    if (!attempted.ok()) {
        std::optional<PersistenceFailure> failure;
        if (result.error.has_value() && result.error_context.has_value()) {
            failure = PersistenceFailure{*result.error, *result.error_context};
        }
        auto finished = publish_lease_->finish(
            final_disposition, std::move(failure));
        if (!finished.ok()) {
            // The lease destructor will still fail closed according to the
            // already-recorded publication stage. Never downgrade facts.
            result = build_outcome(
                final_disposition, attempted.candidate_verified,
                finished.error());
        } else {
            publish_lease_.reset();
        }
    }
    disposition_ = final_disposition;
    manifest_candidate_verified_ = attempted.candidate_verified;

    if (final_disposition == CommitDisposition::NotCommitted) {
        if (snapshot_token_.has_value()) {
            auto aborted = store_->abort_consistent_snapshot(*snapshot_token_);
            if (!aborted.ok() && !result.error.has_value()) {
                result = build_outcome(
                    final_disposition, false, aborted.error());
            }
            snapshot_token_.reset();
        }
        store_ = nullptr;
        session_ = nullptr;
        if (manifest_file_) {
            (void)manifest_file_->close();
            manifest_file_.reset();
        }
    }
    return result;
}

CheckpointTransactionOutcome PreparedCheckpointTransaction::resolve_commit(
    const OperationContext& context,
    const CommitResolutionRequest& request) {
    auto result = CheckpointTransactionOutcome{};
    result.disposition = disposition_;
    result.generation = checkpoint_.generation;
    result.manifest_candidate_verified = manifest_candidate_verified_;
    const auto set_error = [&](const Error& source,
                               const CommitDisposition disposition) {
        auto code = disposition == CommitDisposition::CommitIndeterminate
            ? PersistenceErrorCode::CommitIndeterminate
            : persistence_code_for(source);
        if (disposition ==
                CommitDisposition::CommittedNeedsReconciliation &&
            (code == PersistenceErrorCode::Cancelled ||
             code == PersistenceErrorCode::DeadlineExceeded)) {
            code = PersistenceErrorCode::VerificationFailed;
        }
        auto error_context = correlation_context_.value();
        error_context.operation = "checkpoint.resolve";
        error_context.code = code;
        error_context.phase =
            disposition ==
                    CommitDisposition::CommittedNeedsReconciliation
                ? SessionPhase::Reconciling
                : SessionPhase::ResolvingCommit;
        error_context.native_code = source.native_code;
        error_context.generation = checkpoint_.generation;
        error_context.disposition = disposition;
        error_context.safe_context = "commit resolution failed closed";
        result.error = Error{
            ErrorDomain::persistence, static_cast<std::uint32_t>(code),
            L"checkpoint.resolve", source.message, source.native_code};
        result.error_context = std::move(error_context);
    };
    if (store_ == nullptr || session_ == nullptr ||
        !snapshot_token_.has_value() || publish_lease_.has_value() ||
        !publication_identity_.has_value() ||
        disposition_ != CommitDisposition::CommitIndeterminate) {
        set_error(checkpoint_failure<void>(
            L"transaction is not waiting for commit resolution",
            PersistenceErrorCode::OperationBusy).error(), disposition_);
        return result;
    }
    auto lease_result = session_->begin_commit_resolution();
    if (!lease_result.ok()) {
        set_error(lease_result.error(), disposition_);
        return result;
    }
    if (manifest_file_) {
        const auto closed = manifest_file_->close();
        manifest_file_.reset();
        if (!closed.ok()) {
            set_error(closed.error(), disposition_);
            return result;
        }
    }
    auto lease = lease_result.move_value();
    CommitResolutionOutcome resolved{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (request.test_recover && request.test_reflush_manifest) {
        resolved = resolve_indeterminate_commit(
            context, lease,
            CommitResolutionAttemptRequest{
                publication_identity_->old_generation(),
                publication_identity_->attempted_generation(),
                publication_identity_->attempted_slot_index(),
                request.test_recover, request.test_reflush_manifest});
    } else
#endif
    {
        resolved = resolve_indeterminate_commit_from_disk(
            context, lease,
            request.filesystem.value_or(FilesystemCommitResolution{}),
            *publication_identity_);
    }
    result.disposition = resolved.disposition;
    result.manifest_candidate_verified =
        resolved.recovery_evidence_complete &&
        resolved.disposition ==
            CommitDisposition::CommittedNeedsReconciliation;
    std::optional<PersistenceFailure> failure;
    if (resolved.error.has_value()) {
        set_error(*resolved.error, resolved.disposition);
        failure = PersistenceFailure{*result.error, *result.error_context};
    }
    auto finished = lease.finish(resolved.disposition, std::move(failure));
    if (!finished.ok() && !result.error.has_value()) {
        set_error(finished.error(), resolved.disposition);
    }
    disposition_ = resolved.disposition;
    manifest_candidate_verified_ = result.manifest_candidate_verified;
    if (resolved.disposition == CommitDisposition::NotCommitted) {
        auto aborted = store_->abort_consistent_snapshot(*snapshot_token_);
        if (!aborted.ok() && !result.error.has_value()) {
            set_error(aborted.error(), resolved.disposition);
        }
        snapshot_token_.reset();
        store_ = nullptr;
        session_ = nullptr;
    }
    return result;
}

void PreparedCheckpointTransaction::abandon_noexcept() noexcept {
    try {
        if (disposition_ == CommitDisposition::NotCommitted) {
            (void)abort();
            return;
        }
        // A published transaction may not be rolled back, but its in-memory
        // snapshot cut can always be retired without acknowledging any dirty
        // member. Destroying an active lease then moves the session to the
        // fail-closed phase implied by its publication stage.
        if (store_ != nullptr && snapshot_token_.has_value()) {
            abort_snapshot(*store_, *snapshot_token_);
            snapshot_token_.reset();
        }
        publish_lease_.reset();
        if (manifest_file_) {
            (void)manifest_file_->close();
            manifest_file_.reset();
        }
        store_ = nullptr;
        session_ = nullptr;
    } catch (...) {
        snapshot_token_.reset();
        publish_lease_.reset();
        manifest_file_.reset();
        store_ = nullptr;
        session_ = nullptr;
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
        *session_, *store_, token, std::move(lease), prepared.move_value(),
        request.correlation_context,
        *request.volume_barrier, metrics};
    snapshot_guard.release();
    return CheckpointPrepareOutcome{std::move(transaction)};
}

}  // namespace myrd::persistence
