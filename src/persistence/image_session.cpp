#include "image_session.h"

#include <chrono>
#include <utility>

namespace myrd::persistence {

struct ImageSessionLeaseAnchor final {
    std::mutex mutex;
    ImageSession* session{nullptr};
};

namespace {

bool is_checkpoint_like(const SessionOperation operation) noexcept {
    return operation == SessionOperation::Checkpoint ||
           operation == SessionOperation::Compaction;
}

bool begins_from_ready(const SessionOperation operation) noexcept {
    return is_checkpoint_like(operation) ||
           operation == SessionOperation::Cleanup;
}

bool is_valid_finish(const SessionOperation operation,
                     const CommitDisposition disposition) noexcept {
    if (is_checkpoint_like(operation)) return true;
    if (operation == SessionOperation::Cleanup) {
        return disposition == CommitDisposition::NotCommitted;
    }
    if (operation == SessionOperation::CommitResolution) {
        return disposition == CommitDisposition::NotCommitted ||
               disposition == CommitDisposition::CommittedNeedsReconciliation ||
               disposition == CommitDisposition::CommitIndeterminate;
    }
    return disposition == CommitDisposition::Committed ||
           disposition == CommitDisposition::CommittedNeedsReconciliation;
}

bool disposition_matches_stage(const SessionOperation operation,
                               const PublicationStage stage,
                               const CommitDisposition disposition) noexcept {
    if (is_checkpoint_like(operation)) {
        switch (stage) {
        case PublicationStage::PrePublication:
            return disposition == CommitDisposition::NotCommitted;
        case PublicationStage::PublishAttempted:
            return disposition == CommitDisposition::CommitIndeterminate;
        case PublicationStage::CommitProven:
            return disposition == CommitDisposition::Committed ||
                   disposition ==
                       CommitDisposition::CommittedNeedsReconciliation;
        }
    }
    if (operation == SessionOperation::Cleanup) {
        return stage == PublicationStage::PrePublication &&
               disposition == CommitDisposition::NotCommitted;
    }
    if (operation == SessionOperation::CommitResolution) {
        if (disposition == CommitDisposition::NotCommitted) {
            return stage == PublicationStage::PrePublication;
        }
        if (disposition == CommitDisposition::CommitIndeterminate) {
            return stage != PublicationStage::CommitProven;
        }
        return disposition == CommitDisposition::CommittedNeedsReconciliation &&
               stage == PublicationStage::CommitProven;
    }
    return stage == PublicationStage::CommitProven &&
           (disposition == CommitDisposition::Committed ||
            disposition == CommitDisposition::CommittedNeedsReconciliation);
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
    if (operation == SessionOperation::Cleanup) {
        return disposition == CommitDisposition::NotCommitted
            ? SessionPhase::Ready : SessionPhase::PersistenceFaulted;
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
    std::shared_ptr<ImageSessionLeaseAnchor> anchor,
    const SessionOperation operation,
    const std::uint64_t operation_id, std::stop_token stop_token) noexcept
    : anchor_(std::move(anchor)), operation_(operation),
      operation_id_(operation_id),
      stop_token_(std::move(stop_token)) {}

PublishOperationLease::PublishOperationLease(
    PublishOperationLease&& other) noexcept
    : anchor_(std::move(other.anchor_)),
      operation_(other.operation_),
      operation_id_(other.operation_id_),
      stop_token_(std::move(other.stop_token_)) {}

PublishOperationLease& PublishOperationLease::operator=(
    PublishOperationLease&& other) noexcept {
    if (this != &other) {
        abandon_noexcept();
        anchor_ = std::move(other.anchor_);
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
    const CommitDisposition disposition,
    std::optional<PersistenceFailure> failure) {
    if (anchor_ == nullptr) {
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"operation lease is inactive"));
    }
    auto anchor = anchor_;
    std::unique_lock anchor_lock(anchor->mutex);
    if (anchor->session == nullptr) {
        anchor_lock.unlock();
        anchor_.reset();
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"owning session no longer exists"));
    }
    auto result = anchor->session->finish_operation(
        operation_id_, operation_, disposition, std::move(failure));
    anchor_lock.unlock();
    if (result.ok()) anchor_.reset();
    return result;
}

Result<void> PublishOperationLease::note_publish_attempt_started() {
    if (anchor_ == nullptr) {
        return Result<void>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.publish", L"operation lease is inactive"));
    }
    auto anchor = anchor_;
    std::lock_guard anchor_lock(anchor->mutex);
    if (anchor->session == nullptr) {
        return Result<void>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.publish", L"owning session no longer exists"));
    }
    return anchor->session->advance_publication_stage(
        operation_id_, operation_, PublicationStage::PublishAttempted);
}

