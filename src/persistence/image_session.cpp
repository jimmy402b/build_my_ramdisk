#include "image_session.h"

#include <chrono>
#include <utility>

namespace myrd::persistence {
namespace {

bool is_checkpoint_like(const SessionOperation operation) noexcept {
    return operation == SessionOperation::Checkpoint ||
           operation == SessionOperation::Compaction;
}

bool is_valid_finish(const SessionOperation operation,
                     const CommitDisposition disposition) noexcept {
    if (is_checkpoint_like(operation)) return true;
    if (operation == SessionOperation::CommitResolution) {
        return disposition == CommitDisposition::NotCommitted ||
               disposition == CommitDisposition::CommittedNeedsReconciliation ||
               disposition == CommitDisposition::CommitIndeterminate;
    }
    return disposition == CommitDisposition::Committed ||
           disposition == CommitDisposition::CommittedNeedsReconciliation;
}

SessionPhase phase_after_finish(const SessionOperation operation,
                                const CommitDisposition disposition) noexcept {
    if (is_checkpoint_like(operation)) {
        switch (disposition) {
        case CommitDisposition::NotCommitted:
        case CommitDisposition::Committed:
            return SessionPhase::Ready;
        case CommitDisposition::CommitIndeterminate:
            return SessionPhase::ResolvingCommit;
        case CommitDisposition::CommittedNeedsReconciliation:
            return SessionPhase::Reconciling;
        }
    }
    if (operation == SessionOperation::CommitResolution) {
        switch (disposition) {
        case CommitDisposition::NotCommitted: return SessionPhase::Ready;
        case CommitDisposition::CommittedNeedsReconciliation:
            return SessionPhase::Reconciling;
        case CommitDisposition::CommitIndeterminate:
        case CommitDisposition::Committed:
            return SessionPhase::PersistenceFaulted;
        }
    }
    switch (disposition) {
    case CommitDisposition::Committed: return SessionPhase::Ready;
    case CommitDisposition::CommittedNeedsReconciliation:
        return SessionPhase::Reconciling;
    case CommitDisposition::NotCommitted:
    case CommitDisposition::CommitIndeterminate:
        return SessionPhase::PersistenceFaulted;
    }
    return SessionPhase::PersistenceFaulted;
}

} // namespace

PublishOperationLease::PublishOperationLease(
    ImageSession* session, const SessionOperation operation,
    const std::uint64_t operation_id, std::stop_token stop_token) noexcept
    : session_(session), operation_(operation), operation_id_(operation_id),
      stop_token_(std::move(stop_token)) {}

PublishOperationLease::PublishOperationLease(
    PublishOperationLease&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)),
      operation_(other.operation_),
      operation_id_(other.operation_id_),
      stop_token_(std::move(other.stop_token_)) {}

PublishOperationLease& PublishOperationLease::operator=(
    PublishOperationLease&& other) noexcept {
    if (this != &other) {
        abandon_noexcept();
        session_ = std::exchange(other.session_, nullptr);
        operation_ = other.operation_;
        operation_id_ = other.operation_id_;
        stop_token_ = std::move(other.stop_token_);
    }
    return *this;
}

PublishOperationLease::~PublishOperationLease() {
    abandon_noexcept();
}

Result<SessionOutcome> PublishOperationLease::finish(
    const CommitDisposition disposition, const bool durable_verified,
    std::optional<PersistenceFailure> failure) {
    if (session_ == nullptr) {
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"operation lease is inactive"));
    }
    auto result = session_->finish_operation(
        operation_id_, operation_, disposition, durable_verified,
        std::move(failure));
    if (result.ok()) session_ = nullptr;
    return result;
}

Result<SessionOutcome> PublishOperationLease::abort() {
    if (session_ == nullptr) {
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.abort", L"operation lease is inactive"));
    }
    auto result = session_->abandon_operation(operation_id_, operation_);
    if (result.ok()) session_ = nullptr;
    return result;
}

void PublishOperationLease::abandon_noexcept() noexcept {
    if (session_ != nullptr) {
        session_->abandon_operation_noexcept(operation_id_, operation_);
        session_ = nullptr;
    }
}

ImmutableReadLease::ImmutableReadLease(ImmutableReadLease&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)) {}

ImmutableReadLease& ImmutableReadLease::operator=(
    ImmutableReadLease&& other) noexcept {
    if (this != &other) {
        release();
        session_ = std::exchange(other.session_, nullptr);
    }
    return *this;
}

ImmutableReadLease::~ImmutableReadLease() {
    release();
}

void ImmutableReadLease::release() noexcept {
    if (session_ != nullptr) {
        session_->release_read();
        session_ = nullptr;
    }
}

ImageSession::ImageSession(const SessionPhase initial_phase)
    : phase_(initial_phase) {}

SessionPhase ImageSession::phase() const {
    std::lock_guard lock(mutex_);
    return phase_;
}

