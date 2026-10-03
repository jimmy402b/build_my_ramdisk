#pragma once

// The production execution backend between ServiceCore and the per-profile
// worker.
//
// This is a rebuild, not a port.  The historical file of this name drove
// `host::HostMessage` over HostCore and included `host_ipc.h`,
// `host_protocol.h` and `windows_service.h`, all of which the transfer manifest
// prohibits.  What it keeps is the *action mapping*, restated below action by
// action, because that mapping is the accumulated knowledge of which durable
// step is realized by which worker command -- and the reason that knowledge is
// written down here is that inventing a step the worker did not perform is the
// failure this adapter exists to prevent.
//
// Realized, with the proof named:
//
//   create_runtime_and_worker  -> launch and authenticate a worker process
//                                 through the provider seam below.
//   recover_bundle             -> adopt the worker already bound to the
//                                 profile, and verify the binding.
//   start_proxy,
//   create_exact_device,
//   locate_exact_physical_drive,
//   provision_gpt_ntfs,
//   verify_existing_volume_identity,
//   record_volume_identity,
//   assign_exact_mount,
//   readiness_io               -> facts established by the authenticated `start`
//                                 that produced this worker's status.  The
//                                 worker performs this whole sequence inside
//                                 DiskWorkerRuntime::start() and answers only
//                                 once it has finished, and the answer is
//                                 authenticated and its image identity is
//                                 re-read from the bundle, so the reply is the
//                                 proof.
//                                 `verify_existing_volume_identity` and
//                                 `record_volume_identity` additionally compare
//                                 or return the volume's persistent GPT
//                                 identities, which now travel in the status.
//                                 Departure worth stating plainly: the
//                                 historical adapter *also* checked device
//                                 geometry (extent length) per step.  The
//                                 M5 status carries no geometry, so those checks
//                                 are gone -- a coarser proof boundary, chosen
//                                 deliberately rather than overlooked.
//   quiesce_mutations          -> require_runtime_observation, which fails rather
//                                 than reporting "could not ask" as clean, and
//                                 proves the live worker is the one this
//                                 operation's durable profile describes.
//   flush_exact_volume         -> checkpoint_prepare, which flushes the volume
//                                 and freezes the candidate the later commit
//                                 publishes.  The candidate is held for that
//                                 commit, or aborted when ServiceCore decides no
//                                 checkpoint is needed after all.
//   checkpoint,
//   checkpoint_if_required     -> commit or abort that candidate across
//                                 ServiceCore's pre-commit boundary callback.
//   resume_mutations           -> abort a candidate the stop abandoned.
//   remove_mount               -> remove_and_stop, after this adapter has itself
//                                 established the two facts the removal turns
//                                 on: the strict observation says the worker is
//                                 alive, and it says the runtime is not dirty.
//                                 See the removal note below -- the M5 worker no
//                                 longer refuses a dirty removal on its own, so
//                                 that check has to live here or nowhere.
//   remove_exact_device,
//   stop_runtime_and_release_lock
//                              -> no-ops *proven* by that removal.  Both steps
//                                 describe the same teardown the removal barrier
//                                 performs, and a reply that claimed a removal
//                                 the adapter had not proven is refused below.
//
// `prove_new_uninitialized` checks the durable mount target before a later
// start and needs no worker at all.
//
// `create_and_verify_bundle` and `inspect_bundle_read_only` are **fail-closed
// here, and that is a scope decision rather than a gap in the protocol.** The
// worker now produces the `BundleRegistrationEvidence` these two actions must
// return (§1.18), so the data exists; what is unsettled is *when*. The action
// plan places both **before** `create_runtime_and_worker`, while in M5 the
// bundle is created by the worker's own `start()` -- so satisfying them means
// either launching a worker at a point the plan treats as pre-worker, or
// re-cutting the plan's ordering. That is orchestration, and Task 6 owns
// `service_core.cpp` and the ordering table. Implementing one of the two
// options here would silently decide a Task 6 question in a Task 5 file, so the
// adapter refuses both actions with a reason and the seam stays visible.
//
// Everything else fails closed with `unrealized_action`:
//   compact, and the compaction family
//                              -> no worker command performs or reconciles a
//                                 compaction.
//   publish_live_config,
//   publish_stopped_config,
//   verify_rebind_bundle,
//   publish_rebind,
//   forget_metadata            -> no worker command performs any of these.
//   create_and_verify_bundle,
//   inspect_bundle_read_only   -> see above: the evidence exists, the ordering
//                                 does not, and the ordering is Task 6's.
//   reconcile_checkpoint       -> deliberately not overridden, so it keeps the
//                                 inherited fail-closed answer. `recovery_proof`
//                                 would realize it, but the correlation it needs
//                                 requires the prepared candidate's manifest slot
//                                 and bytes digest, and
//                                 `CheckpointCommitBoundaryEvidence` carries
//                                 neither -- so the adapter cannot build one from
//                                 what it is handed. §7 item 19 claimed this
//                                 action became realizable; that claim was wrong
//                                 and is corrected there.
//
// Reporting success for any of those would tell ServiceCore a mutation had
// happened when nothing had.  They are reported as blockers rather than papered
// over.
//
// `prove_forget_safe`, `execute_compaction`, `reconcile_compaction` and
// `publish_compaction_epoch` are deliberately not overridden, so they keep the
// inherited fail-closed answers.  The private worker IPC cannot prove a writer
// lock, a matching device or a bundle identity is absent -- and a forget
// reported safe without that proof is what forget exists to prevent -- and no
// worker command performs a compaction at all.
//
// Not a concurrent object, and it does not pretend to be one.  Its session map
// and its held-candidate list are unsynchronized: two threads calling `execute`
// at once would race on both, and a candidate could be aborted by one caller
// while another commits it.  That is deliberate rather than an omission.  A lock
// here would have to be held across a worker round-trip, which is the thing the
// layers above are already structured to serialize, and ServiceCore shares the
// same contract -- the same mutable profile and operation records sit behind it.
// The production wiring supplies the serialization at the pipe door; a future
// executor that calls these objects from a second thread must bring its own.

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/management/service_core.h"
#include "src/management/service_worker_session.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace myrd::management {

