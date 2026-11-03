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

// The absolute bound the session will ever put on a worker request.
//
// **Only a bundle-creating action asks for more than the ordinary bound**, and it has to: its
// cost is set by the disk, not by the adapter.  Measured on the guest, a create's generation-zero
// checkpoint takes 98 s at 8 GiB and 123 s at 16 GiB, and reading that journal back to verify the
// bundle's identity adds ~20-40 s -- so the 120 s ordinary bound abandoned a worker that was
// still working and the operation ended `CommitIndeterminate`.  The host reading of the same
// defect is `code=4107 step=WorkerPipeChannel::read detail=pipe transfer exceeded its bounded
// wait`.  A request longer than this is still refused before it is sent (`require_bounded`), and
// the worker's own command deadline is the same value, so neither side can be waiting on a
// request the other has already abandoned.
inline constexpr std::uint64_t kProvisioningWorkerCommandTimeoutMs = 900'000;

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
    // Asks the worker to write its durable image to `destination`.  The reply is
    // the worker's observation of the bundle it exported; the file is verified by
    // the worker's reader before the command succeeds.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> export_bundle(
        const OperationContext& context,
        const product::Uuid& operation_id,
        const std::wstring& destination);

    // Takes the worker's volume write freeze and keeps it.  The reply's
    // `writes_frozen` is required, not consulted: a success that did not carry it
    // would report a protected commit over a volume that is still writable.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> freeze_writes(
        const OperationContext& context, const product::Uuid& operation_id);
    // Releases the freeze without dismantling anything, for a stop that is being
    // rolled back.  The reply is required to report the freeze as *gone*: a
    // success that left the volume locked would record a resumed profile that
    // cannot be written to.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> release_write_freeze(
        const OperationContext& context, const product::Uuid& operation_id);

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
    // Dismantles a leftover this service proved is its own.  Requires that `start` already
    // adopted the volume -- the worker refuses otherwise -- so a reclaim that adopted
    // nothing cannot report a still-mounted residue as dismantled.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> reclaim_leftover(
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
    // Opens the bundle read-only and returns the evidence an import registers.
    // Separate from `create_bundle` because it starts no runtime: recovering a
    // bundle would claim a drive letter, and an import registers a bundle it is
    // not starting.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> inspect_bundle(
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

    // The one implementation behind `create_bundle` and `inspect_bundle`.  They
    // differ only in which command they send -- one runs a lifecycle, the other
    // opens the bundle and stops -- and both must be answered with the same
    // evidence or neither may be recorded.
    [[nodiscard]] Result<m5::WorkerRuntimeStatus> bundle_registration(
        const OperationContext& context, const product::Uuid& operation_id,
        m5::WorkerCommand command);

    std::unique_ptr<m5::WorkerPipeChannel> channel_;
    m5::WorkerBinding binding_;
    // 1 was the handshake; the worker refuses anything not strictly greater than
    // what it has accepted, so this must only advance on an accepted request.
    std::uint64_t next_sequence_{2};
};

} // namespace myrd::management
