#include "src/service/service_host.h"

namespace myrd::service {
namespace {

Error host_error(const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::internal, 1, operation, message, 0};
}

// The clock is used as a *source*, never advanced: `IServiceClock` is a
// read-only view of time, so a deadline is expressed by adding the budget to the
// reading taken when the stop began.  A host that tried to normalise the clock
// would be writing to a seam that has no write.
std::uint64_t deadline_after(const std::uint64_t now_ms,
                             const std::uint32_t budget_ms) noexcept {
    return now_ms + budget_ms;
}

} // namespace

const char* to_string(const ServiceHostState state) noexcept {
    switch (state) {
    case ServiceHostState::constructed: return "constructed";
    case ServiceHostState::running: return "running";
    case ServiceHostState::stopping: return "stopping";
    case ServiceHostState::stopped: return "stopped";
    case ServiceHostState::failed: return "failed";
    }
    return "unknown";
}

ServiceHost::ServiceHost(management::IServiceClock& clock,
                         management::ServiceCore& core,
                         management::IExecutionScheduler& scheduler,
                         ServiceHostOptions options) noexcept
    : clock_(clock), core_(core), scheduler_(scheduler),
      options_(options) {}

Result<void> ServiceHost::start() {
    std::lock_guard<std::mutex> guard(mutex_);
    // Starting an already-running service is not an error: the SCM will not do
    // it, but a restart path that re-entered here would otherwise tear down a
    // running service to start it again.
    if (state_ == ServiceHostState::running) return Result<void>::success();

    // Durable state first, work second.  See the header for why this order is
    // the correctness of the thing and not a preference.
    auto recovered = core_.recover_after_restart();
    if (!recovered.ok()) {
        last_error_ = recovered.error();
        state_ = ServiceHostState::failed;
        return Result<void>::failure(recovered.error());
    }
    recovered_operations_ = recovered.value().size();

    auto started = scheduler_.start();
    if (!started.ok()) {
        // The store reconciled, but nothing can run.  That is a failed start:
        // reporting RUNNING would describe a service that accepts commands and
        // never executes them.
        last_error_ = started.error();
        state_ = ServiceHostState::failed;
        return Result<void>::failure(started.error());
    }

    state_ = ServiceHostState::running;
    last_error_.reset();
    return Result<void>::success();
}

Result<void> ServiceHost::stop() {
    std::lock_guard<std::mutex> guard(mutex_);
    return stop_locked();
}

Result<void> ServiceHost::preshutdown() {
    std::lock_guard<std::mutex> guard(mutex_);
    return stop_locked();
}

Result<void> ServiceHost::stop_locked() {
    // Already down, or never up.  Both are answers, not failures: a stop that
    // arrived after the service stopped on its own must not be reported as a
    // shutdown failure, and the SCM sends a stop to a service whose start
    // failed.
    if (state_ == ServiceHostState::stopped) return Result<void>::success();
    if (state_ == ServiceHostState::constructed) {
        state_ = ServiceHostState::stopped;
        return Result<void>::success();
    }

    state_ = ServiceHostState::stopping;
    // The scheduler is stopped before anything it calls into is destroyed, and
    // the deadline is absolute because that is what the seam takes.  `stop` is
    // idempotent on the scheduler, so a second stop through this host is
    // answered rather than refused.
    const auto deadline =
        deadline_after(clock_.monotonic_time_ms(), options_.stop_budget_ms);
    auto stopped = scheduler_.stop(deadline);
    if (!stopped.ok()) {
        // Reported, and the host does not claim to be stopped: a scheduler that
        // did not retire its threads is still calling into the core, and
        // declaring the service down over that would be the lie this whole
        // object exists to prevent.
        last_error_ = stopped.error();
        state_ = ServiceHostState::failed;
        return Result<void>::failure(stopped.error());
    }

    state_ = ServiceHostState::stopped;
    return Result<void>::success();
}

ServiceHostState ServiceHost::state() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return state_;
}

bool ServiceHost::running() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return state_ == ServiceHostState::running;
}

ServiceHostDiagnostics ServiceHost::diagnostics() const {
    std::lock_guard<std::mutex> guard(mutex_);
    ServiceHostDiagnostics diagnostics{};
    diagnostics.state = state_;
    diagnostics.recovered_operations = recovered_operations_;
    diagnostics.scheduler_running = scheduler_.is_running();
    diagnostics.last_error = last_error_;
    return diagnostics;
}

} // namespace myrd::service
