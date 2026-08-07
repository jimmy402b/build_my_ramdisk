#include "src/persistence/checkpoint_coordinator.h"

#include "src/persistence/composite_backing_store.h"
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
    const Error& source,
    const bool compaction) {
    const auto code = persistence_code_for(source);
    auto context = request.correlation_context.value();
    // This public operation owns the failure identity even when the caller's
    // otherwise-complete correlation context fails request validation.
    context.operation = compaction
        ? "compaction.prepare" : "checkpoint.prepare";
    context.code = code;
    context.phase = phase;
    context.native_code = source.native_code;
    context.disposition = CommitDisposition::NotCommitted;
    context.safe_context = compaction
        ? "pre-manifest compaction preparation failed"
        : "pre-manifest checkpoint preparation failed";

    Error normalized{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        compaction ? L"compaction.prepare" : L"checkpoint.prepare",
        source.message,
        source.native_code};
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::NotCommitted;
    outcome.generation = context.generation;
    outcome.error = normalized;
    outcome.error_context = context;
    return {std::move(outcome)};
}

Result<void> run_transaction_observer(
    const std::function<Result<void>()>& observer,
    const wchar_t* exception_message) {
    if (!observer) return Result<void>::success();
    try {
        return observer();
    } catch (...) {
        return checkpoint_failure<void>(
            exception_message,
            PersistenceErrorCode::FatalInternalInvariant);
    }
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
    std::shared_ptr<IPersistenceFile> journal_file,
    JournalHeader journal_header,
    WindowsFileIdentity journal_file_identity,
    const std::uint64_t previous_committed_length,
    CompletePersistenceErrorContext correlation_context,
    const VolumeBarrierKind volume_barrier,
    CheckpointPrepareMetrics metrics,
    CheckpointTransactionObservers observers,
    std::optional<ManifestArtifactReplacement> artifact_replacement) noexcept
    : store_(&store),
      session_(&session),
      snapshot_token_(snapshot_token),
      publish_lease_(std::move(publish_lease)),
      journal_file_(std::move(journal_file)),
      checkpoint_(std::move(checkpoint)),
      journal_header_(std::move(journal_header)),
      journal_file_identity_(std::move(journal_file_identity)),
      previous_committed_length_(previous_committed_length),
      correlation_context_(std::move(correlation_context)),
      volume_barrier_(volume_barrier),
      metrics_(metrics),
      observers_(std::move(observers)),
      artifact_replacement_(std::move(artifact_replacement)) {}

