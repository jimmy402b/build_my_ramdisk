#pragma once

#include "persistence_types.h"

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <stop_token>

namespace myrd::persistence {

/// The only operations which may publish or otherwise change durable image
/// state. There is deliberately one gate for checkpoint and compaction.
enum class SessionOperation : std::uint8_t {
    Checkpoint = 0,
    Compaction = 1,
    CommitResolution = 2,
    Reconciliation = 3,
    Cleanup = 4,
};

enum class PublicationStage : std::uint8_t {
    PrePublication = 0,
    PublishAttempted = 1,
    CommitProven = 2,
};

struct SessionSnapshot final {
    SessionPhase phase{SessionPhase::Closed};
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    std::optional<PersistenceFailure> first_failure;
    std::size_t active_immutable_reads{0};
    bool publish_operation_active{false};
    PublicationStage publication_stage{PublicationStage::PrePublication};
    bool close_requested{false};
};

struct SessionOutcome {
    SessionPhase phase{SessionPhase::Closed};
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    bool durable_success{false};
    std::optional<PersistenceFailure> first_failure;
};

struct SessionCloseOutcome final : SessionOutcome {
    bool already_closed{false};
};

class ImageSession;
struct ImageSessionLeaseAnchor;

/// Move-only lease for the single publish-affecting operation gate. A lease
/// fails safely if its owner was already destroyed; concurrent owner
/// destruction is unsupported, so the lifecycle owner must still drain/join.
class PublishOperationLease final {
public:
    PublishOperationLease() = delete;
    PublishOperationLease(const PublishOperationLease&) = delete;
    PublishOperationLease& operator=(const PublishOperationLease&) = delete;
    PublishOperationLease(PublishOperationLease&& other) noexcept;
    PublishOperationLease& operator=(PublishOperationLease&& other) noexcept;
    ~PublishOperationLease();

    [[nodiscard]] SessionOperation operation() const noexcept { return operation_; }
    [[nodiscard]] bool active() const noexcept { return anchor_ != nullptr; }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_token_; }

    /// Record the point after which NotCommitted is no longer a safe claim.
    Result<void> note_publish_attempt_started();

    /// Record that successful manifest FlushFileBuffers proved the new
    /// generation durable. Task 31 must call this immediately at that commit
    /// point; a later readback failure is reconciliation, not indeterminate.
    Result<void> note_durable_commit_proven();

    /// Finish with a disposition compatible with the recorded publication
    /// stage. An in-memory phase change alone cannot produce Committed.
    Result<SessionOutcome> finish(
        CommitDisposition disposition,
        std::optional<PersistenceFailure> failure = std::nullopt);

    /// Abort before publication. The destructor uses the same conservative
    /// path if a caller forgets to finish explicitly.
    Result<SessionOutcome> abort();

private:
    friend class ImageSession;
    PublishOperationLease(std::shared_ptr<ImageSessionLeaseAnchor> anchor,
                           SessionOperation operation,
                           std::uint64_t operation_id,
                           std::stop_token stop_token) noexcept;
    void abandon_noexcept() noexcept;

    std::shared_ptr<ImageSessionLeaseAnchor> anchor_;
    SessionOperation operation_;
    std::uint64_t operation_id_;
    std::stop_token stop_token_;
};

/// Move-only read lease. Immutable reads do not contend with the publish gate,
/// but close waits for them before releasing the session's persistence state.
/// Owner-first destruction is detected safely; concurrent owner destruction
/// remains outside the supported lifecycle and must be prevented by drain/join.
class ImmutableReadLease final {
public:
    ImmutableReadLease() = delete;
    ImmutableReadLease(const ImmutableReadLease&) = delete;
    ImmutableReadLease& operator=(const ImmutableReadLease&) = delete;
    ImmutableReadLease(ImmutableReadLease&& other) noexcept;
    ImmutableReadLease& operator=(ImmutableReadLease&& other) noexcept;
    ~ImmutableReadLease();

    [[nodiscard]] bool active() const noexcept { return anchor_ != nullptr; }
    void release() noexcept;

private:
    friend class ImageSession;
    explicit ImmutableReadLease(
        std::shared_ptr<ImageSessionLeaseAnchor> anchor) noexcept
        : anchor_(std::move(anchor)) {}

    std::shared_ptr<ImageSessionLeaseAnchor> anchor_;
};

/// Lifecycle and concurrency contract for one persistent image.
///
/// This class owns no files and performs no checkpoint/compaction work. It is
/// the serialized state boundary that those algorithms must use in Tasks
/// 30–33. Ordinary immutable backing reads may proceed while a publish
/// operation is active, resolving, reconciling or persistence-faulted.
class ImageSession final {
public:
    ImageSession();
    [[nodiscard]] static ImageSession ready();
    ImageSession(const ImageSession&) = delete;
    ImageSession& operator=(const ImageSession&) = delete;
    ~ImageSession();

