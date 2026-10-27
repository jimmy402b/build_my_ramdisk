#include "tests/support/test_framework.h"

// The M4 checkpoint fixture, for its durable stores, clock, ids and worker
// double.  Including it with the guard defined gives the fixture without its
// own cases, which is how the M5 model suite already reuses it.
#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

#include "src/management/execution_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace myrd;
using namespace myrd::management;

namespace {

bool wait_until(const std::function<bool()>& done, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

const OperationRecord* find_record(const Fixture& fixture,
                                   const product::Uuid& operation_id) {
    const auto& records = fixture.operations.records;
    const auto found = std::find_if(
        records.begin(), records.end(), [&](const auto& record) {
            return record.operation_id == operation_id;
        });
    return found == records.end() ? nullptr : &*found;
}

bool record_is_succeeded(const Fixture& fixture,
                         const product::Uuid& operation_id) {
    const auto* record = find_record(fixture, operation_id);
    return record != nullptr && record->state == OperationState::succeeded;
}

bool record_is_terminal(const Fixture& fixture,
                        const product::Uuid& operation_id) {
    const auto* record = find_record(fixture, operation_id);
    if (record == nullptr) return false;
    return record->state == OperationState::succeeded
        || record->state == OperationState::failed
        || record->state == OperationState::cancelled;
}

struct InFlightScope final {
    std::atomic_int& counter;
    ~InFlightScope() { --counter; }
};

// The fixture's `Worker` is `final`, so this wraps one rather than deriving from
// it, forwarding every call unchanged and adding only the two observations the
// scheduler cases need: how many executions were ever in flight at once, and a
// hook that can make one throw.
class InstrumentedWorker final : public IWorkerControl {
public:
    bool begins_noncancellable(ActionKind action) const noexcept override {
        return inner.begins_noncancellable(action);
    }

    Result<WorkerActionResult> execute(
        const WorkerActionRequest& request) override {
        enter(request);
        const InFlightScope leave{in_flight};
        if (before_execute) before_execute();
        return inner.execute(request);
    }

    Result<WorkerActionResult> execute_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryHandler& boundary) override {
        enter(request);
        const InFlightScope leave{in_flight};
        if (before_execute) before_execute();
        return inner.execute_checkpoint(request, boundary);
    }

    Result<WorkerActionResult> execute_compaction(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryHandler& boundary,
        const CompactionCutPublicationHandler& cut) override {
        return inner.execute_compaction(request, boundary, cut);
    }

    Result<WorkerActionResult> reconcile_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation) override {
        return inner.reconcile_checkpoint(request, correlation);
    }

    Result<void> publish_checkpoint_epoch(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation,
        const CheckpointEpochPublicationHandler& publish) override {
        return inner.publish_checkpoint_epoch(request, correlation, publish);
    }

    Result<WorkerActionResult> reconcile_compaction(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryEvidence& correlation) override {
        return inner.reconcile_compaction(request, correlation);
    }

    Result<void> publish_compaction_epoch(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryEvidence& correlation,
        const CompactionEpochPublicationHandler& publish) override {
        return inner.publish_compaction_epoch(request, correlation, publish);
    }

    Result<std::optional<std::uint64_t>> current_dirty_epoch(
        const product::Uuid& profile_id) override {
        return inner.current_dirty_epoch(profile_id);
    }

    Result<std::optional<WorkerRuntimeObservation>> observe_runtime(
        const product::Uuid& profile_id) override {
        return inner.observe_runtime(profile_id);
    }

    Result<WorkerRuntimeObservation> require_runtime_observation(
        const product::Uuid& profile_id) override {
        return inner.require_runtime_observation(profile_id);
    }

    Result<ForgetSafetyEvidence> prove_forget_safe(
        const WorkerActionRequest& request) override {
        return inner.prove_forget_safe(request);
    }