// A worker answers inside its own bounded wait, so the adapter's own budget is
// the same constant the session already refuses to exceed.
inline constexpr std::uint32_t kDefaultWorkerControlTimeoutMs =
    static_cast<std::uint32_t>(kMaximumWorkerCommandTimeoutMs);

enum class ServiceWorkerControlError : std::uint32_t {
    invalid_argument = 0x5C01,
    // The action needs a worker for this profile, none is held, and none could
    // be adopted.  Nothing was changed.
    execution_not_bound,
    // The runtime this authority can reach is not the one the operation's
    // durable profile describes.  Launching or adopting another would let two
    // runtimes claim one disk, so the action stops here instead.
    execution_identity_conflict,
    worker_reply_unexpected,
    // The action has no authenticated worker command that realizes it.  It fails
    // closed rather than reporting a mutation that never happened.
    unrealized_action,
    // The flush that opens a checkpoint boundary produced no candidate, so there
    // is nothing for a commit to publish.
    checkpoint_candidate_missing,
    // The worker refused to remove at all, or this adapter established that it
    // must not: its runtime stands dirty.  Removal drops every uncommitted
    // write, so the caller has to resolve that against durable truth rather than
    // have the removal forced.
    removal_refused,
    // A later step of the stop claims the device is gone or the runtime is
    // stopped, but no removal was proven for this operation to stand on.
    removal_unproven,
    // The worker refused to cancel a checkpoint candidate and said why.  Kept
    // apart from worker_reply_unexpected because the worker answered: the reply
    // is well-formed and carries the failure that stopped it, so the caller has
    // a reason to act on rather than a protocol violation to report.
    abort_refused,
    // The worker holds a volume but could not report its complete persistent
    // identity, so the service has nothing to record and must not invent one.
    volume_identity_unproven,
    // The worker removed at a generation other than the one this authority
    // proved committed.  Removal drops every uncommitted write, so a removal
    // proven at the wrong generation is not a removal this service may record.
    removal_generation_mismatch,
    // The durable correlation cannot support a recovery proof at all: it carries
    // no manifest slot and no bytes digest, or no native operation id.  A
    // correlation recorded before the schema minor that added them is the common
    // case.  Refused rather than answered, because a proof taken against the
    // wrong question is worse than none.
    recovery_not_provable,
};