PreparedCheckpointTransaction::PreparedCheckpointTransaction(
    PreparedCheckpointTransaction&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)),
      session_(std::exchange(other.session_, nullptr)),
      snapshot_token_(std::move(other.snapshot_token_)),
      publish_lease_(std::move(other.publish_lease_)),
      manifest_file_(std::move(other.manifest_file_)),
      journal_file_(std::move(other.journal_file_)),
      journal_delete_guard_(std::move(other.journal_delete_guard_)),
      checkpoint_(std::move(other.checkpoint_)),
      journal_header_(std::move(other.journal_header_)),
      journal_file_identity_(std::move(other.journal_file_identity_)),
      previous_committed_length_(other.previous_committed_length_),
      correlation_context_(std::move(other.correlation_context_)),
      disposition_(other.disposition_),
      manifest_candidate_verified_(other.manifest_candidate_verified_),
      runtime_reconciled_(other.runtime_reconciled_),
      published_candidate_(std::move(other.published_candidate_)),
      publication_identity_(std::move(other.publication_identity_)),
      recovery_validation_(std::move(other.recovery_validation_)),
      volume_barrier_(other.volume_barrier_),
      metrics_(other.metrics_),
      observers_(std::move(other.observers_)),
      artifact_replacement_(std::move(other.artifact_replacement_)) {
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
    journal_file_ = std::move(other.journal_file_);
    journal_delete_guard_ = std::move(other.journal_delete_guard_);
    checkpoint_ = std::move(other.checkpoint_);
    journal_header_ = std::move(other.journal_header_);
    journal_file_identity_ = std::move(other.journal_file_identity_);
    previous_committed_length_ = other.previous_committed_length_;
    correlation_context_ = std::move(other.correlation_context_);
    disposition_ = other.disposition_;
    manifest_candidate_verified_ = other.manifest_candidate_verified_;
    runtime_reconciled_ = other.runtime_reconciled_;
    published_candidate_ = std::move(other.published_candidate_);
    publication_identity_ = std::move(other.publication_identity_);
    recovery_validation_ = std::move(other.recovery_validation_);
    volume_barrier_ = other.volume_barrier_;
    metrics_ = other.metrics_;
    observers_ = std::move(other.observers_);
    artifact_replacement_ = std::move(other.artifact_replacement_);
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
    journal_file_.reset();
    journal_delete_guard_.reset();
    if (artifact_replacement_.has_value()) {
        artifact_replacement_->base_capability_anchor.reset();
    }
    if (first_error.has_value()) {
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

CheckpointTransactionOutcome PreparedCheckpointTransaction::publish_manifest(
    const OperationContext& context,
    CheckpointManifestPublishRequest request) {
    const bool compaction = artifact_replacement_.has_value();
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
            error_context.operation = compaction
                ? "compaction.publish" : "checkpoint.publish";
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
                compaction ? L"compaction.publish" : L"checkpoint.publish",
                source->message,
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

    ManifestPublishAttemptOutcome attempted{};
    auto observed = run_transaction_observer(
        observers_.before_manifest_publish,
        L"before-manifest observer threw");
    if (!observed.ok()) {
        attempted.error = observed.error();
    } else {
        manifest_file_ = std::shared_ptr<IPersistenceFile>(
            std::move(request.manifest_file));
        recovery_validation_ = request.complete_validation;
        const auto prepared = prepare_manifest_candidate(
            request.active_slot, checkpoint_, request.committed_time_100ns,
            artifact_replacement_);
        if (!prepared.ok()) {
            attempted.error = prepared.error();
        } else {
            published_candidate_ = prepared.value().slot;
            publication_identity_ = FrozenPublicationIdentity{
                request.active_slot.generation.checkpoint_generation,
                checkpoint_.generation,
                prepared.value().slot.slot_index};
            // Journal construction is complete and durable. Drop the
            // writer/delete capability before normal recovery validates the
            // candidate, then retain a read capability that denies namespace
            // deletion until normal recovery installs the complete runtime
            // BackingView with its own opaque guards.
            if (request.complete_validation.has_value()) {
                auto closed = journal_file_->close();
                journal_file_.reset();
                if (!closed.ok()) {
                    attempted.error = closed.error();
                } else {
                    auto capability = open_bundle_file_for_backing_view(
                        request.complete_validation->bundle_root,
                        prepared.value().slot.journal_relative_name);
                    if (!capability.ok()) {
                        attempted.error = capability.error();
                    } else {
                        auto opened = capability.move_value();
                        if (!same_file_object(
                                   opened.identity(),
                                   journal_file_identity_) ||
                               opened.identity().size !=
                                   checkpoint_.committed_length) {
                            attempted.error = checkpoint_failure<void>(
                                L"reopened journal identity changed before publish",
                                PersistenceErrorCode::GenerationConflict).error();
                        } else {
                            journal_file_identity_ = opened.identity();
                            auto reopened = open_validated_persistence_file(
                                opened.take_file());
                            if (!reopened.ok()) {
                                attempted.error = reopened.error();
                            } else {
                                journal_delete_guard_.emplace(
                                    opened.take_delete_guard());
                                journal_file_ =
                                    std::shared_ptr<IPersistenceFile>(
                                        reopened.move_value());
                            }
                        }
                    }
                }
            }
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
            publish_request.test_phase_hook =
                std::move(request.test_phase_hook);
#endif
            if (!attempted.error.has_value()) {
                attempted = attempt_manifest_publication(
                    context, *publish_lease_, publish_request);
            }
            if (attempted.ok()) {
                observed = run_transaction_observer(
                    observers_.after_manifest_publish,
                    L"after-manifest observer threw");
                if (!observed.ok()) attempted.error = observed.error();
            }
        }
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
        if (artifact_replacement_.has_value()) {
            artifact_replacement_->base_capability_anchor.reset();
        }
    }
    return result;
}

CheckpointTransactionOutcome PreparedCheckpointTransaction::resolve_commit(
    const OperationContext& context,
    const CommitResolutionRequest& request) {
    const bool compaction = artifact_replacement_.has_value();
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
        error_context.operation = compaction
            ? "compaction.resolve" : "checkpoint.resolve";
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
            compaction ? L"compaction.resolve" : L"checkpoint.resolve",
            source.message, source.native_code};
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
    if (resolved.disposition ==
            CommitDisposition::CommittedNeedsReconciliation &&
        request.filesystem.has_value()) {
        recovery_validation_ = CompleteCandidateValidation{
            request.filesystem->bundle_root,
            request.filesystem->manifest_relative_name,
            request.filesystem->options};
    }
    if (resolved.disposition == CommitDisposition::NotCommitted) {
        auto aborted = store_->abort_consistent_snapshot(*snapshot_token_);
        if (!aborted.ok() && !result.error.has_value()) {
            set_error(aborted.error(), resolved.disposition);
        }
        snapshot_token_.reset();
        store_ = nullptr;
        session_ = nullptr;
        if (artifact_replacement_.has_value()) {
            artifact_replacement_->base_capability_anchor.reset();
        }
    }
    return result;
}

CheckpointReconcileOutcome PreparedCheckpointTransaction::reconcile(
    const OperationContext& context,
    const CheckpointReconcileRequest& request) {
#if !defined(MYRD_ENABLE_TEST_SEAMS)
    (void)request;
#endif
    const bool compaction = artifact_replacement_.has_value();
    CheckpointReconcileOutcome result{};
    result.disposition = disposition_;
    result.generation = checkpoint_.generation;
    const auto set_error = [&](const Error& source,
                               const CommitDisposition disposition) {
        const auto code = persistence_code_for(source);
        auto error_context = correlation_context_.value();
        error_context.operation = compaction
            ? "compaction.reconcile" : "checkpoint.reconcile";
        error_context.code = code;
        error_context.phase = disposition == CommitDisposition::NotCommitted
            ? SessionPhase::Checkpointing
            : SessionPhase::Reconciling;
        error_context.native_code = source.native_code;
        error_context.generation = checkpoint_.generation;
        error_context.disposition = disposition;
        error_context.safe_context = disposition ==
                CommitDisposition::CommittedNeedsReconciliation
            ? "durable checkpoint requires runtime reconciliation"
            : "checkpoint is not eligible for reconciliation";
        result.error = Error{
            ErrorDomain::persistence, static_cast<std::uint32_t>(code),
            compaction ? L"compaction.reconcile" : L"checkpoint.reconcile",
            source.message, source.native_code};
        result.error_context = std::move(error_context);
        result.disposition = disposition;
    };
    const bool durable = disposition_ == CommitDisposition::Committed ||
        disposition_ == CommitDisposition::CommittedNeedsReconciliation;
    const bool runtime_state_valid = runtime_reconciled_
        ? !snapshot_token_.has_value()
        : snapshot_token_.has_value() && journal_file_;
    if (!durable || store_ == nullptr || session_ == nullptr ||
        !runtime_state_valid) {
        set_error(checkpoint_failure<void>(
            L"transaction has no durable checkpoint to reconcile",
            PersistenceErrorCode::OperationBusy).error(), disposition_);
        return result;
    }

    if (!publish_lease_.has_value()) {
        auto begun = session_->begin_reconciliation();
        if (!begun.ok()) {
            set_error(begun.error(), disposition_);
            return result;
        }
        publish_lease_ = begun.move_value();
    }
    auto* lease = &*publish_lease_;

    std::stop_source combined_stop;
    const auto request_stop = [&combined_stop]() noexcept {
        combined_stop.request_stop();
    };
    std::stop_callback caller_stop(context.stop_token, request_stop);
    std::stop_callback session_stop(lease->stop_token(), request_stop);
    const OperationContext operation_context{
        combined_stop.get_token(), context.deadline};

    const auto fail = [&](const Error& source) {
        set_error(source,
                  CommitDisposition::CommittedNeedsReconciliation);
        std::optional<PersistenceFailure> failure;
        if (result.error.has_value() && result.error_context.has_value()) {
            failure = PersistenceFailure{
                *result.error, *result.error_context};
        }
        const auto finished = lease->finish(
            CommitDisposition::CommittedNeedsReconciliation,
            std::move(failure));
        if (finished.ok()) publish_lease_.reset();
        disposition_ =
            CommitDisposition::CommittedNeedsReconciliation;
        return result;
    };

#if defined(MYRD_ENABLE_TEST_SEAMS)
    const auto injected = [&](const CheckpointReconcileTestPhase phase)
        -> std::optional<Error> {
        if (!request.test_phase_hook) return std::nullopt;
        try {
            auto outcome = request.test_phase_hook(phase);
            if (!outcome.ok()) return outcome.error();
        } catch (...) {
            return checkpoint_failure<void>(
                L"checkpoint reconcile phase hook threw",
                PersistenceErrorCode::FatalInternalInvariant).error();
        }
        return std::nullopt;
    };
#endif

    const auto report_success = [&]() -> CheckpointReconcileOutcome {
        auto session_finished = lease->finish(CommitDisposition::Committed);
        if (!session_finished.ok()) {
            set_error(session_finished.error(),
                      CommitDisposition::CommittedNeedsReconciliation);
            disposition_ =
                CommitDisposition::CommittedNeedsReconciliation;
            return result;
        }
        publish_lease_.reset();
        disposition_ = CommitDisposition::Committed;
        result.disposition = CommitDisposition::Committed;
        result.error.reset();
        result.error_context.reset();
        store_ = nullptr;
        session_ = nullptr;
        journal_file_.reset();
        journal_delete_guard_.reset();
        manifest_file_.reset();
        if (artifact_replacement_.has_value()) {
            artifact_replacement_->base_capability_anchor.reset();
        }
        return result;
    };

    const auto initially_live = check_operation_context(operation_context);
    if (!initially_live.ok()) return fail(initially_live.error());

    if (runtime_reconciled_) {
        result.backing_installed = true;
        result.snapshot_finished = true;
#if defined(MYRD_ENABLE_TEST_SEAMS)
        if (const auto failure = injected(
                CheckpointReconcileTestPhase::BeforeReportSuccess)) {
            return fail(*failure);
        }
#endif
        auto observed = run_transaction_observer(
            observers_.complete, L"compaction-complete observer threw");
        if (!observed.ok()) return fail(observed.error());
        return report_success();
    }

    if (manifest_file_) {
        const auto closed = manifest_file_->close();
        manifest_file_.reset();
        if (!closed.ok()) return fail(closed.error());
    }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            CheckpointReconcileTestPhase::BeforeRecoverView)) {
        return fail(*failure);
    }