    // Whether this worker was ever asked to act for that operation.  Recorded
    // per operation rather than as a total, because one operation runs a
    // sequence of actions: a total would say "something ran twice", which is
    // not the same claim as "both operations ran".
    bool saw_operation(const product::Uuid& operation_id) {
        const std::scoped_lock lock(seen_mutex);
        return std::find(
                   operations_seen.begin(), operations_seen.end(), operation_id)
            != operations_seen.end();
    }

    Worker inner;
    std::atomic_int in_flight{0};
    std::atomic_int max_in_flight{0};
    std::atomic_int executions{0};
    std::function<void()> before_execute;

private:
    void enter(const WorkerActionRequest& request) noexcept {
        ++executions;
        {
            const std::scoped_lock lock(seen_mutex);
            operations_seen.push_back(request.operation_id);
        }
        const int now = ++in_flight;
        int observed = max_in_flight.load();
        while (now > observed
               && !max_in_flight.compare_exchange_weak(observed, now)) {
        }
    }

    std::mutex seen_mutex;
    std::vector<product::Uuid> operations_seen;
};

} // namespace

TEST_CASE(m5_scheduler_takes_nothing_from_an_empty_queue) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    std::timed_mutex gate;

    auto empty = core.claim_next_pending_operation();
    REQUIRE(empty.ok());
    REQUIRE(!empty.value().has_value());

    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate,
        ExecutionSchedulerConfig{1, 5, 1000});
    REQUIRE(!scheduler.is_running());
    REQUIRE(scheduler.start().ok());
    REQUIRE(scheduler.is_running());
    REQUIRE(scheduler.stop(0).ok());
    REQUIRE(!scheduler.is_running());
    // Nothing was admitted, so nothing could have been executed.
    REQUIRE_EQ(fixture.worker.calls.size(), 0U);
}

// Step 1's first claim: the durable admission comes first and is the only thing
// dispatch does.  A client that never connects again, or that disconnects while
// the service is still holding the operation, is answered by this record.
TEST_CASE(m5_admission_is_durable_before_anything_executes_it) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888888"));
    REQUIRE(admitted.ok());
    REQUIRE(admitted.value().operation_id.has_value());
    REQUIRE_EQ(
        admitted.value().disposition,
        ServiceResponseDisposition::operation_queued);
    const auto operation_id = admitted.value().operation_id.value();

    // Admission alone ran nothing: the durable record is the entire effect, and
    // the worker has not been asked to act on it.
    REQUIRE_EQ(fixture.worker.calls.size(), 0U);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    const auto* record = find_record(fixture, operation_id);
    REQUIRE(record != nullptr);
    REQUIRE_EQ(record->operation_id, operation_id);
    REQUIRE(!record_is_terminal(fixture, operation_id));

    // And it is still there to be picked up: a scheduler that finds it is
    // reading durable work, not a queue entry it had to be present for.
    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{1, 5, 1000});
    REQUIRE(scheduler.start().ok());
    REQUIRE(
        wait_until([&] { return record_is_succeeded(fixture, operation_id); }));
    REQUIRE(scheduler.stop(0).ok());
}

// Step 1's fourth claim, and the one a naive design gets wrong: a client whose
// wait expired -- or that gave up before the work even started -- is told so and
// then has no further say.  Its timeout is a fact about the reply, never a
// cancellation of a safe operation, so the work still runs to its own durable
// conclusion.
TEST_CASE(m5_a_client_wait_timeout_never_cancels_admitted_work) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto abandoned = fixture.request("88888888-8888-4888-8888-888888888889");
    abandoned.client_wait = ClientWaitState::cancelled;
    auto admitted = core.dispatch(abandoned);
    REQUIRE(admitted.ok());
    REQUIRE(admitted.value().operation_id.has_value());
    REQUIRE_EQ(
        admitted.value().disposition,
        ServiceResponseDisposition::client_wait_cancelled);
    const auto operation_id = admitted.value().operation_id.value();

    // The client's withdrawal changed the answer, not the work: the operation
    // is durable, non-terminal, and nothing has run yet.
    REQUIRE(!record_is_terminal(fixture, operation_id));
    REQUIRE_EQ(fixture.worker.calls.size(), 0U);

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{1, 5, 1000});
    REQUIRE(scheduler.start().ok());
    REQUIRE(
        wait_until([&] { return record_is_succeeded(fixture, operation_id); }));
    REQUIRE(scheduler.stop(0).ok());

    REQUIRE_EQ(
        find_record(fixture, operation_id)->state, OperationState::succeeded);
}

