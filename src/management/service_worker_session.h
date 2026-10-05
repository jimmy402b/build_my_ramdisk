#pragma once

// The service's end of one authenticated private worker channel.
//
// This is a rebuild, not a port: the historical file of this name drove
// `host::HostMessage` over HostCore, and both of those are prohibited imports.
// What it keeps is the shape -- one method per private command, the request and
// the accepted reply stated in the same place so they cannot disagree -- and the
// reason for that shape, which is that a reply can be wrong in three ways that
// have each been a real defect in this codebase:
//
//   * a different command (the peer misread the request),
//   * a different operation id (the answer belongs to another operation),
//   * a mismatched binding (the answer came from another worker).
//
// A reply is only accepted when all three match, and a reply carrying an error
// is returned as a failure rather than as a payload -- the worker's reason is
// what the caller has to act on, and a success payload read past a reported
// failure is exactly how a refusal becomes a recorded success.

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/host/m5_worker_private_pipe.h"
#include "src/host/m5_worker_protocol.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>

namespace myrd::management {

// A worker answers inside its own bounded wait; a request built with a deadline
// beyond the service's own budget could never be answered at all, so the
// session refuses to send one rather than discovering that as a timeout.
inline constexpr std::uint64_t kMaximumWorkerCommandTimeoutMs = 120'000;

class ServiceWorkerSession final {
public:
    // The channel must already be authenticated: `WorkerPrivatePipeServer::
    // accept_and_handshake` is what produces one, and it is what proves the peer
    // read the startup secret.  A session is never constructed over a raw pipe.
    [[nodiscard]] static Result<std::unique_ptr<ServiceWorkerSession>> create(
        std::unique_ptr<m5::WorkerPipeChannel> channel,
        m5::WorkerBinding binding);

    ~ServiceWorkerSession() noexcept;
    ServiceWorkerSession(const ServiceWorkerSession&) = delete;
    ServiceWorkerSession& operator=(const ServiceWorkerSession&) = delete;

    [[nodiscard]] const m5::WorkerBinding& binding() const noexcept {
        return binding_;
    }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept {
        return next_sequence_ - 1;
    }

    // No product operation id: a status query is not attributable to one, and
    // the protocol's shape table does not require one of it.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> status(
        const OperationContext& context);
    // Strict, unlike `status`: "could not observe a live runtime" is a failure,
    // so a caller deciding whether a mutation is safe never reads it as clean.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> require_runtime_observation(
        const OperationContext& context);

    // The native operation id is deliberately *not* a parameter.  The worker
    // derives it from the generation it observes and re-derives it from M3's
    // reply; a service-supplied id would be a second source for the same number,
    // which is exactly what the one shared rule exists to prevent.
    [[nodiscard]] Result<m5::WorkerPreparedCandidate> prepare_checkpoint(
        const OperationContext& context, const product::Uuid& operation_id);
    // Returns the whole commit reply, not a bare status: the disposition and the
    // failure evidence are what the caller acts on, and a status cannot carry
    // either.
    [[nodiscard]] Result<m5::WorkerCheckpointCommitReply> commit_checkpoint(
        const OperationContext& context, const product::Uuid& operation_id,
        const m5::WorkerCheckpointCommit& commit);
    // Returns the abort answer rather than an observation, so a caller can tell
    // "there was nothing to cancel" from "the candidate could not be cancelled"
    // and can check that a successful abort left the generation where it was.
    [[nodiscard]] Result<m5::WorkerCheckpointAbortReply> abort_checkpoint(
        const OperationContext& context, const product::Uuid& operation_id,
        std::uint64_t native_operation_id);

    // Judged by the worker against the bundle, never by the service against its
    // own durable record: that is the whole point of the proof.
    [[nodiscard]] Result<m5::WorkerRecoveryProof> recovery_proof(
        const OperationContext& context,
        const m5::WorkerRecoveryCorrelation& correlation);

    // Returns the removal answer rather than an observation: the generation the
    // removal happened at, and on a refusal the reason, are what the caller
    // decides on -- a status carries neither.
    [[nodiscard]] Result<m5::WorkerRemoveAndStopReply> remove_and_stop(
        const OperationContext& context, const product::Uuid& operation_id,
        const m5::WorkerRemovalAuthorization& authorization);

    // Asks the worker to terminate.  Dismantles nothing, so `cleanup` is still
    // available afterwards -- the two are not synonyms.
    [[nodiscard]] Result<void> shutdown(const OperationContext& context,
                                        const product::Uuid& operation_id);
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> cleanup(
        const OperationContext& context, const product::Uuid& operation_id);
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> start(
        const OperationContext& context, const product::Uuid& operation_id);
    // Brings the runtime up and requires the bundle evidence that makes a
    // registration possible.  Separate from `start` even though the worker
    // reaches the same runtime call for both: this is the one the service sends
    // to a worker it launched from a lifecycle intent, and the one whose reply
    // it records as the bundle's identity.  A reply without complete evidence is
    // returned as a failure rather than as a status the caller could register.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> create_bundle(
        const OperationContext& context, const product::Uuid& operation_id);

private:
    ServiceWorkerSession(std::unique_ptr<m5::WorkerPipeChannel> channel,
                         m5::WorkerBinding binding) noexcept;

    // Sends one request and returns the reply that answers it, or fails.  The
    // caller has already checked the deadline and filled in the payload.
    [[nodiscard]] Result<m5::WorkerMessage> exchange(
        const OperationContext& context, m5::WorkerMessage request);

    [[nodiscard]] Result<void> require_bounded(
        const OperationContext& context) const;

    std::unique_ptr<m5::WorkerPipeChannel> channel_;
    m5::WorkerBinding binding_;
    // 1 was the handshake; the worker refuses anything not strictly greater than
    // what it has accepted, so this must only advance on an accepted request.
    std::uint64_t next_sequence_{2};
};

} // namespace myrd::management
