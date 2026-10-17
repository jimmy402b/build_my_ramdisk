#pragma once

#include "src/common/result.h"
#include "src/management/service_core.h"
#include "src/product/uuid.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::management {

enum class ExecutionSchedulerError : std::uint32_t {
    invalid_argument = 0x5F01,
    // `start` was called on a scheduler that is already running.  Nothing was
    // started, and the running one is untouched.
    already_running,
    // `stop` was called on a scheduler that was never started, or whose start
    // failed.  There is nothing to stop.
    not_running,
    // The workers did not all retire before the deadline.  This is reported
    // rather than swallowed, because it means a service shutdown would have to
    // destroy an object its workers are still calling into.
    stop_deadline_expired,
};

class IExecutionScheduler {
public:
    virtual ~IExecutionScheduler() noexcept = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop(std::uint64_t monotonic_deadline_ms) = 0;
    virtual bool is_running() const noexcept = 0;
};

struct ExecutionSchedulerConfig final {
    std::uint32_t worker_threads{4};
    std::uint32_t poll_interval_ms{25};
    // The budget `stop` uses when the caller does not name a deadline of its
    // own.  The 120-second default is the service's existing preshutdown
    // budget, so a scheduler stop that a service shutdown drives is bounded by
    // the same number the SCM already allows.
    std::uint32_t stop_deadline_ms{120'000};
};

// The one executor of admitted work.
//
// `ServiceCore::dispatch` durably admits a mutation and returns; nothing runs
// the action sequence until something calls into the core to do it.  This is
// that something: it claims admitted operations from the core and runs them
// through `execute_claimed_operation`.
//
// **Serialization is the point of this object, and it takes the same one the
// pipe does.**  `ServiceCore` and `ServiceWorkerControl` both state that they
// are not concurrent objects -- they hold mutable profile, operation,
// registration and worker-session state that two callers would interleave --
// and both say the production wiring supplies the serialization at the door
// rather than inside each object.  That door is this gate: it is handed to every
// public request by `PublicCommandEndpoint` and to this scheduler, so no two of
// them go into the core at once.  `worker_threads` therefore sizes the polling
// pool; it does not make two operations execute at once, and it cannot, because
// the gate is held across the execution.
//
// **The gate is timed, and the distinction is load-bearing for reads.**  It is
// held across a *whole operation* here -- a create holds it for minutes -- so a
// caller that entered the core through it could not answer while an operation ran.
// A query therefore does not take it at all (`disk list` must return inside a
// second during a create, which is T1-04's acceptance), and a mutation takes it
// with a deadline, so a command arriving behind a long operation is answered with
// a timeout rather than parked until the operation ends.
//
// The gate is not this object's to create.  Passing one in is how the wiring
// states that the pipe and the scheduler are serialized against each other;
// a scheduler built with a gate no pipe uses would serialize against nothing.
class ServiceExecutionScheduler final : public IExecutionScheduler {
public:
    ServiceExecutionScheduler(
        ServiceCore& core,
        IServiceClock& clock,
        std::timed_mutex& execution_gate,
        ExecutionSchedulerConfig config) noexcept;
    ~ServiceExecutionScheduler() noexcept override;

    ServiceExecutionScheduler(const ServiceExecutionScheduler&) = delete;
    ServiceExecutionScheduler& operator=(
        const ServiceExecutionScheduler&) = delete;

    Result<void> start() override;
    // `monotonic_deadline_ms` is an absolute `IServiceClock::monotonic_time_ms`
    // value.  Zero means "use the configured `stop_deadline_ms` from now",
    // which is what a caller with no deadline of its own wants.
    Result<void> stop(std::uint64_t monotonic_deadline_ms) override;
    bool is_running() const noexcept override;

private:
    void run(std::stop_token token) noexcept;
    // Wait out one poll interval, returning early when stop is requested.  The
    // wait is on the stop token rather than on a bare sleep so a stop is
    // answered promptly instead of after a full interval.
    void wait_for_poll(std::stop_token token) noexcept;

    ServiceCore& core_;
    IServiceClock& clock_;
    std::timed_mutex& execution_gate_;
    ExecutionSchedulerConfig config_{};
    mutable std::mutex state_mutex_;
    std::condition_variable_any state_cv_;
    // `workers_` is declared last on purpose.  Members die in reverse
    // declaration order, so declaring the threads last means they are destroyed
    // first -- joined while `state_mutex_`, `state_cv_` and the two counters
    // they retire into still exist.  Declaring the counters after `workers_`
    // would let a retiring worker increment a counter that is already gone.
    std::size_t workers_done_{};
    bool running_{};
    std::vector<std::jthread> workers_;
};

} // namespace myrd::management
