#pragma once

#include "src/common/result.h"
#include "src/host/m5_worker_protocol.h"

#include <cstdint>
#include <optional>

namespace myrd::m5 {

// What the worker session drives.
//
// Production implements this over `DiskRuntime` built by the shared
// `make_runtime_operations()` adapter, so the worker and the foreground CLI run
// one execution implementation rather than two.  Keeping it an interface is
// what lets the session's authentication, sequencing and two-phase state be
// tested without a driver, a volume, or a bundle.
class WorkerRuntime {
public:
    virtual ~WorkerRuntime() = default;

    virtual Result<void> start() = 0;
    virtual Result<WorkerRuntimeStatus> observe() = 0;
    virtual Result<WorkerPreparedCandidate> prepare_checkpoint(
        std::uint64_t native_operation_id) = 0;
    // The commit answer: the disposition the persistence layer actually
    // returned, the post-commit observation, and -- when the commit did not
    // cleanly land -- the failure that decided it.
    //
    // Returning a bare status here was a defect, not a simplification: a status
    // carries a generation, and the three unsuccessful dispositions cannot be
    // derived from one.  "Nothing was published, restore the prior record" and
    // "the outcome is unknown, park the operation" want opposite handling, and a
    // reply that cannot tell them apart forces the caller to guess at the one
    // moment guessing is most expensive.
    virtual Result<WorkerCheckpointCommitReply> commit_checkpoint(
        const WorkerCheckpointCommit& commit) = 0;
    // The abort answer.  A bare status cannot express it: `aborted == false`
    // covers both "there was nothing to cancel" and "the candidate could not be
    // cancelled", and the first leaves the bundle untouched while the second
    // forces a reconciliation.  The generation rides along because the caller
    // checks that a successful abort did not move it.
    virtual Result<WorkerCheckpointAbortReply> abort_checkpoint(
        std::uint64_t native_operation_id) = 0;
    // Judges what actually reached the bundle against the P4 correlation.  The
    // originating worker's in-process transaction is gone, so this may only
    // report one of the three designed outcomes -- never a synthesized success.
    //
    // The correlation is the persistence layer's own type, so the implementation
    // forwards it to the read-only M3 inspection unchanged rather than
    // translating a parallel field set.
    virtual Result<WorkerRecoveryProof> prove_recovery(
        const WorkerRecoveryCorrelation& correlation) = 0;
    // Removes this profile's *verified own* resources and stops.  The
    // implementation is expected to re-check the authorization against what it
    // actually holds; the session's gate refuses first, so a runtime is never
    // asked to remove something a partial proof was presented for.
    // The removal answer, reporting the generation the removal happened at and,
    // when it did not happen, the reason as a first-class refusal rather than a
    // generic failure.  A refusal is a decision this runtime reached about its
    // own state and the caller has to act on the reason; a bare status could
    // express neither the generation nor the reason, and an error could not be
    // told apart from a broken channel.
    virtual Result<WorkerRemoveAndStopReply> remove_and_stop(
        const WorkerRemovalAuthorization& authorization) = 0;
    // The service's instruction to terminate.  It deliberately dismantles
    // nothing: the ordered teardown stays `cleanup`'s job, so a caller can tell
    // the worker to go away and still get a truthful teardown report from it.
    virtual Result<void> shutdown() = 0;
    virtual Result<void> cleanup() = 0;
};

// The worker's command loop.
//
// It is deliberately transport-free: it takes a decoded message and returns the
// reply, so the same code runs whether the bytes arrived over a named pipe or
// from a test.  Three invariants live here rather than in the transport:
//
//   1. Every request is authenticated before anything is executed.  A refused
//      request produces an error reply and no partial execution.
//   2. A prepare is tracked so a commit that does not repeat the frozen
//      candidate is refused and the candidate stays prepared.
//   3. `cleanup` runs the runtime's reverse-order teardown exactly once, and a
//      command arriving after it is refused rather than served against a
//      dismantled runtime.
class WorkerSession final {
public:
    // `startup_secret` is the value this session delivered through the worker's
    // inherited startup channel.  Requiring it in the handshake is what makes
    // "the worker I launched" different from "anything that can open the pipe".
    WorkerSession(WorkerBinding binding, std::uint64_t session_nonce,
                  StartupSecret startup_secret, WorkerRuntime& runtime) noexcept;

    [[nodiscard]] Result<WorkerMessage> handle(const WorkerMessage& request);

    [[nodiscard]] const WorkerBinding& binding() const noexcept {
        return binding_;
    }
    [[nodiscard]] std::uint64_t last_accepted_sequence() const noexcept {
        return last_accepted_sequence_;
    }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool checkpoint_prepared() const noexcept {
        return prepared_.has_value();
    }
    [[nodiscard]] bool cleaned_up() const noexcept { return cleaned_up_; }
    // The service asked this worker to terminate.  The transport's read loop
    // owns the process, so the session reports the request rather than acting on
    // it; `cleanup` may still run afterwards, which is why shutdown is not a
    // synonym for it.
    [[nodiscard]] bool shutdown_requested() const noexcept {
        return shutdown_requested_;
    }

private:
    Result<WorkerMessage> dispatch(const WorkerMessage& request);
    // Builds the reply envelope: same command, same binding, the request's
    // operation id, and the next sequence.
    [[nodiscard]] WorkerMessage make_reply(const WorkerMessage& request);
    [[nodiscard]] Error refusal(const wchar_t* operation,
                                const wchar_t* message) const;

    WorkerBinding binding_;
    std::uint64_t session_nonce_{};
    StartupSecret startup_secret_;
    WorkerRuntime& runtime_;
    std::uint64_t last_accepted_sequence_{};
    std::uint64_t next_reply_sequence_{1};
    bool started_{};
    bool cleaned_up_{};
    bool shutdown_requested_{};
    std::optional<WorkerPreparedCandidate> prepared_;
};

} // namespace myrd::m5