#endif

    BackingView committed_view{};
    std::optional<CompositeBackingIdentity> committed_identity;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (request.test_committed_view.has_value()) {
        committed_view = *request.test_committed_view;
        committed_identity = request.test_committed_identity;
    } else
#endif
    {
        if (!recovery_validation_.has_value()) {
            return fail(checkpoint_failure<void>(
                L"checkpoint reconciliation has no recovery descriptor",
                PersistenceErrorCode::InvalidFormat).error());
        }
        auto recovered = recover_complete_image(
            operation_context, recovery_validation_->bundle_root,
            recovery_validation_->manifest_relative_name,
            recovery_validation_->options);
        if (!recovered.ok() || !recovered.image.has_value()) {
            return fail(recovered.error.value_or(
                checkpoint_failure<void>(
                    L"normal recovery did not return the committed view",
                    PersistenceErrorCode::VerificationFailed).error()));
        }
        const auto& image = *recovered.image;
        const auto& identity = image.backing_identity;
        const auto replacement_base_matches =
            !artifact_replacement_.has_value() ||
            (same_file_object(
                 identity.base_windows_identity,
                 artifact_replacement_->base_windows_identity) &&
             identity.base_windows_identity.size ==
                 artifact_replacement_->base_windows_identity.size);
        const auto same_geometry = [](const ImageGeometry& left,
                                      const ImageGeometry& right) noexcept {
            return left.virtual_bytes == right.virtual_bytes &&
                left.logical_sector_size == right.logical_sector_size &&
                left.physical_sector_size == right.physical_sector_size;
        };
        if (!published_candidate_.has_value() ||
            image.selected_manifest != *published_candidate_ ||
            image.checkpoint_generation != checkpoint_.generation ||
            image.backing_view.generation != checkpoint_.generation ||
            image.image_uuid != journal_header_.image_uuid ||
            !same_geometry(image.geometry, journal_header_.geometry) ||
            identity.image_uuid != journal_header_.image_uuid ||
            !same_geometry(identity.geometry, journal_header_.geometry) ||
            identity.checkpoint_generation != checkpoint_.generation ||
            !replacement_base_matches ||
            !same_file_object(identity.journal_windows_identity,
                              journal_file_identity_) ||
            identity.journal_windows_identity.size <
                checkpoint_.committed_length ||
            identity.journal_identity != journal_header_.journal_identity ||
            identity.journal_epoch != journal_header_.epoch ||
            identity.journal_committed_length !=
                checkpoint_.committed_length ||
            identity.commit != checkpoint_.commit ||
            identity.root != checkpoint_.root) {
            return fail(checkpoint_failure<void>(
                L"normal recovery selected a different immutable identity",
                PersistenceErrorCode::GenerationConflict).error());
        }
        committed_view = image.backing_view;
        committed_identity = identity;
    }
    if (!committed_view.store ||
        committed_view.generation != checkpoint_.generation) {
        return fail(checkpoint_failure<void>(
            L"recovered backing view does not match the durable generation",
            PersistenceErrorCode::GenerationConflict).error());
    }
    if (committed_identity.has_value()) {
        const auto& identity = *committed_identity;
        const auto replacement_base_matches =
            !artifact_replacement_.has_value() ||
            (same_file_object(
                 identity.base_windows_identity,
                 artifact_replacement_->base_windows_identity) &&
             identity.base_windows_identity.size ==
                 artifact_replacement_->base_windows_identity.size);
        const auto same_geometry = [](const ImageGeometry& left,
                                      const ImageGeometry& right) noexcept {
            return left.virtual_bytes == right.virtual_bytes &&
                left.logical_sector_size == right.logical_sector_size &&
                left.physical_sector_size == right.physical_sector_size;
        };
        if (identity.image_uuid != journal_header_.image_uuid ||
            !same_geometry(identity.geometry, journal_header_.geometry) ||
            identity.checkpoint_generation != checkpoint_.generation ||
            !replacement_base_matches ||
            !same_file_object(identity.journal_windows_identity,
                              journal_file_identity_) ||
            identity.journal_windows_identity.size <
                checkpoint_.committed_length ||
            identity.journal_identity != journal_header_.journal_identity ||
            identity.journal_epoch != journal_header_.epoch ||
            identity.journal_committed_length !=
                checkpoint_.committed_length ||
            identity.commit != checkpoint_.commit ||
            identity.root != checkpoint_.root) {
            return fail(checkpoint_failure<void>(
                L"committed backing identity does not match the transaction",
                PersistenceErrorCode::GenerationConflict).error());
        }
    }

    auto stream = checkpoint_.ack_stream;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (request.test_ack_stream.has_value()) {
        stream = *request.test_ack_stream;
    }
