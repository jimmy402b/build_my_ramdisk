// The startup sweep's orchestration, over fakes.
//
// `worker_sweep_policy.h` and `worker_process_source.h` decide *what* a sweep may
// touch; this decides in what order it does it, what it does when a termination
// fails, and what it reports.  Those are the parts a live test cannot pin down,
// because a real orphan is not reproducible on demand -- so the order, the
// escalation and the report are measured here and the real thing is measured on
// the guest.
//
// What is worth guarding above all: the sweep must not turn a *failure to read
// the durable bindings* into a mass termination.  With no bindings every orphan
// looks foreign, and a sweep that proceeded would kill processes that were
// legitimately mid-teardown.  That case has its own test.

#include "tests/support/test_framework.h"

#include "src/management/worker_sweep.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace myrd;
using namespace myrd::management;

namespace {

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

product::Uuid profile_a() {
    return uuid("aaaaaaaa-1111-2222-3333-444444444444");
}

OrphanedProcess orphan(std::uint32_t pid,
                       std::optional<std::uint64_t> creation_time,
                       std::optional<product::Uuid> profile) {
    OrphanedProcess process{};
    process.process_id = pid;
    process.process_creation_time = creation_time;
    process.profile_id = std::move(profile);
    return process;
}

// Records what it was asked to do, and can be told to refuse either call.
class FakeWorkerProcessSource final : public IWorkerProcessSource {
public:
    std::vector<OrphanedProcess> orphans;
    bool observe_fails{false};
    bool cooperative_succeeds{false};
    bool terminate_succeeds{true};

    // In call order, so a test can assert that an escalation followed a refusal
    // rather than replaced it.
    std::vector<std::string> calls;

    [[nodiscard]] Result<std::vector<OrphanedProcess>> observe_orphans()
        const override {
        if (observe_fails) {
            return Result<std::vector<OrphanedProcess>>::failure(
                {ErrorDomain::internal, 1, L"observe_orphans",
                 L"the process table could not be read", 0});
        }
        return Result<std::vector<OrphanedProcess>>::success(orphans);
    }

    [[nodiscard]] Result<void> request_cooperative_exit(
        const OrphanedProcess& process) override {
        calls.push_back("ask:" + std::to_string(process.process_id));
        if (!cooperative_succeeds) {
            return Result<void>::failure(
                {ErrorDomain::internal, 2, L"request_cooperative_exit",
                 L"no reachable channel", 0});
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> terminate_process(
        const OrphanedProcess& process) override {
        calls.push_back("kill:" + std::to_string(process.process_id));
        if (!terminate_succeeds) {
            return Result<void>::failure(
                {ErrorDomain::internal, 3, L"terminate_process",
                 L"the worker process could not be ended", 0});
        }
        return Result<void>::success();
    }
};

class FakeWorkerBindings final : public IExpectedWorkerBindings {
public:
    std::vector<ExpectedWorkerBinding> bindings;
    bool fails{false};

    [[nodiscard]] Result<std::vector<ExpectedWorkerBinding>> expected()
        const override {
        if (fails) {
            return Result<std::vector<ExpectedWorkerBinding>>::failure(
                {ErrorDomain::internal, 4, L"expected",
                 L"the durable profile store could not be opened", 0});
        }
        return Result<std::vector<ExpectedWorkerBinding>>::success(bindings);
    }
};

} // namespace

TEST_CASE(m5_sweep_terminates_an_orphan_it_cannot_place) {
    // The kill-and-recover case in miniature: a process in the job that no profile
    // claims.  It goes, and it goes *forced* -- there is no profile whose ordered
    // teardown it could be asked to run.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(4242, 99, std::nullopt)};
    FakeWorkerBindings bindings;

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().orphans_observed, 1U);
    REQUIRE_EQ(report.value().forced_to_terminate, 1U);
    REQUIRE_EQ(report.value().failed_to_terminate, 0U);
    REQUIRE(!report.value().needs_attention());
    REQUIRE_EQ(source.calls.size(), 1U);
    REQUIRE_EQ(source.calls[0], std::string("kill:4242"));
}

TEST_CASE(m5_sweep_refuses_to_mass_terminate_when_bindings_are_unreadable) {
    // The dangerous shortcut.  Unreadable bindings make every orphan look like "no
    // binding we recognise", and the obvious implementation would therefore force
    // every one of them down -- killing workers that were legitimately ours and
    // mid-teardown.  A store that failed to open must stop the sweep, not redirect
    // it.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(1, 10, std::nullopt), orphan(2, 20, profile_a())};
    FakeWorkerBindings bindings;
    bindings.fails = true;

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(!report.ok());
    REQUIRE(report.error().operation == L"expected");
    // Nothing was touched: not one termination call reached the source.
    REQUIRE(source.calls.empty());
}

TEST_CASE(m5_sweep_fails_rather_than_reporting_a_clean_world) {
    // An enumeration that fails is not "there was nothing to sweep".  Reporting
    // success there is the one answer that is certainly wrong, because it is the
    // answer that ends the investigation.
    FakeWorkerProcessSource source;
    source.observe_fails = true;
    FakeWorkerBindings bindings;

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(!report.ok());
    REQUIRE(report.error().operation == L"observe_orphans");
}

TEST_CASE(m5_sweep_leaves_an_unreachable_orphan_and_says_so) {
    // The default for a real orphan.  Its previous session's startup secret died
    // with that session, so the private channel cannot be reached and "asked it
    // down" is unavailable.  The honest answer is to report it rather than to
    // pretend the request landed -- and *not* to silently escalate, because
    // ending a process that may be running its ordered teardown is a decision, not
    // a default.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(7, 30, profile_a())};
    FakeWorkerBindings bindings;
    // A recorded binding that matches, with the profile not expected running, so
    // the verdict is cooperative rather than forced.
    bindings.bindings = {ExpectedWorkerBinding{profile_a(), 30, false}};

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().asked_to_terminate, 1U);
    REQUIRE_EQ(report.value().forced_to_terminate, 0U);
    REQUIRE_EQ(report.value().left_alone, 1U);
    REQUIRE_EQ(report.value().failed_to_terminate, 0U);
    // Needs attention: an orphan the sweep could not attribute still holds
    // whatever device it was given.
    REQUIRE(report.value().needs_attention());
    REQUIRE(!report.value().first_failure.empty());
    REQUIRE_EQ(source.calls.size(), 1U);
    REQUIRE_EQ(source.calls[0], std::string("ask:7"));
}

