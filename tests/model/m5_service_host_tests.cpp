// The service's own lifecycle, without a service control manager.
//
// T1-02's acceptance is "the service installs, starts, stops, and exits cleanly
// with no disks".  This file is the part of that which can be measured on every
// build: what `ServiceHost` does, in what order, and what it refuses to claim.
// What remains for the VM is the SCM itself -- that a control manager can start
// it, stop it, and reach it before the process is killed -- which is a property
// of the wrapper and of Windows rather than of this logic.
//
// T1-02 is where "no disks" stops being an edge case and becomes the acceptance
// criterion, because it is the state every fresh install starts in and the one a
// service that assumes work exists would fail on.

#include "tests/support/test_framework.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

#include "src/control/public_pipe_client.h"
#include "src/service/service_host.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace myrd;
using namespace myrd::management;
using namespace myrd::service;

namespace {

// The scheduler, recorded rather than simulated.
//
// Each call is logged so a case can assert the *order* the host drove and not
// only the state it ended in -- "recovered, then started" and "started, then
// recovered" reach the same state and are not the same service.
class FakeScheduler final : public IExecutionScheduler {
public:
    Result<void> start() override {
        events.push_back("scheduler.start");
        if (fail_start) return Result<void>::failure(error(L"scheduler.start"));
        running = true;
        return Result<void>::success();
    }

    Result<void> stop(const std::uint64_t monotonic_deadline_ms) override {
        events.push_back("scheduler.stop");
        stop_deadlines.push_back(monotonic_deadline_ms);
        if (fail_stop) return Result<void>::failure(error(L"scheduler.stop"));
        running = false;
        return Result<void>::success();
    }

    bool is_running() const noexcept override { return running; }

    static Error error(const wchar_t* operation) {
        return {ErrorDomain::internal, 7, operation, L"injected", 0};
    }

    std::vector<std::string> events;
    std::vector<std::uint64_t> stop_deadlines;
    bool running{};
    bool fail_start{};
    bool fail_stop{};
};

// A clock the case controls.  `monotonic` is what a deadline is computed from,
// so a case can assert that the budget became the deadline it should have.
class FakeClock final : public IServiceClock {
public:
    std::uint64_t unix_time_ms() const noexcept override { return unix; }
    std::uint64_t monotonic_time_ms() const noexcept override {
        return monotonic;
    }

    std::uint64_t unix{1'700'000'000'000ULL};
    std::uint64_t monotonic{5'000};
};

// The start-up sweep, recorded in the same event log as the scheduler so a case
// can assert the *order* rather than only that both happened.  That order is the
// reason the sweep is a host step at all: it has to sit between the recovery and
// the first claim, and a host that ran it after starting the scheduler would race
// the scheduler for the operation belonging to the worker being swept.
class FakeSweep final : public IStartupSweep {
public:
    explicit FakeSweep(std::vector<std::string>& log) noexcept : log_(log) {}

    Result<void> run_sweep() override {
        log_.push_back("sweep.run");
        ++runs;
        if (fail) {
            return Result<void>::failure(
                {ErrorDomain::internal, 9, L"startup_sweep", L"injected", 0});
        }
        return Result<void>::success();
    }

    bool needs_attention() const noexcept override { return attention; }

    int runs{};
    bool fail{};
    bool attention{};

private:
    std::vector<std::string>& log_;
};

} // namespace

// ---------------------------------------------------------------------------
// The no-disk lifecycle, which is T1-02's acceptance criterion.
// ---------------------------------------------------------------------------
TEST_CASE(service_host_starts_and_stops_cleanly_with_no_disks) {
    FakeClock clock;
    FakeScheduler scheduler;
    // A store with no profiles at all: the state a fresh install is in.
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);

    REQUIRE_EQ(host.state(), ServiceHostState::constructed);
    REQUIRE(!host.running());

    REQUIRE(host.start().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::running);
    REQUIRE(host.running());

    // Nothing was recovered, and the service still came up.  This is the fact
    // the whole case exists for: "no work to reconcile" is a healthy start, not
    // a degenerate one.
    const auto diagnostics = host.diagnostics();
    REQUIRE_EQ(diagnostics.recovered_operations, 0U);
    REQUIRE(diagnostics.scheduler_running);
    REQUIRE(!diagnostics.last_error.has_value());