#endif
    const auto checked_add = [](const std::uint64_t left,
                                const std::uint64_t right,
                                std::uint64_t& output) noexcept {
        if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
            return false;
        }
        output = left + right;
        return true;
    };
    const auto checked_mul = [](const std::uint64_t left,
                                const std::uint64_t right,
                                std::uint64_t& output) noexcept {
        if (left != 0 &&
            right > (std::numeric_limits<std::uint64_t>::max)() / left) {
            return false;
        }
        output = left * right;
        return true;
    };
    std::uint64_t metadata_count = 0;
    std::uint64_t expected_record_count = 0;
    std::uint64_t data_bytes = 0;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t expected_ack_end = 0;
    std::uint64_t expected_commit_offset = 0;
    std::uint64_t root_end = 0;
    std::uint64_t commit_end = 0;
    const bool empty_root = checkpoint_.root.offset == 0 &&
        checkpoint_.root.length == 0 && checkpoint_.root.crc32c == 0;
    const bool descriptor_complete =
        checked_add(checkpoint_.zero_count, checkpoint_.discard_count,
                    metadata_count) &&
        checked_add(checkpoint_.data_count, metadata_count,
                    expected_record_count) &&
        checked_mul(checkpoint_.data_count, kDataRecordBytes, data_bytes) &&
        checked_mul(metadata_count, kFixedRecordBytes, metadata_bytes) &&
        checked_mul(checkpoint_.index_node_count, kIndexNodeBytes,
                    index_bytes) &&
        checked_add(previous_committed_length_, data_bytes,
                    expected_ack_end) &&
        checked_add(expected_ack_end, metadata_bytes, expected_ack_end) &&
        checked_add(expected_ack_end, index_bytes,
                    expected_commit_offset) &&
        checked_add(checkpoint_.root.offset, checkpoint_.root.length,
                    root_end) &&
        checked_add(checkpoint_.commit.offset, checkpoint_.commit.length,
                    commit_end) &&
        stream.record_count == expected_record_count &&
        stream.begin_offset == previous_committed_length_ &&
        stream.end_offset == expected_ack_end &&
        stream.begin_offset % kJournalAlignment == 0 &&
        stream.end_offset % kJournalAlignment == 0 &&
        checkpoint_.commit.offset == expected_commit_offset &&
        checkpoint_.commit.length == kFixedRecordBytes &&
        (checkpoint_.index_node_count == 0
             ? empty_root &&
                   stream.end_offset == checkpoint_.commit.offset
             : !empty_root &&
                   checkpoint_.root.length == kIndexNodeBytes &&
                   checkpoint_.root.offset >= stream.end_offset &&
                   (checkpoint_.root.offset - stream.end_offset) %
                           kIndexNodeBytes == 0 &&
                   root_end <= checkpoint_.commit.offset) &&
        commit_end == checkpoint_.committed_length &&
        checkpoint_.chain_digest == checkpoint_.commit.chain_digest;
    if (!descriptor_complete) {
        return fail(checkpoint_failure<void>(
            L"ack stream does not completely cover the prepared checkpoint",
            PersistenceErrorCode::VerificationFailed).error());
    }
    const auto before_install_live =
        check_operation_context(operation_context);
    if (!before_install_live.ok()) return fail(before_install_live.error());

    auto observed = run_transaction_observer(
        observers_.before_backing_install,
        L"before-backing-install observer threw");
    if (!observed.ok()) return fail(observed.error());