Result<void> PublishOperationLease::note_durable_commit_proven() {
    if (anchor_ == nullptr) {
        return Result<void>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.commit", L"operation lease is inactive"));
    }
    auto anchor = anchor_;
    std::lock_guard anchor_lock(anchor->mutex);
    if (anchor->session == nullptr) {
        return Result<void>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.commit", L"owning session no longer exists"));
    }
    return anchor->session->advance_publication_stage(
        operation_id_, operation_, PublicationStage::CommitProven);
}

Result<SessionOutcome> PublishOperationLease::abort() {
    if (anchor_ == nullptr) {
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.abort", L"operation lease is inactive"));
    }
    auto anchor = anchor_;
    std::unique_lock anchor_lock(anchor->mutex);
    if (anchor->session == nullptr) {
        anchor_lock.unlock();
        anchor_.reset();
        return Result<SessionOutcome>::failure(ImageSession::state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.abort", L"owning session no longer exists"));
    }
    auto result = anchor->session->abandon_operation(
        operation_id_, operation_);
    anchor_lock.unlock();
    if (result.ok()) anchor_.reset();
    return result;
}

void PublishOperationLease::abandon_noexcept() noexcept {
    if (anchor_ != nullptr) {
        try {
            auto anchor = anchor_;
            std::lock_guard anchor_lock(anchor->mutex);
            if (anchor->session != nullptr) {
                anchor->session->abandon_operation_noexcept(
                    operation_id_, operation_);
            }
        } catch (...) {
        }
        anchor_.reset();
    }
}

ImmutableReadLease::ImmutableReadLease(ImmutableReadLease&& other) noexcept
    : anchor_(std::move(other.anchor_)) {}

ImmutableReadLease& ImmutableReadLease::operator=(
    ImmutableReadLease&& other) noexcept {
    if (this != &other) {
        release();
        anchor_ = std::move(other.anchor_);
    }
    return *this;
}

ImmutableReadLease::~ImmutableReadLease() {
    release();
}

void ImmutableReadLease::release() noexcept {
    if (anchor_ != nullptr) {
        try {
            auto anchor = anchor_;
            std::lock_guard anchor_lock(anchor->mutex);
            if (anchor->session != nullptr) anchor->session->release_read();
        } catch (...) {
        }
        anchor_.reset();
    }
}

ImageSession::ImageSession()
    : phase_(SessionPhase::Closed),
      lease_anchor_(std::make_shared<ImageSessionLeaseAnchor>()) {
    lease_anchor_->session = this;
}

ImageSession::ImageSession(const SessionPhase initial_phase)
    : phase_(initial_phase),
      lease_anchor_(std::make_shared<ImageSessionLeaseAnchor>()) {
    lease_anchor_->session = this;
}

ImageSession::~ImageSession() {
    std::lock_guard anchor_lock(lease_anchor_->mutex);
    lease_anchor_->session = nullptr;
}

ImageSession ImageSession::ready() {
    return ImageSession(SessionPhase::Ready);
}

SessionPhase ImageSession::phase() const {
    std::lock_guard lock(mutex_);
    return phase_;
}

SessionSnapshot ImageSession::snapshot() const {
    std::lock_guard lock(mutex_);
    return {
        phase_, disposition_, first_failure_, active_immutable_reads_,
        publish_operation_active_, active_publication_stage_, close_requested_};
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

Result<PublishOperationLease> ImageSession::begin_cleanup() {
    return begin_operation(SessionOperation::Cleanup);
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
    if (begins_from_ready(operation)) {
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
        if (operation == SessionOperation::Cleanup &&
            active_immutable_reads_ != 0) {
            return Result<PublishOperationLease>::failure(state_error(
                PersistenceErrorCode::OperationBusy,
                L"image_session.cleanup",
                L"immutable readers must drain before cleanup"));
        }
        phase_ = operation == SessionOperation::Checkpoint
            ? SessionPhase::Checkpointing
            : operation == SessionOperation::Cleanup
                ? SessionPhase::Verifying : SessionPhase::Compacting;
        // Commit disposition and first failure are operation-scoped. A new
        // independent checkpoint must not inherit the previous outcome.
        disposition_ = CommitDisposition::NotCommitted;
        first_failure_.reset();
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
    active_publication_stage_ =
        operation == SessionOperation::Reconciliation
            ? PublicationStage::CommitProven
            : PublicationStage::PrePublication;
    return Result<PublishOperationLease>::success(
        PublishOperationLease(lease_anchor_, operation, operation_id,
                              active_operation_stop_source_.get_token()));
}

Result<ImmutableReadLease> ImageSession::begin_immutable_read() {
    std::lock_guard lock(mutex_);
    if (close_requested_ || phase_ == SessionPhase::Opening ||
        phase_ == SessionPhase::Verifying ||
        phase_ == SessionPhase::Closing || phase_ == SessionPhase::Closed) {
        return Result<ImmutableReadLease>::failure(state_error(
            PersistenceErrorCode::OperationBusy, L"image_session.read",
            L"immutable state is not available"));
    }
    ++active_immutable_reads_;
    return Result<ImmutableReadLease>::success(
        ImmutableReadLease(lease_anchor_));
}

Result<SessionOutcome> ImageSession::finish_operation(
    const std::uint64_t operation_id, const SessionOperation operation,
    const CommitDisposition disposition,
    std::optional<PersistenceFailure> failure) {
    std::lock_guard lock(mutex_);
    if (!publish_operation_active_ || active_operation_id_ != operation_id ||
        active_operation_ != operation) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"operation ownership mismatch"));
    }
    if (!is_valid_finish(operation, disposition) ||
        !is_valid_disposition_transition(disposition_, disposition) ||
        !disposition_matches_stage(
            operation, active_publication_stage_, disposition)) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.finish", L"invalid structured outcome transition"));
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
    active_publication_stage_ = PublicationStage::PrePublication;
    notify_if_drained_locked();
    return Result<SessionOutcome>::success(
        outcome_locked(disposition == CommitDisposition::Committed));
}