    /// Normative exhaustive transition table. It is intentionally pure so it
    /// can be independently checked without touching session state.
    [[nodiscard]] static constexpr bool is_valid_transition(
        SessionPhase from, SessionPhase to) noexcept;

    [[nodiscard]] SessionPhase phase() const;
    [[nodiscard]] SessionSnapshot snapshot() const;

    Result<PublishOperationLease> begin_checkpoint();
    Result<PublishOperationLease> begin_compaction();
    Result<PublishOperationLease> begin_commit_resolution();
    Result<PublishOperationLease> begin_reconciliation();
    // Cleanup holds the same single publish-affecting gate but never advances
    // the manifest publication stage. Its lease is completed with abort() once
    // best-effort immutable artifact deletion finishes.
    Result<PublishOperationLease> begin_cleanup();
    Result<ImmutableReadLease> begin_immutable_read();

    /// Idempotent close. It waits for the active publish operation and all
    /// immutable reads to drain, then closes the lifecycle state. It does not
    /// claim that RAM contents were durably checkpointed.
    Result<SessionCloseOutcome> close(
        const OperationContext& context = OperationContext{});

private:
    friend class PublishOperationLease;
    friend class ImmutableReadLease;

    Result<PublishOperationLease> begin_operation(SessionOperation operation);
    Result<SessionOutcome> finish_operation(
        std::uint64_t operation_id,
        SessionOperation operation,
        CommitDisposition disposition,
        std::optional<PersistenceFailure> failure);
    Result<void> advance_publication_stage(
        std::uint64_t operation_id,
        SessionOperation operation,
        PublicationStage target);
    Result<SessionOutcome> abandon_operation(
        std::uint64_t operation_id, SessionOperation operation);
    void abandon_operation_noexcept(
        std::uint64_t operation_id, SessionOperation operation) noexcept;
    void release_read() noexcept;

    static Error state_error(PersistenceErrorCode code,
                             const wchar_t* operation,
                             const wchar_t* message);
    SessionOutcome outcome_locked(bool durable_success) const;
    void notify_if_drained_locked() noexcept;

    explicit ImageSession(SessionPhase initial_phase);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    SessionPhase phase_;
    CommitDisposition disposition_{CommitDisposition::NotCommitted};
    std::optional<PersistenceFailure> first_failure_;
    std::size_t active_immutable_reads_{0};
    bool publish_operation_active_{false};
    SessionOperation active_operation_{};
    std::uint64_t active_operation_id_{0};
    std::uint64_t next_operation_id_{1};
    std::stop_source active_operation_stop_source_{};
    PublicationStage active_publication_stage_{PublicationStage::PrePublication};
    bool close_requested_{false};
    std::shared_ptr<ImageSessionLeaseAnchor> lease_anchor_;
};

constexpr bool ImageSession::is_valid_transition(
    const SessionPhase from, const SessionPhase to) noexcept {
    switch (from) {
    case SessionPhase::Closed:
        return to == SessionPhase::Opening;
    case SessionPhase::Opening:
        return to == SessionPhase::Ready || to == SessionPhase::Closing ||
               to == SessionPhase::Closed;
    case SessionPhase::Ready:
        return to == SessionPhase::Checkpointing ||
               to == SessionPhase::Compacting || to == SessionPhase::Verifying ||
               to == SessionPhase::Closing || to == SessionPhase::PersistenceFaulted;
    case SessionPhase::Checkpointing:
    case SessionPhase::Compacting:
        return to == SessionPhase::Ready ||
               to == SessionPhase::ResolvingCommit ||
               to == SessionPhase::Reconciling ||
               to == SessionPhase::PersistenceFaulted;
    case SessionPhase::Verifying:
        return to == SessionPhase::Ready || to == SessionPhase::Closing ||
               to == SessionPhase::PersistenceFaulted;
    case SessionPhase::ResolvingCommit:
        return to == SessionPhase::Ready ||
               to == SessionPhase::Reconciling ||
               to == SessionPhase::Closing ||
               to == SessionPhase::PersistenceFaulted;
    case SessionPhase::Reconciling:
        return to == SessionPhase::Ready ||
               to == SessionPhase::Reconciling ||
               to == SessionPhase::Closing ||
               to == SessionPhase::PersistenceFaulted;
    case SessionPhase::Closing:
        return to == SessionPhase::Closed || to == SessionPhase::PersistenceFaulted;
    case SessionPhase::PersistenceFaulted:
        return to == SessionPhase::Closing;
    }
    return false;
}

} // namespace myrd::persistence