#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            CheckpointReconcileTestPhase::BeforeInstallBacking)) {
        return fail(*failure);
    }
#endif
    const auto current_view = store_->capture_backing_view();
    if (current_view.generation < committed_view.generation) {
        auto installed = store_->install_backing_view_for_snapshot(
            *snapshot_token_, committed_view);
        if (!installed.ok()) return fail(installed.error());
    } else if (current_view.generation == committed_view.generation) {
        bool equivalent = current_view.store == committed_view.store;
        if (!equivalent && current_view.store && committed_view.store) {
            const auto current_composite =
                std::dynamic_pointer_cast<const CompositeBackingStore>(
                    current_view.store);
            const auto committed_composite =
                std::dynamic_pointer_cast<const CompositeBackingStore>(
                    committed_view.store);
            equivalent = current_composite && committed_composite &&
                current_composite->identity() ==
                    committed_composite->identity();
        }
        if (!equivalent) {
            return fail(checkpoint_failure<void>(
                L"installed generation has a different backing identity",
                PersistenceErrorCode::GenerationConflict).error());
        }
    } else {
        return fail(checkpoint_failure<void>(
            L"live backing generation is newer than the transaction",
            PersistenceErrorCode::GenerationConflict).error());
    }
    result.backing_installed = true;
    observed = run_transaction_observer(
        observers_.after_backing_install,
        L"after-backing-install observer threw");
    if (!observed.ok()) return fail(observed.error());

    observed = run_transaction_observer(
        observers_.before_conditional_ack,
        L"before-conditional-ack observer threw");
    if (!observed.ok()) return fail(observed.error());

