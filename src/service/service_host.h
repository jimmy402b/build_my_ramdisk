#pragma once

#include "src/common/result.h"
#include "src/management/execution_scheduler.h"
#include "src/management/service_core.h"
#include "src/management/service_maintenance.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace myrd::service {

// Where the service is in its own lifecycle.
//
// This is deliberately *not* an SCM state.  The SCM has its own vocabulary
// (`SERVICE_START_PENDING`, `SERVICE_RUNNING`, ...) and it is the wrapper's job
// to translate; keeping the two apart is what lets this object's lifecycle be
// tested without a service control manager, and what stops the process's
// externally-reported state from drifting away from what it is actually doing.
enum class ServiceHostState : std::uint8_t {
    constructed = 0,
    running,
    stopping,
    stopped,
    failed,
};

[[nodiscard]] const char* to_string(ServiceHostState state) noexcept;

struct ServiceHostOptions final {
    // How long a stop may take before the host gives up waiting on the
    // scheduler.  The SCM's preshutdown budget is the ceiling this has to stay
    // under; the wrapper passes what it was granted, and the default is the
    // 120-second figure the SCM allows for a preshutdown.
    std::uint32_t stop_budget_ms{120'000};
    // Whether a start that had to escalate against an orphan it could not
    // attribute still reports success.
    //
    // The default is to start anyway, and that is a deliberate choice rather than
    // an oversight: a machine whose leftover worker will not die still has other
    // disks the operator wants served, and refusing to start would turn one stuck
    // orphan into a service that is down.  The condition is reported instead --
    // through `ServiceHostDiagnostics::sweep_needs_attention` and the log -- so it
    // is visible rather than silent.
    bool fail_start_when_sweep_needs_attention{false};
};

// The start-up sweep, as the host sees it.
//
// An interface so the host can be tested without a job object and a live orphan,
// and so the host does not acquire a dependency on the worker-process layer.  The
// production implementation is `management::run_worker_sweep`.
class IStartupSweep {
public:
    virtual ~IStartupSweep() noexcept = default;

    // Runs once, before the scheduler admits any work.
    //
    // Before the scheduler, and that ordering is the whole reason this is not
    // simply part of the composition root: the scheduler's first act is to claim a
    // non-terminal operation, and one of those operations may belong to a worker
    // this sweep is about to terminate.  Sweeping afterwards would race the
    // scheduler for it.
    [[nodiscard]] virtual Result<void> run_sweep() = 0;

    // Whether the last sweep found something a human has to look at: an orphan it
    // could not attribute, or one that would not die.  Separate from the `Result`
    // above because "the sweep ran and reports a condition" is not a failure of
    // the sweep, and a caller that conflated them would report a stuck orphan as a
    // broken service.
    [[nodiscard]] virtual bool needs_attention() const noexcept = 0;
};

// The public command surface, as the host sees it: something that can be opened for
// business and closed again.
//
// An interface for the same reason the sweep is one -- the ordering below is the
// property, and it can be measured with a recorder instead of a pipe -- and because
// the host must not know how a pipe's security descriptor is built.  The production
// implementation is `PublicPipeSurface`, over `PublicPipeServer` and
// `PublicCommandEndpoint`.
class IPublicCommandSurface {
public:
    virtual ~IPublicCommandSurface() noexcept = default;

    // Accept requests.  Called last in `start`, so a client never reaches a service
    // whose durable state has not been reconciled or whose executor is not running:
    // a command answered before the scheduler is up would be admitted and never run.
    [[nodiscard]] virtual Result<void> open() = 0;