SessionSnapshot ImageSession::snapshot() const {
    std::lock_guard lock(mutex_);
    return {
        phase_, disposition_, first_failure_, active_immutable_reads_,
        publish_operation_active_, close_requested_};
}

Error ImageSession::state_error(const PersistenceErrorCode code,
                                const wchar_t* operation,
                                const wchar_t* message) {
    return {ErrorDomain::persistence, static_cast<std::uint32_t>(code),
            operation, message};
}

Result<PublishOperationLease> ImageSession::begin_checkpoint() {
    return begin_operation(SessionOperation::Checkpoint);
}

Result<PublishOperationLease> ImageSession::begin_compaction() {
    return begin_operation(SessionOperation::Compaction);
}

Result<PublishOperationLease> ImageSession::begin_commit_resolution() {
    return begin_operation(SessionOperation::CommitResolution);
}

Result<PublishOperationLease> ImageSession::begin_reconciliation() {
    return begin_operation(SessionOperation::Reconciliation);
}

Result<PublishOperationLease> ImageSession::begin_operation(
    const SessionOperation operation) {
    std::lock_guard lock(mutex_);
    if (close_requested_ || phase_ == SessionPhase::Closing ||
        phase_ == SessionPhase::Closed) {
        return Result<PublishOperationLease>::failure(state_error(
            PersistenceErrorCode::OperationBusy, L"image_session.begin",
            L"session is closing or closed"));
    }
    if (publish_operation_active_) {
        return Result<PublishOperationLease>::failure(state_error(
            PersistenceErrorCode::OperationBusy, L"image_session.begin",
            L"another publish-affecting operation is active"));
    }
    if (is_checkpoint_like(operation)) {
        if (phase_ == SessionPhase::ResolvingCommit) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::CommitIndeterminate, L"image_session.begin",
                L"commit disposition must be resolved before mutation"));
        }
        if (phase_ == SessionPhase::Reconciling) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::OperationBusy, L"image_session.begin",
                L"committed state must be reconciled before mutation"));
        }
        if (phase_ != SessionPhase::Ready) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::OperationBusy, L"image_session.begin",
                L"session is not ready for mutation"));
        }
        phase_ = operation == SessionOperation::Checkpoint
            ? SessionPhase::Checkpointing : SessionPhase::Compacting;
    } else if (operation == SessionOperation::CommitResolution) {
        if (phase_ != SessionPhase::ResolvingCommit) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::OperationBusy, L"image_session.resolve",
                L"session is not resolving a commit"));
        }
    } else {
        if (phase_ != SessionPhase::Reconciling) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::OperationBusy, L"image_session.reconcile",
                L"session is not reconciling"));
        }
    }

    if (next_operation_id_ == 0) next_operation_id_ = 1;
    const auto operation_id = next_operation_id_++;
    active_operation_stop_source_ = std::stop_source{};
    publish_operation_active_ = true;
    active_operation_ = operation;
    active_operation_id_ = operation_id;
    return Result<PublishOperationLease>::success(
        PublishOperationLease(this, operation, operation_id,
                              active_operation_stop_source_.get_token()));
}

Result<ImmutableReadLease> ImageSession::begin_immutable_read() {
    std::lock_guard lock(mutex_);
    if (close_requested_ || phase_ == SessionPhase::Opening ||
        phase_ == SessionPhase::Closing || phase_ == SessionPhase::Closed) {
        return Result<ImmutableReadLease>::failure(state_error(
            PersistenceErrorCode::OperationBusy, L"image_session.read",
            L"immutable state is not available"));
    }
    ++active_immutable_reads_;
    return Result<ImmutableReadLease>::success(ImmutableReadLease(this));
}

Result<SessionOutcome> ImageSession::finish_operation(
    const std::uint64_t operation_id, const SessionOperation operation,
    const CommitDisposition disposition, const bool durable_verified,
    std::optional<PersistenceFailure> failure) {
    std::lock_guard lock(mutex_);
    if (!publish_operation_active_ || active_operation_id_ != operation_id ||
        active_operation_ != operation) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"operation ownership mismatch"));
    }
    if (!is_valid_finish(operation, disposition) ||
        !is_valid_disposition_transition(disposition_, disposition)) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"invalid structured outcome transition"));
    }
    const bool committed_disposition =
        disposition == CommitDisposition::Committed ||
        disposition == CommitDisposition::CommittedNeedsReconciliation;
    if (durable_verified != committed_disposition) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish",
            L"commit disposition and durable proof disagree"));
    }

    const auto next_phase = phase_after_finish(operation, disposition);
    if (!is_valid_transition(phase_, next_phase)) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"invalid session phase transition"));
    }
    if (failure.has_value()) {
        failure->context.disposition = disposition;
        if (!first_failure_.has_value()) first_failure_ = std::move(failure);
    }
    disposition_ = disposition;
    phase_ = next_phase;
    publish_operation_active_ = false;
    active_operation_id_ = 0;
    active_operation_stop_source_ = std::stop_source{};
    notify_if_drained_locked();
    return Result<SessionOutcome>::success(
        outcome_locked(disposition == CommitDisposition::Committed &&
                       durable_verified));
}