#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            CheckpointReconcileTestPhase::BeforeAckReplay)) {
        return fail(*failure);
    }
#endif
    auto replayed = replay_checkpoint_ack_stream(
        operation_context,
        {journal_file_, journal_header_, stream,
         store_, *snapshot_token_});
    if (!replayed.ok()) return fail(replayed.error());
    result.ack = replayed.move_value();
    observed = run_transaction_observer(
        observers_.after_conditional_ack,
        L"after-conditional-ack observer threw");
    if (!observed.ok()) return fail(observed.error());

    observed = run_transaction_observer(
        observers_.before_snapshot_finish,
        L"before-snapshot-finish observer threw");
    if (!observed.ok()) return fail(observed.error());

#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            CheckpointReconcileTestPhase::BeforeFinishSnapshot)) {
        return fail(*failure);
    }
#endif
    auto snapshot_finished =
        store_->finish_consistent_snapshot(*snapshot_token_);
    if (!snapshot_finished.ok()) return fail(snapshot_finished.error());
    snapshot_token_.reset();
    runtime_reconciled_ = true;
    result.snapshot_finished = true;
    observed = run_transaction_observer(
        observers_.after_snapshot_finish,
        L"after-snapshot-finish observer threw");
    if (!observed.ok()) return fail(observed.error());