    REQUIRE(host.stop().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::stopped);
    REQUIRE(!host.running());
    REQUIRE(!scheduler.is_running());
}

TEST_CASE(service_host_recovers_before_it_admits_work) {
    // The order is the correctness of the thing, not a preference: a scheduler
    // polling while the recovery is still rewriting operation records can claim
    // an operation whose ownership the recovery has not decided yet.
    //
    // Asserted by *failing the recovery* and watching nothing get started.  A
    // hook that only recorded the order would prove the sequence and not the
    // consequence; this proves the consequence -- the executor is never admitted
    // over a store that could not be reconciled.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    operations.fail_reconcile = true;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);

    REQUIRE(!host.start().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::failed);
    // The load-bearing assertion: the recovery failed, and the scheduler was
    // never started, so nothing was ever admitted over unreconciled state.
    REQUIRE_EQ(scheduler.events.size(), 0U);
    REQUIRE(!scheduler.is_running());
}

TEST_CASE(service_host_does_not_reach_the_scheduler_when_recovery_fails) {
    // The same property from the other side: an SCM that reported RUNNING over a
    // store the service could not reconcile would be claiming a healthy service
    // over unknown durable state.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    operations.fail_reconcile = true;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);
    REQUIRE(!host.start().ok());

    const auto diagnostics = host.diagnostics();
    REQUIRE_EQ(diagnostics.state, ServiceHostState::failed);
    REQUIRE(diagnostics.last_error.has_value());
    REQUIRE(!diagnostics.scheduler_running);
}

TEST_CASE(service_host_stop_is_idempotent_and_safe_before_start) {
    // The SCM sends a stop to a service whose start failed, and a restart path
    // can stop a host twice.  Neither is a shutdown failure, and reporting one
    // would make a machine going down look like a broken service.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);

    // Never started: a stop answers, and does not reach the scheduler.
    REQUIRE(host.stop().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::stopped);
    REQUIRE_EQ(scheduler.events.size(), 0U);

    // Started, stopped, stopped again: the second stop must not ask a scheduler
    // that is already down to stop again.
    REQUIRE(host.start().ok());
    REQUIRE(host.stop().ok());
    const auto after_first = scheduler.events.size();
    REQUIRE(host.stop().ok());
    REQUIRE_EQ(scheduler.events.size(), after_first);
}

TEST_CASE(service_host_start_is_idempotent_while_running) {
    // Re-entering start on a running service must not tear it down to bring it
    // back up: the observable damage would be a scheduler stopped and restarted
    // under operations that were mid-flight.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);
    REQUIRE(host.start().ok());
    const auto after_first = scheduler.events.size();

    REQUIRE(host.start().ok());
    REQUIRE_EQ(scheduler.events.size(), after_first);
    REQUIRE(host.running());

    REQUIRE(host.stop().ok());
}

TEST_CASE(service_host_reports_a_failed_scheduler_start_as_not_running) {
    // A service that reconciled its store but cannot execute is not running.
    // Reporting RUNNING over it would describe a service that accepts every
    // command and never carries one out.
    FakeClock clock;
    FakeScheduler scheduler;
    scheduler.fail_start = true;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);

    REQUIRE(!host.start().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::failed);
    REQUIRE(!host.running());
    REQUIRE(host.diagnostics().last_error.has_value());
}

TEST_CASE(service_host_does_not_claim_stopped_when_the_scheduler_will_not_stop) {
    // The scheduler holds the core, and the core holds the stores.  Declaring
    // the service stopped while its executor is still polling would let the
    // process tear down objects a thread is still calling into -- so the failure
    // is reported and the state is *not* `stopped`.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHost host(clock, core, scheduler);
    REQUIRE(host.start().ok());

    scheduler.fail_stop = true;
    REQUIRE(!host.stop().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::failed);
    REQUIRE(host.diagnostics().last_error.has_value());
}

