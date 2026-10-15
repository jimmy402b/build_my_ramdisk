#pragma once

#include "src/common/result.h"
#include "src/management/execution_scheduler.h"
#include "src/management/service_core.h"

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
// §9 -- "close admission, stop the scheduler, checkpoint and dismantle each disk
// within a budget shared across the disks" -- is not here.  The scheduler stop
// is, because it is the part that must happen before anything else is destroyed,
// and the per-profile part belongs with the code that owns worker teardown
// (T1-03) rather than being invented here against a seam that does not exist yet.
// Stated rather than left to be discovered.
class ServiceHost final {
public:
    ServiceHost(management::IServiceClock& clock, management::ServiceCore& core,
                management::IExecutionScheduler& scheduler,
                ServiceHostOptions options = {}) noexcept;

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    // Brings the service up.
    //
    // The order is the one design spec §9 gives: recover the durable state
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

    mutable std::mutex mutex_;
    ServiceHostState state_{ServiceHostState::constructed};
    std::size_t recovered_operations_{};
    std::optional<Error> last_error_;
};

} // namespace myrd::service