#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            CheckpointReconcileTestPhase::BeforeReportSuccess)) {
        return fail(*failure);
    }
#endif
    observed = run_transaction_observer(
        observers_.complete, L"compaction-complete observer threw");
    if (!observed.ok()) return fail(observed.error());
    return report_success();
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
        journal_file_.reset();
        journal_delete_guard_.reset();
        store_ = nullptr;
        session_ = nullptr;
    } catch (...) {
        snapshot_token_.reset();
        publish_lease_.reset();
        manifest_file_.reset();
        journal_file_.reset();
        journal_delete_guard_.reset();
        store_ = nullptr;
        session_ = nullptr;
    }
}

CheckpointPrepareOutcome CheckpointCoordinator::prepare(
    const OperationContext& context,
    const CheckpointPrepareRequest& request) {
    return prepare_impl(
        context, request, std::nullopt, {}, {}, {}, std::nullopt);
}

CheckpointPrepareOutcome CheckpointCoordinator::prepare_with_lease(
    const OperationContext& context,
    const CheckpointPrepareRequest& request,
    PublishOperationLease lease,
    std::function<Result<void>()> after_snapshot_begin,
    std::function<Result<void>()> after_journal_prepare,
    CheckpointTransactionObservers observers,
    std::optional<ManifestArtifactReplacement> artifact_replacement) {
    return prepare_impl(
        context, request,
        std::optional<PublishOperationLease>{std::move(lease)},
        std::move(after_snapshot_begin),
        std::move(after_journal_prepare),
        std::move(observers),
        std::move(artifact_replacement));
}