TEST_CASE(service_host_stop_budget_becomes_the_scheduler_deadline) {
    // The budget has to travel as a *deadline*, because that is what the seam
    // takes, and it has to be derived from the clock rather than invented -- a
    // host that guessed would give the scheduler a deadline that has nothing to
    // do with when the shutdown started.
    FakeClock clock;
    clock.monotonic = 12'345;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHostOptions options{};
    options.stop_budget_ms = 4'000;
    ServiceHost host(clock, core, scheduler, options);
    REQUIRE(host.start().ok());
    REQUIRE(host.stop().ok());

    REQUIRE_EQ(scheduler.stop_deadlines.size(), 1U);
    REQUIRE_EQ(scheduler.stop_deadlines[0], 16'345ULL);
}

TEST_CASE(service_host_preshutdown_is_a_stop_and_keeps_the_same_budget) {
    // A preshutdown is the same work under a different (and non-negotiable)
    // budget.  What matters here is that it is not a *different* code path: a
    // second shutdown implementation is one that can disagree with the first.
    FakeClock clock;
    clock.monotonic = 900;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHostOptions options{};
    options.stop_budget_ms = 60'000;
    ServiceHost host(clock, core, scheduler, options);
    REQUIRE(host.start().ok());
    REQUIRE(host.preshutdown().ok());

    REQUIRE_EQ(host.state(), ServiceHostState::stopped);
    REQUIRE(!scheduler.is_running());
    REQUIRE_EQ(scheduler.stop_deadlines.size(), 1U);
    REQUIRE_EQ(scheduler.stop_deadlines[0], 60'900ULL);
    // And the machine going away is answered again rather than refused.
    REQUIRE(host.preshutdown().ok());
    REQUIRE_EQ(scheduler.stop_deadlines.size(), 1U);
}

// ---------------------------------------------------------------------------
// The start-up sweep's place in the sequence.
// ---------------------------------------------------------------------------

TEST_CASE(m5_host_sweeps_between_the_recovery_and_the_first_claim) {
    // The ordering is the correctness, not a preference.  After the recovery,
    // because the recovery says which non-terminal operations are still owned --
    // sweeping first would judge a worker against a binding the recovery is about
    // to rewrite.  Before the scheduler, because the scheduler's first act is to
    // claim one of those operations, and it may belong to the worker being swept.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    FakeSweep sweep(scheduler.events);

    ServiceHostOptions options{};
    ServiceHost host(clock, core, scheduler, options, &sweep);
    REQUIRE(host.start().ok());

    // The scheduler's own log carries the sweep, so the relative order is
    // asserted rather than assumed: recovery is recorded by the fixtures, the
    // sweep and the scheduler start are both in this one vector.
    const auto sweep_at = std::find(scheduler.events.begin(),
                                    scheduler.events.end(), "sweep.run");
    const auto start_at = std::find(scheduler.events.begin(),
                                   scheduler.events.end(), "scheduler.start");
    REQUIRE(sweep_at != scheduler.events.end());
    REQUIRE(start_at != scheduler.events.end());
    REQUIRE(sweep_at < start_at);
    REQUIRE_EQ(sweep.runs, 1);
    REQUIRE_EQ(host.state(), ServiceHostState::running);
    REQUIRE(!host.diagnostics().sweep_needs_attention);
}

TEST_CASE(m5_host_fails_the_start_when_the_sweep_cannot_run) {
    // Unlike a sweep that merely *found* something, one that could not run leaves
    // the service admitting work while a predecessor's worker may still hold a
    // device -- which is the state the sweep exists to prevent.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    FakeSweep sweep(scheduler.events);
    sweep.fail = true;

    ServiceHostOptions options{};
    ServiceHost host(clock, core, scheduler, options, &sweep);
    const auto started = host.start();
    REQUIRE(!started.ok());
    REQUIRE(started.error().operation == L"startup_sweep");
    REQUIRE_EQ(host.state(), ServiceHostState::failed);
    // And the scheduler was never started, so nothing can be admitted over a
    // sweep that did not happen.
    REQUIRE_EQ(scheduler.events.size(), 1U);
    REQUIRE_EQ(scheduler.events[0], std::string("sweep.run"));
    REQUIRE(!scheduler.running);
}

TEST_CASE(m5_host_starts_anyway_when_the_sweep_needs_attention) {
    // The default, and it is a choice: a machine whose leftover worker will not die
    // still has other disks the operator wants served, and refusing to start turns
    // one stuck orphan into a service that is down.  The condition is *reported*
    // instead, which is the part that must not be lost.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    FakeSweep sweep(scheduler.events);
    sweep.attention = true;

    ServiceHostOptions options{};
    ServiceHost host(clock, core, scheduler, options, &sweep);
    REQUIRE(host.start().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::running);
    // Visible, not silent.
    REQUIRE(host.diagnostics().sweep_needs_attention);
}

TEST_CASE(m5_host_can_be_configured_to_refuse_a_start_over_the_sweep) {
    // The other side of the same choice, available for a deployment that would
    // rather be down than serve alongside an orphan it cannot account for.
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    FakeSweep sweep(scheduler.events);
    sweep.attention = true;

    ServiceHostOptions options{};
    options.fail_start_when_sweep_needs_attention = true;
    ServiceHost host(clock, core, scheduler, options, &sweep);
    const auto started = host.start();
    REQUIRE(!started.ok());
    REQUIRE_EQ(host.state(), ServiceHostState::failed);
    REQUIRE(host.diagnostics().sweep_needs_attention);
}

TEST_CASE(m5_host_without_a_sweep_still_starts) {
    // A host with nothing to sweep is a legitimate configuration, and its
    // diagnostics must read as "nothing to report" rather than as "unknown".
    FakeClock clock;
    FakeScheduler scheduler;
    Profiles profiles(provisioning_profile());
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;

    ServiceCore core(clock, profiles, operations, worker, logger, ids, memory);
    ServiceHostOptions options{};
    ServiceHost host(clock, core, scheduler, options);
    REQUIRE(host.start().ok());
    REQUIRE_EQ(host.state(), ServiceHostState::running);
    REQUIRE(!host.diagnostics().sweep_needs_attention);
}

// **The step a client needs before it can be a client, tested where it can be tested.**
//
// Building a request frame from a command is the join between the codec and the frame, and it did not exist --
// which is why the service's whole command surface had no client and `public_pipe_client` was compiled but
// unused.  A real client cannot be verified without a real service, but this join can: build a frame from a
// command, encode it to bytes, decode it back, and recover the command.  If any of the three disagree the round
// trip fails here rather than as an unexplained protocol error later.
//
// `disk list` is the command used, because it is the one the read/write separation acceptance needs, and because
// it carries no arguments -- so a payload mismatch would show up as a mismatch rather than as a field difference
// with two possible readings.
TEST_CASE(service_host_builds_a_request_frame_that_round_trips) {
    auto request_id = product::Uuid::random();
    REQUIRE(request_id.ok());
    auto frame = ipc::PublicPipeClient::build_command_request(
        control::DiskListCommand{}, request_id.value());
    REQUIRE(frame.ok());
    REQUIRE(frame.value().message_type == ipc::MessageType::request);
    REQUIRE(frame.value().request_id == request_id.value());
    REQUIRE(frame.value().flags == 0);
    REQUIRE_EQ(frame.value().command_id,
               static_cast<std::uint32_t>(ipc::CommandId::disk_list));

    // Through the wire format and back: the exact pair the client and the service exchange, so a disagreement
    // here is the one that would otherwise surface as a malformed frame at run time.
    auto bytes = ipc::encode_frame(frame.value());
    REQUIRE(bytes.ok());
    auto decoded = ipc::decode_frame(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().request_id == request_id.value());
    REQUIRE_EQ(decoded.value().command_id, frame.value().command_id);

    // And the command itself, which is what the service will execute.
    auto command = ipc::decode_command_payload(
        static_cast<ipc::CommandId>(decoded.value().command_id),
        decoded.value().payload, decoded.value().protocol_minor);
    REQUIRE(command.ok());
    REQUIRE(std::holds_alternative<control::DiskListCommand>(command.value()));
}

TEST_CASE(service_host_carries_the_control_flags_and_claims_no_operation) {
    auto request_id = product::Uuid::random();
    REQUIRE(request_id.ok());
    auto frame = ipc::PublicPipeClient::build_command_request(
        control::StatusCommand{}, request_id.value(), true, false);
    REQUIRE(frame.ok());
    REQUIRE((frame.value().flags & ipc::kFrameFlagNoWait) != 0);
    REQUIRE((frame.value().flags & ipc::kFrameFlagDryRun) == 0);

    // A client owns no operation, so the frame must not claim one.  Asserted against a nil UUID rather than as
    // "not equal to something", because the service reads a non-nil operation id as one it has already admitted.
    const auto nil = product::Uuid::parse("00000000-0000-0000-0000-000000000000");
    REQUIRE(nil.ok());
    REQUIRE(frame.value().operation_id == nil.value());
}
