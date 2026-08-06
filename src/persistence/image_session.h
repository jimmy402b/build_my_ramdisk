#pragma once

#include "persistence_types.h"

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
};

struct SessionSnapshot final {
    SessionPhase phase{SessionPhase::Closed};
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    std::optional<PersistenceFailure> first_failure;
    std::size_t active_immutable_reads{0};
    bool publish_operation_active{false};
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

/// Move-only lease for the single publish-affecting operation gate. The owning
/// ImageSession must outlive every lease, as enforced by the lifecycle owner.
class PublishOperationLease final {
public:
    PublishOperationLease() = delete;
    PublishOperationLease(const PublishOperationLease&) = delete;
    PublishOperationLease& operator=(const PublishOperationLease&) = delete;
    PublishOperationLease(PublishOperationLease&& other) noexcept;
    PublishOperationLease& operator=(PublishOperationLease&& other) noexcept;
    ~PublishOperationLease();

    [[nodiscard]] SessionOperation operation() const noexcept { return operation_; }
    [[nodiscard]] bool active() const noexcept { return session_ != nullptr; }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_token_; }

    /// Finish the operation at a durable boundary. `durable_verified` must
    /// come from actual flush/readback evidence; an in-memory state change is
    /// never sufficient to report durable success.
    Result<SessionOutcome> finish(
        CommitDisposition disposition,
        bool durable_verified,
        std::optional<PersistenceFailure> failure = std::nullopt);

    /// Abort before publication. The destructor uses the same conservative
    /// path if a caller forgets to finish explicitly.
    Result<SessionOutcome> abort();

private:
    friend class ImageSession;
    PublishOperationLease(ImageSession* session,
                           SessionOperation operation,
                           std::uint64_t operation_id,
                           std::stop_token stop_token) noexcept;
    void abandon_noexcept() noexcept;

    ImageSession* session_;
    SessionOperation operation_;
    std::uint64_t operation_id_;
    std::stop_token stop_token_;
};

/// Move-only read lease. Immutable reads do not contend with the publish gate,
/// but close waits for them before releasing the session's persistence state.
/// The owning ImageSession must outlive every lease.
class ImmutableReadLease final {
public:
    ImmutableReadLease() = delete;
    ImmutableReadLease(const ImmutableReadLease&) = delete;
    ImmutableReadLease& operator=(const ImmutableReadLease&) = delete;
    ImmutableReadLease(ImmutableReadLease&& other) noexcept;
    ImmutableReadLease& operator=(ImmutableReadLease&& other) noexcept;
    ~ImmutableReadLease();

    [[nodiscard]] bool active() const noexcept { return session_ != nullptr; }
    void release() noexcept;

private:
    friend class ImageSession;
    explicit ImmutableReadLease(ImageSession* session) noexcept
        : session_(session) {}

    ImageSession* session_;
};

/// Lifecycle and concurrency contract for one persistent image.
///
/// This class owns no files and performs no checkpoint/compaction work. It is
/// the serialized state boundary that those algorithms must use in Tasks
/// 30–33. Ordinary immutable backing reads may proceed while a publish
/// operation is active, resolving, reconciling or persistence-faulted.
class ImageSession final {
public:
    explicit ImageSession(SessionPhase initial_phase = SessionPhase::Closed);
    ImageSession(const ImageSession&) = delete;
    ImageSession& operator=(const ImageSession&) = delete;
    ~ImageSession() = default;

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
        bool durable_verified,
        std::optional<PersistenceFailure> failure);
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
    bool close_requested_{false};
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
        return to == SessionPhase::Ready || to == SessionPhase::Closing ||
               to == SessionPhase::PersistenceFaulted;
    case SessionPhase::Closing:
        return to == SessionPhase::Closed || to == SessionPhase::PersistenceFaulted;
    case SessionPhase::PersistenceFaulted:
        return to == SessionPhase::Closing;
    }
    return false;
}

} // namespace myrd::persistence