// The seam between this adapter and whoever can actually start or find a worker
// process.
//
// It is separate from `IWorkerControl` because the mapping from an action plan
// onto worker commands is what this file *is*, and a test of that mapping should
// not have to spawn a process to observe it.  The production implementation
// lives with the service composition, which is the only layer allowed to reach
// CreateProcess and the private pipe server.
class IWorkerSessionProvider {
public:
    virtual ~IWorkerSessionProvider() noexcept = default;

    // Launches a worker for this profile and returns its authenticated session.
    // Failure means no worker is running for it and nothing was changed.
    //
    // Takes the durable record rather than a snapshot: the launch spec is built
    // from exactly the record's fields -- bundle path, image, size, mount target
    // -- and a snapshot would carry the mutable service state a launch must not
    // read.
    [[nodiscard]] virtual Result<std::unique_ptr<ServiceWorkerSession>> launch(
        const profile::ProfileRecord& profile,
        const OperationContext& context) = 0;
    // Adopts the worker already running for this profile.  A successful
    // `nullopt` means none is running -- which is an answer, not a failure.
    [[nodiscard]] virtual Result<std::unique_ptr<ServiceWorkerSession>> attach(
        const product::Uuid& profile_id, const OperationContext& context) = 0;
};

class ServiceWorkerControl final : public IWorkerControl {
public:
    explicit ServiceWorkerControl(
        IWorkerSessionProvider& sessions,
        std::uint32_t control_timeout_ms = kDefaultWorkerControlTimeoutMs) noexcept;

    // Always false, and deliberately so: declaring a boundary here would narrow
    // M4's cancellation semantics, which M5 may not change.  Nothing is lost by
    // the empty answer -- ServiceCore forces the stop removal noncancellable on
    // its own stop-removal-boundary rule rather than on this adapter's, and a
    // checkpoint cancel is resolved at the pre-commit boundary into a proven
    // abort.
    bool begins_noncancellable(ActionKind action) const noexcept override;
    Result<WorkerActionResult> execute(
        const WorkerActionRequest& request) override;
    Result<WorkerActionResult> execute_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryHandler& boundary) override;
    // Realized through `recovery_proof`, now that the durable correlation carries
    // the manifest slot and the bytes digest the proof needs.  Before that
    // widening it stayed fail-closed, because there was nothing to build a
    // correlation from -- not even from the durable record, which persists the
    // same type.
    Result<WorkerActionResult> reconcile_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation) override;
    Result<void> publish_checkpoint_epoch(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation,
        const CheckpointEpochPublicationHandler& publish) override;
    Result<std::uint64_t> current_dirty_epoch(
        const product::Uuid& profile_id) override;
    Result<std::optional<WorkerRuntimeObservation>> observe_runtime(
        const product::Uuid& profile_id) override;
    Result<WorkerRuntimeObservation> require_runtime_observation(
        const product::Uuid& profile_id) override;