TEST_CASE(m5_scheduler_claims_a_queued_operation_exactly_once) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888881"));
    REQUIRE(admitted.ok());
    REQUIRE(admitted.value().operation_id.has_value());
    const auto operation_id = admitted.value().operation_id.value();

    auto first = core.claim_next_pending_operation();
    REQUIRE(first.ok());
    REQUIRE(first.value().has_value());
    REQUIRE_EQ(first.value().value(), operation_id);

    // The claim is taken: the same operation is not handed out again.
    auto second = core.claim_next_pending_operation();
    REQUIRE(second.ok());
    REQUIRE(!second.value().has_value());

    // And an operation nobody claimed cannot be run through the claimed path,
    // so skipping the claim cannot become a second executor for it.
    auto unclaimed = core.execute_claimed_operation(
        uuid("99999999-9999-4999-8999-999999999999"));
    REQUIRE(!unclaimed.ok());
    REQUIRE_EQ(unclaimed.error().code,
        static_cast<std::uint32_t>(ServiceCoreError::operation_invalid));

    auto terminal = core.execute_claimed_operation(operation_id);
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
}

TEST_CASE(m5_scheduler_executes_admitted_work_through_the_core) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888882"));
    REQUIRE(admitted.ok());
    const auto operation_id = admitted.value().operation_id.value();

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{2, 5, 1000});
    REQUIRE(scheduler.start().ok());
    REQUIRE(wait_until([&] { return record_is_succeeded(fixture, operation_id); }));
    REQUIRE(scheduler.stop(0).ok());
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id);
}

TEST_CASE(m5_scheduler_never_runs_two_operations_at_once) {
    // The scheduler's whole job here is to be the serialization the core and
    // the worker control are written against, so the assertion is that two
    // executions were never in flight together -- not that two profiles made
    // progress in parallel.  See the header comment on
    // ServiceExecutionScheduler: worker_threads sizes a polling pool, it does
    // not buy concurrency, because the gate is held across the execution.
    Fixture fixture(product::PersistenceState::dirty);
    auto secondary = running_profile(product::PersistenceState::dirty);
    secondary.record.profile.profile_uuid =
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    secondary.record.profile.display_name = L"other";
    secondary.record.profile.owner_sid = L"S-1-5-21-100-200-300-1002";
    fixture.profiles.add_secondary(secondary);
    fixture.ids.values.push_back(
        uuid("55555555-5555-4555-8555-555555555555"));

    InstrumentedWorker worker;
    // Long enough that two concurrent executions could not miss each other.
    worker.before_execute = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto a = fixture.request("88888888-8888-4888-8888-888888888883");
    auto b = a;
    b.request_id = uuid("88888888-8888-4888-8888-888888888884");
    b.command = control::DiskCheckpointCommand{L"other"};
    b.caller = caller(secondary.record.profile.owner_sid);
    auto admitted_a = core.dispatch(a);
    auto admitted_b = core.dispatch(b);
    REQUIRE(admitted_a.ok());
    REQUIRE(admitted_b.ok());
    const auto operation_a = admitted_a.value().operation_id.value();
    const auto operation_b = admitted_b.value().operation_id.value();
    REQUIRE(operation_a != operation_b);

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{4, 2, 1000});
    REQUIRE(scheduler.start().ok());
    REQUIRE(wait_until([&] {
        return record_is_succeeded(fixture, operation_a)
            && record_is_succeeded(fixture, operation_b);
    }));
    REQUIRE(scheduler.stop(0).ok());

    // The claim this case exists for: the two executions never overlapped.
    REQUIRE_EQ(worker.max_in_flight.load(), 1);
    REQUIRE(worker.saw_operation(operation_a));
    REQUIRE(worker.saw_operation(operation_b));
    // Both profiles reached their own conclusion, and neither kept the other's
    // gate.
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id);
    REQUIRE(!fixture.profiles.secondary_snapshot().operation_gate
        .active_operation_id);
}