CheckpointPrepareOutcome CheckpointCoordinator::prepare_impl(
    const OperationContext& context,
    const CheckpointPrepareRequest& request,
    std::optional<PublishOperationLease> existing_lease,
    std::function<Result<void>()> after_snapshot_begin,
    std::function<Result<void>()> after_journal_prepare,
    CheckpointTransactionObservers observers,
    std::optional<ManifestArtifactReplacement> artifact_replacement) {
    const bool compaction = existing_lease.has_value();
    const auto fail_before_snapshot = [&](const Error& error) {
        auto failure = prepare_failure(
            request, session_->phase(), error, compaction);
        if (existing_lease.has_value()) {
            finish_not_committed(*existing_lease, failure);
            existing_lease.reset();
        }
        return CheckpointPrepareOutcome{std::move(failure)};
    };
    // The barrier is deliberately the first validation: an omitted or forged
    // marker cannot begin either the session operation or the M2 snapshot.
    if (!is_valid_barrier_for_checkpoint(request.volume_barrier)) {
        const auto error = checkpoint_failure<void>(
            L"volume barrier evidence is missing or unknown").error();
        return fail_before_snapshot(error);
    }
    auto expected_target_generation = request.journal.previous_generation;
    const bool target_generation_valid =
        expected_target_generation < kInvalidGeneration - 1;
    if (target_generation_valid) ++expected_target_generation;
    const auto& correlation = request.correlation_context.value();
    if (!request.journal.blocks.empty() ||
        correlation.operation != (compaction
            ? "compaction.prepare" : "checkpoint.prepare") ||
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
        return fail_before_snapshot(error);
    }

    std::optional<PublishOperationLease> lease_owner;
    if (existing_lease.has_value()) {
        if (!existing_lease->active() ||
            existing_lease->operation() != SessionOperation::Compaction) {
            const auto error = checkpoint_failure<void>(
                L"existing publish lease is not an active compaction")
                                   .error();
            return fail_before_snapshot(error);
        }
        lease_owner = std::move(existing_lease);
        existing_lease.reset();
    } else {
        auto lease_result = session_->begin_checkpoint();
        if (!lease_result.ok()) {
            return CheckpointPrepareOutcome{prepare_failure(
                request, session_->phase(), lease_result.error(), false)};
        }
        lease_owner = lease_result.move_value();
    }
    auto& lease = *lease_owner;

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
            request,
            compaction ? SessionPhase::Compacting
                       : SessionPhase::Checkpointing,
            token_result.error(), compaction);
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }
    const auto token = token_result.value();
    // From this point until ownership is transferred into the prepared
    // transaction, every return and every C++ exception must retire the cut.
    SnapshotAbortGuard snapshot_guard{*store_, token};
    if (after_snapshot_begin) {
        Result<void> observed = Result<void>::success();
        try {
            observed = after_snapshot_begin();
        } catch (...) {
            observed = checkpoint_failure<void>(
                L"snapshot-begin observer threw",
                PersistenceErrorCode::FatalInternalInvariant);
        }
        if (!observed.ok()) {
            auto failure = prepare_failure(
                request,
                compaction ? SessionPhase::Compacting
                           : SessionPhase::Checkpointing,
                observed.error(), compaction);
            snapshot_guard.abort_now();
            finish_not_committed(lease, failure);
            return CheckpointPrepareOutcome{std::move(failure)};
        }
    }
    if (token.source_backing_generation !=
        request.journal.previous_generation) {
        const auto error = checkpoint_failure<void>(
            L"snapshot source generation changed before the consistency cut",
            PersistenceErrorCode::GenerationConflict).error();
        auto failure = prepare_failure(
            request,
            compaction ? SessionPhase::Compacting
                       : SessionPhase::Checkpointing,
            error, compaction);
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
            // Advancing the M2 frontier transfers any COW pre-image into
            // copied; it does not discard the last authoritative bytes.
            // consume() synchronously writes that bounded ownership into the
            // journal. If any later pre-commit step fails, the snapshot is
            // aborted and the post-cut live version remains dirty. If commit
            // succeeds, Task32 replays the durable record identity and either
            // exact-acks the old member or proves that newer dirty state still
            // exists. Holding every COW payload until manifest publication
            // would violate the bounded-memory/stall contract.
        }
        return Result<void>::success();
    };

    auto prepared = journal_writer_->prepare_streaming(
        operation_context, request.journal, producer);
    if (!prepared.ok()) {
        auto failure = prepare_failure(
            request,
            compaction ? SessionPhase::Compacting
                       : SessionPhase::Checkpointing,
            prepared.error(), compaction);
        snapshot_guard.abort_now();
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }
    if (after_journal_prepare) {
        Result<void> observed = Result<void>::success();
        try {
            observed = after_journal_prepare();
        } catch (...) {
            observed = checkpoint_failure<void>(
                L"journal-prepare observer threw",
                PersistenceErrorCode::FatalInternalInvariant);
        }
        if (!observed.ok()) {
            auto failure = prepare_failure(
                request,
                compaction ? SessionPhase::Compacting
                           : SessionPhase::Checkpointing,
                observed.error(), compaction);
            snapshot_guard.abort_now();
            finish_not_committed(lease, failure);
            return CheckpointPrepareOutcome{std::move(failure)};
        }
    }
    if (store_->backing_generation() != token.source_backing_generation) {
        const auto error = checkpoint_failure<void>(
            L"backing generation changed while preparing the checkpoint",
            PersistenceErrorCode::GenerationConflict).error();
        auto failure = prepare_failure(
            request,
            compaction ? SessionPhase::Compacting
                       : SessionPhase::Checkpointing,
            error, compaction);
        snapshot_guard.abort_now();
        finish_not_committed(lease, failure);
        return CheckpointPrepareOutcome{std::move(failure)};
    }

    PreparedCheckpointTransaction transaction{
        *session_, *store_, token, std::move(lease), prepared.move_value(),
        request.journal.file, request.journal.header,
        request.journal.file_identity, request.journal.committed_length,
        request.correlation_context,
        *request.volume_barrier, metrics, std::move(observers),
        std::move(artifact_replacement)};
    snapshot_guard.release();
    return CheckpointPrepareOutcome{std::move(transaction)};
}

}  // namespace myrd::persistence
