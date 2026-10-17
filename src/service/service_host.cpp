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
                         ServiceHostOptions options,
                         IStartupSweep* sweep,
                         IPublicCommandSurface* surface) noexcept
    : clock_(clock), core_(core), scheduler_(scheduler),
      options_(options), sweep_(sweep), surface_(surface) {}

ServiceHost::ServiceHost(ServiceHost&& other) noexcept
    : clock_(other.clock_), core_(other.core_), scheduler_(other.scheduler_),
      options_(other.options_), sweep_(other.sweep_), surface_(other.surface_),
      state_(other.state_),
      recovered_operations_(other.recovered_operations_),
      sweep_needs_attention_(other.sweep_needs_attention_),
      last_error_(std::move(other.last_error_)) {
    // The mutex is not moved: an unlocked one is what a moved-to object must
    // have.  The state fields are read without holding it, which is sound because
    // a move happens while one thread owns the object and is handing it over --
    // the same assumption every move constructor makes.
}

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

    // The sweep, between the recovery and the scheduler, and both neighbours are
    // deliberate.  After the recovery, because the recovery is what says which
    // non-terminal operations are still owned -- sweeping first would judge a
    // worker against a binding the recovery is about to rewrite.  Before the
    // scheduler, because the scheduler's first act is to claim one of those
    // operations, and it may belong to a worker this sweep is about to terminate.
    sweep_needs_attention_ = false;
    if (sweep_ != nullptr) {
        auto swept = sweep_->run_sweep();
        if (!swept.ok()) {
            // A sweep that could not run at all is a failed start, unlike one that
            // merely found something.  Continuing would mean admitting work while
            // a predecessor's worker still holds a device, which is the state the
            // sweep exists to prevent.
            last_error_ = swept.error();
            state_ = ServiceHostState::failed;
            return Result<void>::failure(swept.error());
        }
        sweep_needs_attention_ = sweep_->needs_attention();
        if (sweep_needs_attention_
            && options_.fail_start_when_sweep_needs_attention) {
            const Error refused{ErrorDomain::internal, 1, L"startup_sweep",
                                L"the start-up sweep found a condition that needs "
                                L"attention and this service is configured to "
                                L"refuse to start over it",
                                0};
            last_error_ = refused;
            state_ = ServiceHostState::failed;
            return Result<void>::failure(refused);
        }
    }

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

    // The public surface last, and the ordering is the same argument the sweep's is:
    // a request answered before the executor is running would be admitted and then
    // never executed, and a request answered before the recovery would read durable
    // state that is about to be rewritten.  Opening it here means the first accepted
    // command already has a service that can carry it out.
    if (surface_ != nullptr) {
        auto opened = surface_->open();
        if (!opened.ok()) {
            // A service that cannot be commanded is not a running service: closing the
            // scheduler again is what keeps the reported state and the reachable state
            // the same thing.
            last_error_ = opened.error();
            state_ = ServiceHostState::failed;
            static_cast<void>(scheduler_.stop(deadline_after(
                clock_.monotonic_time_ms(), options_.stop_budget_ms)));
            return Result<void>::failure(opened.error());
        }
    }
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
    // The pipe first (design spec 搂9: close admission, remove the pipe, stop the
    // scheduler).  A command admitted after the scheduler stopped would be a durable
    // record nobody runs -- the exact state the next start has to reconcile -- so the
    // surface is closed before the executor is asked to retire.
    //
    // A failure to close is reported and remembered, but the shutdown continues: the
    // scheduler must still be stopped, and a pipe that would not close has already
    // been asked to.  The host reports itself failed afterwards rather than claiming a
    // clean stop over a surface it could not close.
    std::optional<Error> surface_error;
    if (surface_ != nullptr) {
        auto closed = surface_->close();
        if (!closed.ok()) surface_error = closed.error();
    }
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
    if (surface_error.has_value()) {
        // The executor is down, so the service is not running; what failed is the
        // promise that it stopped cleanly.  Reported rather than swallowed.
        last_error_ = surface_error.value();
        state_ = ServiceHostState::failed;
        return Result<void>::failure(surface_error.value());
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
    diagnostics.sweep_needs_attention = sweep_needs_attention_;
    diagnostics.last_error = last_error_;
    return diagnostics;
}

} // namespace myrd::service