Result<SessionOutcome> ImageSession::abandon_operation(
    const std::uint64_t operation_id, const SessionOperation operation) {
    std::lock_guard lock(mutex_);
    if (!publish_operation_active_ || active_operation_id_ != operation_id ||
        active_operation_ != operation) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.abort", L"operation ownership mismatch"));
    }
    const auto next_phase = is_checkpoint_like(operation)
        ? SessionPhase::Ready : SessionPhase::PersistenceFaulted;
    if (!is_valid_transition(phase_, next_phase)) {
        phase_ = SessionPhase::PersistenceFaulted;
    } else {
        phase_ = next_phase;
    }
    publish_operation_active_ = false;
    active_operation_id_ = 0;
    active_operation_stop_source_ = std::stop_source{};
    notify_if_drained_locked();
    return Result<SessionOutcome>::success(outcome_locked(false));
}

void ImageSession::abandon_operation_noexcept(
    const std::uint64_t operation_id,
    const SessionOperation operation) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!publish_operation_active_ ||
            active_operation_id_ != operation_id ||
            active_operation_ != operation) {
            return;
        }
        const auto next_phase = is_checkpoint_like(operation)
            ? SessionPhase::Ready : SessionPhase::PersistenceFaulted;
        phase_ = is_valid_transition(phase_, next_phase)
            ? next_phase : SessionPhase::PersistenceFaulted;
        publish_operation_active_ = false;
        active_operation_id_ = 0;
        active_operation_stop_source_ = std::stop_source{};
        notify_if_drained_locked();
    } catch (...) {
        // A move-only lease destructor cannot report. Normal callers should
        // use abort(), which returns the structured state outcome.
    }
}

void ImageSession::release_read() noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (active_immutable_reads_ != 0) --active_immutable_reads_;
        notify_if_drained_locked();
    } catch (...) {
        // ImmutableReadLease destruction cannot report.
    }
}

SessionOutcome ImageSession::outcome_locked(const bool durable_success) const {
    return {phase_, disposition_, durable_success, first_failure_};
}

void ImageSession::notify_if_drained_locked() noexcept {
    if (!publish_operation_active_ && active_immutable_reads_ == 0) {
        condition_.notify_all();
    }
}

Result<SessionCloseOutcome> ImageSession::close(
    const OperationContext& context) {
    std::unique_lock lock(mutex_);
    if (phase_ == SessionPhase::Closed) {
        SessionCloseOutcome outcome{};
        outcome.phase = phase_;
        outcome.disposition = disposition_;
        outcome.first_failure = first_failure_;
        outcome.already_closed = true;
        return Result<SessionCloseOutcome>::success(std::move(outcome));
    }
    close_requested_ = true;
    active_operation_stop_source_.request_stop();
    const auto drained = [this] {
        return !publish_operation_active_ && active_immutable_reads_ == 0;
    };
    while (!drained()) {
        if (context.stop_token.stop_requested()) {
            return Result<SessionCloseOutcome>::failure(state_error(
                PersistenceErrorCode::Cancelled, L"image_session.close",
                L"close wait was cancelled"));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= context.deadline) {
            return Result<SessionCloseOutcome>::failure(state_error(
                PersistenceErrorCode::DeadlineExceeded, L"image_session.close",
                L"close wait deadline expired"));
        }
        const auto poll_deadline = now + std::chrono::milliseconds(10);
        condition_.wait_until(lock,
            poll_deadline < context.deadline ? poll_deadline : context.deadline);
    }
    // A different concurrent close may have completed the transition.
    if (phase_ != SessionPhase::Closed) {
        if (!is_valid_transition(phase_, SessionPhase::Closing)) {
            if (!is_valid_transition(phase_, SessionPhase::PersistenceFaulted)) {
                return Result<SessionCloseOutcome>::failure(state_error(
                    PersistenceErrorCode::FatalInternalInvariant,
                    L"image_session.close", L"invalid closing transition"));
            }
            phase_ = SessionPhase::PersistenceFaulted;
        }
        phase_ = SessionPhase::Closing;
        phase_ = SessionPhase::Closed;
    }
    SessionCloseOutcome outcome{};
    outcome.phase = phase_;
    outcome.disposition = disposition_;
    outcome.first_failure = first_failure_;
    outcome.already_closed = false;
    outcome.durable_success = false;
    condition_.notify_all();
    return Result<SessionCloseOutcome>::success(std::move(outcome));
}

} // namespace myrd::persistence
