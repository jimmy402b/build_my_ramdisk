#include "src/management/execution_scheduler.h"

#include <chrono>
#include <optional>
#include <utility>

namespace myrd::management {

namespace {

Result<void> scheduler_error(ExecutionSchedulerError code,
                             const wchar_t* operation,
                             const wchar_t* message) {
    return Result<void>::failure(Error{
        ErrorDomain::internal,
        static_cast<std::uint32_t>(code),
        operation,
        message});
}

} // namespace

ServiceExecutionScheduler::ServiceExecutionScheduler(
    ServiceCore& core,
    IServiceClock& clock,
    std::mutex& execution_gate,
    ExecutionSchedulerConfig config) noexcept
    : core_(core),
      clock_(clock),
      execution_gate_(execution_gate),
      config_(config) {}

ServiceExecutionScheduler::~ServiceExecutionScheduler() noexcept {
    // Joining belongs here, in the body, and not in `workers_`'s destructor.
    // These threads call into the borrowed core and they retire by taking
    // `state_mutex_` and notifying `state_cv_`; a vector destructor running late
    // enough to outlive those members would let a retiring worker lock and
    // notify objects that no longer exist.  Stopping and joining before any
    // member dies closes that window.  It is also unconditional rather than
    // deadline-bounded: an object cannot be destroyed while a thread is still
    // calling into it, so there is no safe outcome other than waiting.
    for (auto& worker : workers_) worker.request_stop();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

Result<void> ServiceExecutionScheduler::start() {
    {
        std::scoped_lock lock(state_mutex_);
        if (running_) {
            return scheduler_error(
                ExecutionSchedulerError::already_running,
                L"execution_scheduler.start",
                L"The execution scheduler is already running");
        }
        if (config_.worker_threads == 0) {
            return scheduler_error(
                ExecutionSchedulerError::invalid_argument,
                L"execution_scheduler.start",
                L"A scheduler needs at least one worker thread");
        }
        if (config_.poll_interval_ms == 0) {
            // A zero interval is not a fast poll, it is a spin: the worker
            // would find an empty queue and return to it without ever yielding.
            return scheduler_error(
                ExecutionSchedulerError::invalid_argument,
                L"execution_scheduler.start",
                L"A scheduler needs a non-zero poll interval");
        }
        running_ = true;
    }
    try {
        for (std::uint32_t index = 0; index < config_.worker_threads; ++index) {
            workers_.emplace_back(
                [this](std::stop_token token) { run(token); });
        }
    } catch (...) {
        // A thread that could not be created leaves half a scheduler behind.
        // Retiring the ones that did start is the only way back to a state this
        // object can answer for, and `running_` must not stay true over a pool
        // that is not there.
        for (auto& worker : workers_) worker.request_stop();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
        {
            std::scoped_lock lock(state_mutex_);
            running_ = false;
            workers_done_ = 0;
        }
        return scheduler_error(
            ExecutionSchedulerError::invalid_argument,
            L"execution_scheduler.start",
            L"A scheduler worker thread could not be created");
    }
    return Result<void>::success();
}

Result<void> ServiceExecutionScheduler::stop(
    const std::uint64_t monotonic_deadline_ms) {
    {
        std::scoped_lock lock(state_mutex_);
        if (!running_) {
            return scheduler_error(
                ExecutionSchedulerError::not_running,
                L"execution_scheduler.stop",
                L"The execution scheduler is not running");
        }
    }
    const auto deadline = monotonic_deadline_ms != 0
        ? monotonic_deadline_ms
        : clock_.monotonic_time_ms() + config_.stop_deadline_ms;
    for (auto& worker : workers_) worker.request_stop();
    {
        std::unique_lock lock(state_mutex_);
        while (workers_done_ != workers_.size()) {
            if (clock_.monotonic_time_ms() >= deadline) {
                // Reported rather than waited out.  A worker that will not
                // retire is holding something -- most likely the execution
                // gate, around an operation that is itself bounded elsewhere --
                // and a caller that is told so can decide what to do with the
                // service.  Silently blocking here would turn "shutdown is
                // taking too long" into "shutdown is hung".
                return scheduler_error(
                    ExecutionSchedulerError::stop_deadline_expired,
                    L"execution_scheduler.stop",
                    L"The execution scheduler did not stop before its deadline");
            }
            static_cast<void>(
                state_cv_.wait_for(lock, std::chrono::milliseconds(1)));
        }
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    {
        std::scoped_lock lock(state_mutex_);
        running_ = false;
        workers_.clear();
        workers_done_ = 0;
    }
    return Result<void>::success();
}

bool ServiceExecutionScheduler::is_running() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return running_;
}

void ServiceExecutionScheduler::run(std::stop_token token) noexcept {
    while (!token.stop_requested()) {
        bool worked = false;
        try {
            // Claimed and run under the gate, as one step.  Claiming outside it
            // would let a worker hold a claim while it waits for the gate,
            // which is the same work with a longer window in which the
            // operation is neither running nor available.
            std::scoped_lock lock(execution_gate_);
            auto claimed = core_.claim_next_pending_operation();
            if (claimed.ok() && claimed.value().has_value()) {
                worked = true;
                // The result is discarded deliberately.  The operation's own
                // durable record is the authority on how it ended, and acting
                // on this return value here would make the scheduler a second
                // one.  What this call has to prove is only that the claim was
                // discharged through the core's own execution path.
                static_cast<void>(core_.execute_claimed_operation(
                    claimed.value().value()));
            }
        } catch (...) {
            // A worker action that throws costs this thread one poll interval,
            // not the process.  The operation stays non-terminal and durable --
            // the state a restart reconciles from -- and nothing is reported as
            // having succeeded.
            worked = false;
        }
        if (!worked) wait_for_poll(token);
    }
    {
        std::scoped_lock lock(state_mutex_);
        ++workers_done_;
    }
    state_cv_.notify_all();
}

void ServiceExecutionScheduler::wait_for_poll(
    const std::stop_token token) noexcept {
    std::unique_lock lock(state_mutex_);
    // Waits on the stop token rather than sleeping: a stop that has to wait out
    // a poll interval first is a stop that reports a deadline it never needed.
    static_cast<void>(state_cv_.wait_for(
        lock,
        token,
        std::chrono::milliseconds(config_.poll_interval_ms),
        [] { return false; }));
}

} // namespace myrd::management