    // Refuse new requests, and stop accepting connections.  Called first in `stop`,
    // which is the design's own order (鎼?: close admission, remove the pipe, then
    // stop the scheduler): a command admitted after the scheduler stopped would be a
    // durable record that the next start has to reconcile.
    [[nodiscard]] virtual Result<void> close() = 0;
};

// What the last start recovered, for evidence and for the operator.
//
// Kept as its own type rather than logged and dropped: "the service started and
// reconciled three operations" and "the service started and found nothing" are
// different facts about a boot, and the second one is the whole acceptance
// criterion for a machine with no disks on it.
struct ServiceHostDiagnostics final {
    ServiceHostState state{ServiceHostState::constructed};
    std::size_t recovered_operations{};
    bool scheduler_running{};
    // Whether the start-up sweep found a condition for a human.  Defaults to
    // false so a host with no sweep configured -- every test that does not care
    // about orphans -- reads as "nothing to report" rather than as "unknown".
    bool sweep_needs_attention{};
    std::optional<Error> last_error;
};

// The service's own lifecycle, with no SCM in it.
//
// A Windows service is three things that are easy to entangle and expensive to
// untangle: the SCM protocol (`StartServiceCtrlDispatcher`, the control handler,
// status reporting), the process's lifecycle, and the work.  This object is the
// middle one.  The wrapper above it does nothing but translate SCM calls into
// `start`/`stop`/`preshutdown` and report the state this object says it is in;
// the layers below it own the durable state and the execution.
//
// **The SCM-free split is the point, not tidiness.**  A host whose lifecycle
// could only be exercised by installing a service would have its start and stop
// paths measured once, on a VM, by hand.  This one's are measured by ordinary
// tests on every build, and what the VM adds is the part that genuinely needs a
// service control manager: that the SCM can start it, stop it, and that a
// shutdown reaches it before the process is killed.
//
// **What this object does not do yet.**  The per-profile shutdown of design spec
// 搂9 -- "close admission, stop the scheduler, checkpoint and dismantle each disk
// within a budget shared across the disks" -- is not here.  The scheduler stop
// is, because it is the part that must happen before anything else is destroyed,
// and the per-profile part belongs with the code that owns worker teardown
// (T1-03) rather than being invented here against a seam that does not exist yet.
// Stated rather than left to be discovered.
class ServiceHost final {
public:
    ServiceHost(management::IServiceClock& clock, management::ServiceCore& core,
                management::IExecutionScheduler& scheduler,
                ServiceHostOptions options = {},
                IStartupSweep* sweep = nullptr,
                IPublicCommandSurface* surface = nullptr,
                management::ISuspendCheckpoint* suspend = nullptr) noexcept;

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;
    // Hand-written rather than defaulted, and the difference matters: a
    // `std::mutex` member makes the *implicit* move constructor deleted, so a
    // defaulted one would be deleted too and callers would silently fall back to
    // the deleted copy.  The explicit form below reconstructs the mutex instead
    // of transferring it -- a mutex is not state to move, it is a lock object,
    // and an unlocked one is what a moved-to host must have.  It is defined in
    // the .cpp for exactly that reason: the work is a member-wise move plus a
    // fresh mutex, which a `= default` cannot express.
    ServiceHost(ServiceHost&& other) noexcept;
    // Move *assignment* is not offered, and cannot be: the host holds references,
    // and a reference cannot be rebound -- so an assignment would have to leave
    // the target pointing at the source's dependencies, which is the dangling
    // state this whole object exists to make impossible.  Deleted explicitly so
    // the compiler reports it instead of a defaulted member doing something
    // surprising.
    ServiceHost& operator=(ServiceHost&&) = delete;

    // Brings the service up.
    //
    // The order is the one design spec 搂9 gives: recover the durable state
    // first, then admit work.  Reconciling after the scheduler is polling would
    // let it claim an operation whose own record the recovery is still
    // rewriting, and the recovery is what decides which non-terminal operations
    // are still owned.
    //
    // A failed recovery leaves the service *stopped*, not running: an SCM that
    // reported RUNNING over a store it could not reconcile would be claiming a
    // healthy service over unknown durable state.
    Result<void> start();

    // Brings the service down within `options_.stop_budget_ms`.
    //
    // Idempotent, and safe to call on a host that never started -- the SCM
    // sends a stop to a service whose start failed, and answering that with a
    // failure would report a broken shutdown for a service that is already down.
    Result<void> stop();

    // The OS is shutting down.  Same work as `stop`, and deliberately a separate
    // name: the SCM gives it a different (and shorter, and non-negotiable)
    // budget, and a caller that conflated the two could not tell an operator's
    // stop from a machine going away.  What it does *not* change is the
    // requirement that the work is bounded and reported honestly.
    Result<void> preshutdown();

    // The machine is about to sleep or hibernate.
    //
    // Not a shutdown and not a stop: the scheduler keeps running, the workers keep
    // their devices, and the only work is the policy's own best-effort checkpoint for
    // the profiles whose `on_suspend` says so.  Returns how many profiles were handed
    // a checkpoint, so a caller can log an answer rather than a shrug.
    std::size_t suspend();

    [[nodiscard]] ServiceHostState state() const;
    [[nodiscard]] ServiceHostDiagnostics diagnostics() const;
    [[nodiscard]] bool running() const noexcept;

private:
    // The one shutdown implementation `stop` and `preshutdown` both run.  Two
    // copies would be two shutdowns that can disagree, and a machine going away
    // is the worst moment to discover they do.
    Result<void> stop_locked();

    management::IServiceClock& clock_;
    management::ServiceCore& core_;
    management::IExecutionScheduler& scheduler_;
    ServiceHostOptions options_{};
    // Borrowed, and may be null: a host with nothing to sweep is a legitimate
    // configuration (a test, or a service whose worker layer is not composed), and
    // requiring one would force every such caller to build a stand-in.
    IStartupSweep* sweep_{};
    // Borrowed, and may be null for the same reason: a host with no public surface is
    // a host nothing can command, which is the right shape for a test whose subject is
    // the durable ordering rather than the protocol.  When it is present the host owns
    // the *ordering* of opening and closing it, which is the part that must not be
    // duplicated in an executable.
    IPublicCommandSurface* surface_{};
    // Borrowed, and may be null: a host with no policy has nothing to checkpoint on
    // suspend, which is every test whose subject is the shutdown ordering.
    management::ISuspendCheckpoint* suspend_{};

    mutable std::mutex mutex_;
    ServiceHostState state_{ServiceHostState::constructed};
    std::size_t recovered_operations_{};
    bool sweep_needs_attention_{};
    std::optional<Error> last_error_;
};

} // namespace myrd::service