private:
    // A candidate the volume flush cut and a checkpoint will publish.  It is
    // keyed by the operation that cut it so two profiles stopping at once can
    // never commit each other's snapshot.
    //
    // The prepared candidate is held alongside the boundary evidence because
    // they carry different halves.  `CheckpointCommitBoundaryEvidence` is what
    // the durable layer persists as P4, and it has no manifest slot and no bytes
    // digest -- but the worker refuses a commit that does not repeat exactly the
    // candidate prepare froze, so the adapter has to be the one that remembers
    // them.  Without this the commit could never be issued at all.
    struct HeldCandidate final {
        product::Uuid profile_id;
        product::Uuid operation_id;
        CheckpointCommitBoundaryEvidence evidence;
        m5::WorkerPreparedCandidate prepared;
    };

    // The generation a removal may be authorized at, and -- once the removal
    // actually happened -- the fact that it did.
    //
    // The two are deliberately distinct.  Quiescing witnesses the generation the
    // live runtime stands at, which is the *authority* a removal may be performed
    // at; it says nothing about whether the removal happened.  `remove_exact_device`
    // and `stop_runtime_and_release_lock` may only be reported as done on a
    // removal that was proven, so a quiesce followed by a removal the worker
    // refused must not license them.
    //
    // The witness moves with a commit this adapter itself performed: a normal
    // stop checkpoints between its flush and its removal, so a frozen witness
    // would authorize the removal at a generation the stop has already left.
    struct HeldRemoval final {
        product::Uuid profile_id;
        product::Uuid operation_id;
        std::uint64_t generation{};
        bool proven{};
    };

    [[nodiscard]] OperationContext context() const;
    [[nodiscard]] ServiceWorkerSession* find_session(
        const product::Uuid& profile_id) noexcept;
    // The session for this profile, adopting an existing worker when none is
    // held and `attach_if_absent` allows it.  Never launches.
    // Takes the profile rather than the whole request: `WorkerActionRequest`
    // holds three `product::Uuid`s and so cannot be value-initialized, which
    // makes it useless as a carrier for a call that only needs an identity.
    Result<ServiceWorkerSession*> require_session(
        const product::Uuid& profile_id, bool attach_if_absent);
    Result<WorkerActionResult> verify_binding(
        const WorkerActionRequest& request,
        ServiceWorkerSession& session) const;
    std::optional<HeldCandidate> take_candidate(
        const product::Uuid& profile_id, const product::Uuid& operation_id);
    std::optional<HeldRemoval> find_removal(
        const product::Uuid& profile_id,
        const product::Uuid& operation_id) const noexcept;
    void advance_held_removal(const product::Uuid& profile_id,
                              const product::Uuid& operation_id,
                              std::uint64_t generation) noexcept;
    void forget_session(const product::Uuid& profile_id) noexcept;

    Result<WorkerActionResult> bind_worker(
        const WorkerActionRequest& request, bool launch_if_absent);
    Result<WorkerActionResult> verify_startup_step(
        const WorkerActionRequest& request);
    Result<WorkerActionResult> quiesce_mutations(
        const WorkerActionRequest& request);
    Result<WorkerActionResult> flush_exact_volume(
        const WorkerActionRequest& request);
    Result<WorkerActionResult> remove_mount(const WorkerActionRequest& request);
    Result<WorkerActionResult> confirm_removal(
        const WorkerActionRequest& request);
    Result<WorkerActionResult> resume_mutations(
        const WorkerActionRequest& request);
    Result<WorkerActionResult> unrealized(
        const WorkerActionRequest& request) const;
    Result<HeldCandidate> cut_candidate(ServiceWorkerSession& session,
                                        const WorkerActionRequest& request);
    // Resolve a candidate the service decided not to publish.  Only an abort the
    // worker confirmed leaves the committed generation untouched.
    Result<WorkerActionResult> abort_held_candidate(
        ServiceWorkerSession& session, const HeldCandidate& candidate);
    Result<WorkerActionResult> checkpoint_across_boundary(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryHandler& boundary);

    IWorkerSessionProvider& sessions_;
    std::uint32_t control_timeout_ms_{};
    std::vector<std::pair<product::Uuid, std::unique_ptr<ServiceWorkerSession>>>
        sessions_by_profile_;
    std::vector<HeldCandidate> held_candidates_;
    std::vector<HeldRemoval> held_removals_;
};

} // namespace myrd::management