TEST_CASE(m5_sweep_escalates_only_when_asked_to) {
    // With `force_when_unreachable`, the same orphan is asked and then ended, and
    // the call order shows both -- so a future change that skipped the ask and
    // went straight to the kill would fail here.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(7, 30, profile_a())};
    FakeWorkerBindings bindings;
    bindings.bindings = {ExpectedWorkerBinding{profile_a(), 30, false}};

    WorkerSweepOptions options{};
    options.force_when_unreachable = true;
    const auto report = run_worker_sweep(source, bindings, options);
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().asked_to_terminate, 1U);
    REQUIRE_EQ(report.value().forced_to_terminate, 1U);
    REQUIRE_EQ(report.value().left_alone, 0U);
    REQUIRE(!report.value().needs_attention());
    REQUIRE_EQ(source.calls.size(), 2U);
    REQUIRE_EQ(source.calls[0], std::string("ask:7"));
    REQUIRE_EQ(source.calls[1], std::string("kill:7"));
}

TEST_CASE(m5_sweep_adopts_a_worker_whose_disk_should_be_serving) {
    // The one verdict that is not a termination, and the reason it must not be:
    // the operator expects that disk to be running, and stopping it is the
    // profile's own start/stop path's decision.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(9, 40, profile_a())};
    FakeWorkerBindings bindings;
    bindings.bindings = {ExpectedWorkerBinding{profile_a(), 40, true}};

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().adopted, 1U);
    REQUIRE_EQ(report.value().acted_on(), 0U);
    REQUIRE(source.calls.empty());
}

TEST_CASE(m5_sweep_reports_a_termination_that_did_not_work) {
    // A worker that will not die still holds its device, so this is a
    // needs-attention condition rather than a footnote -- and the sweep must carry
    // on to the other orphans rather than aborting, which is why the report is a
    // value and not an error.
    //
    // Note the two *different* failures in this list, because conflating them is
    // the easy mistake and the first version of this case did exactly that:
    //
    //   pid 1  ours, matching binding, not expected running  -> asked to leave;
    //          the ask is refused and escalation was not permitted, so it is
    //          LEFT ALONE.  Nothing was attempted that could fail.
    //   pid 2  no binding at all                             -> forced, and the
    //          kill itself fails, so it is a FAILED TERMINATION.
    //
    // A sweep that counted the first as a failed termination would be reporting a
    // kill it never attempted.
    FakeWorkerProcessSource source;
    source.orphans = {orphan(1, 10, profile_a()), orphan(2, 20, std::nullopt)};
    source.terminate_succeeds = false;
    FakeWorkerBindings bindings;
    bindings.bindings = {ExpectedWorkerBinding{profile_a(), 10, false}};

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().orphans_observed, 2U);
    REQUIRE_EQ(report.value().asked_to_terminate, 1U);
    REQUIRE_EQ(report.value().left_alone, 1U);
    REQUIRE_EQ(report.value().failed_to_terminate, 1U);
    REQUIRE(report.value().needs_attention());
    REQUIRE(!report.value().first_failure.empty());
    // Both were acted on: the first refusal did not stop the second orphan from
    // being dealt with, which is the property that keeps one stubborn process from
    // leaving the rest of the job unswept.
    REQUIRE_EQ(source.calls.size(), 2U);  // ask:1, then kill:2
    REQUIRE_EQ(source.calls[0], std::string("ask:1"));
    REQUIRE_EQ(source.calls[1], std::string("kill:2"));
}

TEST_CASE(m5_sweep_with_no_orphans_is_a_clean_report) {
    // A fresh service finds an empty job, and that is a normal start rather than a
    // degenerate sweep.  Asserted so a future "at least one orphan" assumption
    // fails loudly.
    FakeWorkerProcessSource source;
    FakeWorkerBindings bindings;

    const auto report = run_worker_sweep(source, bindings, WorkerSweepOptions{});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().orphans_observed, 0U);
    REQUIRE(!report.value().needs_attention());
    REQUIRE(source.calls.empty());
}