TEST_CASE(m5_scheduler_waits_for_the_gate_the_command_path_holds) {
    // Deterministic form of the serialization claim: while the caller of the
    // pipe holds the one gate, the scheduler must execute nothing at all.  This
    // is the case a scheduler that took its own private lock would fail.
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888885"));
    REQUIRE(admitted.ok());
    const auto operation_id = admitted.value().operation_id.value();

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{2, 2, 1000});
    {
        const std::scoped_lock held(gate);
        REQUIRE(scheduler.start().ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        REQUIRE_EQ(fixture.worker.calls.size(), 0U);
        REQUIRE(!record_is_succeeded(fixture, operation_id));
    }
    REQUIRE(wait_until([&] { return record_is_succeeded(fixture, operation_id); }));
    REQUIRE(scheduler.stop(0).ok());
}

TEST_CASE(m5_scheduler_stop_reports_a_deadline_it_could_not_meet) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888886"));
    REQUIRE(admitted.ok());
    const auto operation_id = admitted.value().operation_id.value();

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{2, 2, 1000});
    {
        const std::scoped_lock held(gate);
        REQUIRE(scheduler.start().ok());
        // The fixture clock is a constant, so this deadline is already behind
        // it and the first check trips.  The workers are parked on the gate,
        // which is the state the deadline exists to report.
        auto stopped = scheduler.stop(fixture.clock.monotonic_time_ms());
        REQUIRE(!stopped.ok());
        REQUIRE_EQ(stopped.error().code,
            static_cast<std::uint32_t>(
                ExecutionSchedulerError::stop_deadline_expired));
        // A stop that could not finish must not claim it did.
        REQUIRE(scheduler.is_running());
        REQUIRE(!record_is_succeeded(fixture, operation_id));
    }
    // With the gate released the workers retire, and a stop that can meet its
    // deadline now succeeds.
    REQUIRE(scheduler.stop(0).ok());
    REQUIRE(!scheduler.is_running());
    // Retiring is not the same as succeeding: stop is not allowed to mark the
    // operation it interrupted as done.
    REQUIRE(!record_is_succeeded(fixture, operation_id));
}

TEST_CASE(m5_scheduler_contains_a_worker_exception) {
    Fixture fixture(product::PersistenceState::dirty);
    InstrumentedWorker worker;
    worker.before_execute = [] { throw std::runtime_error("injected"); };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted = core.dispatch(
        fixture.request("88888888-8888-4888-8888-888888888887"));
    REQUIRE(admitted.ok());
    const auto operation_id = admitted.value().operation_id.value();

    std::timed_mutex gate;
    ServiceExecutionScheduler scheduler(
        core, fixture.clock, gate, ExecutionSchedulerConfig{2, 2, 1000});
    REQUIRE(scheduler.start().ok());
    REQUIRE(wait_until([&] { return worker.executions.load() >= 1; }));
    // The throw cost the worker a poll interval and nothing else: the scheduler
    // is still running and answering.
    REQUIRE(scheduler.is_running());
    REQUIRE(scheduler.stop(0).ok());
    REQUIRE(!scheduler.is_running());
    // Nothing was reported as having succeeded on the strength of a throw.
    REQUIRE(!record_is_succeeded(fixture, operation_id));
}