Result<void> ImageSession::advance_publication_stage(
    const std::uint64_t operation_id, const SessionOperation operation,
    const PublicationStage target) {
    std::lock_guard lock(mutex_);
    if (!publish_operation_active_ || active_operation_id_ != operation_id ||
        active_operation_ != operation ||
        operation == SessionOperation::Reconciliation ||
        operation == SessionOperation::Cleanup) {
        return Result<void>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.publication_stage",
            L"operation cannot advance publication stage"));
    }
    const bool valid =
        (active_publication_stage_ == PublicationStage::PrePublication &&
         target == PublicationStage::PublishAttempted) ||
        (active_publication_stage_ == PublicationStage::PublishAttempted &&
         target == PublicationStage::CommitProven);
    if (!valid) {
        return Result<void>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.publication_stage",
            L"publication stage transition is invalid"));
    }
    active_publication_stage_ = target;
    if (target == PublicationStage::PublishAttempted) {
        disposition_ = CommitDisposition::CommitIndeterminate;
    } else if (target == PublicationStage::CommitProven) {
        disposition_ = operation == SessionOperation::CommitResolution
            ? CommitDisposition::CommittedNeedsReconciliation
            : CommitDisposition::Committed;
    }
    return Result<void>::success();
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
    if ((!is_checkpoint_like(operation) &&
         operation != SessionOperation::Cleanup) ||
        active_publication_stage_ != PublicationStage::PrePublication) {
        return Result<SessionOutcome>::failure(state_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"image_session.abort",
            L"only a pre-publication checkpoint may be aborted"));
    }
    phase_ = SessionPhase::Ready;
    disposition_ = CommitDisposition::NotCommitted;
    publish_operation_active_ = false;
    active_operation_id_ = 0;
    active_operation_stop_source_ = std::stop_source{};
    active_publication_stage_ = PublicationStage::PrePublication;
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
        if (operation == SessionOperation::Cleanup) {
            phase_ = SessionPhase::Ready;
            disposition_ = CommitDisposition::NotCommitted;
        } else if (is_checkpoint_like(operation)) {
            switch (active_publication_stage_) {
            case PublicationStage::PrePublication:
                phase_ = SessionPhase::Ready;
                disposition_ = CommitDisposition::NotCommitted;
                break;
            case PublicationStage::PublishAttempted:
                phase_ = SessionPhase::ResolvingCommit;
                disposition_ = CommitDisposition::CommitIndeterminate;
                break;
            case PublicationStage::CommitProven:
                phase_ = SessionPhase::Reconciling;
                disposition_ =
                    CommitDisposition::CommittedNeedsReconciliation;
                break;
            }
        } else if (operation == SessionOperation::CommitResolution &&
                   active_publication_stage_ ==
                       PublicationStage::CommitProven) {
            phase_ = SessionPhase::Reconciling;
            disposition_ = CommitDisposition::CommittedNeedsReconciliation;
        } else {
            phase_ = SessionPhase::PersistenceFaulted;
        }
        publish_operation_active_ = false;
        active_operation_id_ = 0;
        active_operation_stop_source_ = std::stop_source{};
        active_publication_stage_ = PublicationStage::PrePublication;
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
    // request_stop() synchronously invokes callbacks. Copy the shared stop
    // state, then invoke callbacks without holding the session mutex so a
    // callback may safely finish/abort its lease.
    auto operation_stop_source = active_operation_stop_source_;
    lock.unlock();
    operation_stop_source.request_stop();
    lock.lock();
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
