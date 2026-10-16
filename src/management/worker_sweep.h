#pragma once

// The startup sweep: what the service does about the workers its predecessor left
// behind.
//
// Design spec section 5.4 gives it five steps -- open the job, enumerate, check
// each pid's creation time against the recorded binding, terminate what is not
// ours, and then deal with what the previous life left on the system.  The
// *decisions* those steps make live in `worker_sweep_policy.h` and
// `worker_process_source.h` and are tested there; this is the part that carries
// them out and reports what happened.
//
// It is separate from `ServiceHost` because it is the one start-up step that
// touches other processes, and a host that ran it inline could not be tested
// without a job object and a live orphan.  With the seam here, the execution
// order, the escalation and the report are all ordinary tests over fakes.
//
// **What this does not do, yet.**  It terminates and adopts; it does not reclaim
// the AIM device or the drive letter a killed worker left behind.  That needs the
// persisted last-observed identity (design section 5.5) which is the next slice,
// and the reclamation is deliberately not stubbed out here -- a sweep that
// claimed to have reclaimed something it had only decided about would be worse
// than one that says nothing.

#include "src/common/result.h"
#include "src/management/worker_process_source.h"
#include "src/management/worker_sweep_policy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace myrd::management {

// What the sweep did, in enough detail to be logged and asserted on.
struct WorkerSweepReport final {
    // Processes the job held that this session had not launched.
    std::uint32_t orphans_observed{};
    // Asked to shut down, escalated, or adopted.
    std::uint32_t asked_to_terminate{};
    std::uint32_t forced_to_terminate{};
    std::uint32_t adopted{};
    // Left alone because the pid could not be attributed.  Non-zero means a
    // human has to look: the sweep refuses to act on an identity it cannot
    // establish, and that refusal is the safety property, not a failure.
    std::uint32_t left_alone{};
    // Terminations that did not succeed even after escalation.  A non-zero count
    // is a needs-attention condition: a worker that will not die still holds its
    // device.
    std::uint32_t failed_to_terminate{};
    // Profiles whose leftover resources could be *proven* to be theirs, so a
    // reclamation may act on them.  Counted separately from the process counts
    // because a process and a device are different leaks: a worker that is gone can
    // still have left a mounted volume behind.
    std::uint32_t reclaimable_profiles{};
    // Profiles with leftover resources that could **not** be proven.  This is the
    // count that matters most and it is not a failure of the sweep: design section
    // 5.5 says an unprovable attribution must be *reported* rather than acted on,
    // and a sweep that silently reclaimed nothing here would look identical to one
    // that found nothing to do.
    std::uint32_t unprovable_profiles{};
    // The first failure's text, for the operator.  Kept as text rather than as an
    // error object because the sweep is not itself a failure when one process
    // refuses: it reports and moves on, which is the difference between a sweep
    // and an all-or-nothing operation.
    std::wstring first_failure;

    [[nodiscard]] bool needs_attention() const noexcept {
        return left_alone != 0 || failed_to_terminate != 0
            || unprovable_profiles != 0;
    }
    [[nodiscard]] std::uint32_t acted_on() const noexcept {
        return asked_to_terminate + forced_to_terminate;
    }
    bool operator==(const WorkerSweepReport&) const noexcept = default;
};

// Supplies the bindings the sweep judges the job's processes against.
//
// An interface rather than a store reference so the sweep's orchestration can be
// tested without a durable store, and so the eventual identity persistence has
// one place to plug into.  Returning an empty vector is a legitimate answer and
// means "no profile is expected to have a running worker" -- which makes every
// process in the job foreign and terminates it, the conservative reading.
class IExpectedWorkerBindings {
public:
    virtual ~IExpectedWorkerBindings() noexcept = default;
    [[nodiscard]] virtual Result<std::vector<ExpectedWorkerBinding>> expected()
        const = 0;
};

// Supplies what the durable records observed about one profile's resources.
//
// The sweep needs this to answer design section 5.5's question -- "may I clean up
// what this profile left behind?" -- and it is a separate seam from the bindings
// because the two come from different places and fail differently: a missing
// binding makes a *process* unattributable, a missing identity makes a *device*
// unattributable, and a sweep must be able to report either without the other.
class IObservedResourceIdentitySource {
public:
    virtual ~IObservedResourceIdentitySource() noexcept = default;

    // `nullopt` means no record exists for the profile -- which is a legitimate
    // answer and a different one from an incomplete identity, exactly as
    // `WorkerBindingStore::find` distinguishes them.
    [[nodiscard]] virtual Result<
        std::vector<std::pair<product::Uuid, std::optional<ObservedResourceIdentity>>>>
    observed_identities() const = 0;
};

// Acts on a profile's leftovers once the identity has been proven.
//
// An interface because the production implementation needs a real device to do
// anything: the only component allowed to open a volume is a worker, so reclaiming
// means launching one and asking it.  That is `T1-06`'s territory, and until it
// exists the honest implementation is one that **reports rather than acts** --
// which is why this seam exists now.  Without it, `reclaimable_resources` had no
// caller at all, and a decision nobody consults is the "no consumer" defect this
// project has already recorded three times.
class IResourceReclaimer {
public:
    virtual ~IResourceReclaimer() noexcept = default;

    // Whether this reclaimer can actually carry out a reclamation.  False lets the
    // sweep say "I proved ownership but cannot act" rather than reporting success
    // it did not achieve.
    [[nodiscard]] virtual bool can_reclaim() const noexcept = 0;

    // Called only for an identity `reclaimable_resources` has accepted.  Returns
    // whether the leftovers were removed; `false` is a needs-attention condition,
    // not an error.
    [[nodiscard]] virtual Result<bool> reclaim(
        const product::Uuid& profile_id,
        const ObservedResourceIdentity& identity) = 0;
};

// How long an orphan gets to leave on its own before it is ended.
struct WorkerSweepOptions final {
    // Generous on purpose.  An ordered teardown dismounts a volume, removes a
    // drive letter and stops the Aim provider; giving it too little time is what
    // produces the residue the sweep exists to prevent.
    std::uint32_t cooperative_timeout_ms{30'000};
    // Whether an orphan that cannot be asked down (no reachable private channel)
    // may be ended immediately.  False by default so the escalation is an
    // explicit decision rather than a silent one.
    bool force_when_unreachable{false};
};

// Runs one sweep.  Returns a report rather than a failure for per-process
// problems: a sweep that aborted on the first stubborn orphan would leave the
// rest of them untouched.
//
// `identities` and `reclaimer` are optional and default to null, in which case the
// resource half is simply not attempted and the report says nothing about it
// (`reclaimable_profiles` and `unprovable_profiles` both zero).  That is a real
// configuration: a service composed without a reclaimer, and every test that is only
// about processes.  What it is *not* is a silent success -- the counts distinguish
// "not attempted" from "attempted and nothing to do".
[[nodiscard]] Result<WorkerSweepReport> run_worker_sweep(
    IWorkerProcessSource& source, const IExpectedWorkerBindings& bindings,
    const WorkerSweepOptions& options,
    const IObservedResourceIdentitySource* identities = nullptr,
    IResourceReclaimer* reclaimer = nullptr);

} // namespace myrd::management
