#include "tests/support/test_framework.h"

#include "src/run/run_loop.h"

#include <atomic>
#include <chrono>
#include <vector>

namespace {
using namespace std::chrono_literals;
using myrd::CheckpointAttemptHooks;
using myrd::CheckpointAttemptResult;
using myrd::CheckpointDispositionAdapter;
using myrd::CheckpointOutcomeAdapter;
using myrd::MaintenanceState;
using myrd::MaintenanceStatsAdapter;
using myrd::RunOptions;

// The M3 baseline's named capacity failure is mapped by the M4 adapter into
// the no-retry journal-capacity policy. No raw persistence enum values are
// permitted in this product-layer contract.
constexpr auto kJournalCapacityErrorCode =
    myrd::persistence::PersistenceErrorCode::DiskFull;

RunOptions options() {
    RunOptions value{};
    value.checkpoint_interval = 10s;
    value.checkpoint_dirty_bytes = 100;
    return value;
}

CheckpointOutcomeAdapter result(CheckpointDispositionAdapter disposition,
                                std::uint64_t generation = 0) {
    CheckpointOutcomeAdapter value{};
    value.m3_outcome.disposition = disposition;
    value.m3_outcome.generation = generation;
    return value;
}

CheckpointOutcomeAdapter capacity_outcome(
    bool valid = true, bool mark_adapter = true) {
    CheckpointOutcomeAdapter value{};
    value.m3_outcome.disposition = CheckpointDispositionAdapter::NotCommitted;
    const auto error_code = valid
        ? static_cast<std::uint32_t>(kJournalCapacityErrorCode)
        : 99U;
    const auto context_code = valid
        ? kJournalCapacityErrorCode
        : myrd::persistence::PersistenceErrorCode::FatalInternalInvariant;
    value.m3_outcome.error = myrd::Error{
        valid ? myrd::ErrorDomain::persistence : myrd::ErrorDomain::internal,
        error_code,
        L"checkpoint",
        L"journal capacity exhausted"};
    value.m3_outcome.error_context = myrd::persistence::PersistenceErrorContext{
        .operation = "checkpoint",
        .code = context_code,
        .phase = myrd::persistence::SessionPhase::Checkpointing,
        .operation_id = 1,
        .generation = 1,
        .disposition = myrd::persistence::CommitDisposition::NotCommitted};
    value.journal_capacity_exhausted = mark_adapter;
    return value;
}

} // namespace

TEST_CASE(m4_run_policy_due_at_exact_time_or_dirty_threshold) {
    const auto zero = std::chrono::steady_clock::time_point{};
    MaintenanceState state{};
    state.last_checkpoint = zero;
    MaintenanceStatsAdapter stats{};
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 9s));
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 10s));
    stats.dirty_bytes = 99;
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 1s));
    stats.dirty_bytes = 100;
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 1s));
}

TEST_CASE(m4_run_policy_suppression_expires_at_exact_boundary) {
    const auto zero = std::chrono::steady_clock::time_point{};
    MaintenanceState state{};
    state.checkpoint_suppressed_until = zero + 60s;
    const MaintenanceStatsAdapter stats{.dirty_bytes = 100};
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 59s));
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 60s));
}

TEST_CASE(m4_run_policy_flushes_before_each_checkpoint_and_retries_once) {
    MaintenanceState state{};
    std::vector<int> calls;
    int checkpoints{};
    const CheckpointAttemptHooks hooks{
        [&] { calls.push_back(1); return myrd::Result<void>::success(); },
        [&] {
            calls.push_back(2);
            return result(++checkpoints == 1
                ? CheckpointDispositionAdapter::NotCommitted
                : CheckpointDispositionAdapter::Committed, 7);
        }};
    const auto now = std::chrono::steady_clock::time_point{} + 7s;
    REQUIRE_EQ(myrd::perform_checkpoint_attempts(state, hooks, now),
               CheckpointAttemptResult::Committed);
    REQUIRE_EQ(calls, std::vector<int>({1, 2, 1, 2}));
    REQUIRE_EQ(state.last_checkpoint, now);
}

TEST_CASE(m4_run_policy_retry_already_used_disallows_not_committed_retry) {
    MaintenanceState state{};
    const auto now = std::chrono::steady_clock::time_point{} + 11s;
    REQUIRE(!myrd::apply_checkpoint_outcome(
        state, result(CheckpointDispositionAdapter::NotCommitted), now, true));
    REQUIRE_EQ(state.checkpoint_suppressed_until, now + 60s);
}

TEST_CASE(m4_run_policy_committed_records_checkpoint_time) {
    MaintenanceState state{};
    const auto now = std::chrono::steady_clock::time_point{} + 23s;
    REQUIRE(!myrd::apply_checkpoint_outcome(
        state, result(CheckpointDispositionAdapter::Committed, 7), now, false));
    REQUIRE_EQ(state.last_checkpoint, now);
}

TEST_CASE(m4_run_policy_flush_failure_never_calls_checkpoint) {
    MaintenanceState state{};
    int checkpoints{};
    const CheckpointAttemptHooks hooks{
        [&] { return myrd::Result<void>::failure(
            myrd::Error{myrd::ErrorDomain::win32, 1, L"flush", L"fake"}); },
        [&] { ++checkpoints; return result(CheckpointDispositionAdapter::Committed); }};
    REQUIRE_EQ(myrd::perform_checkpoint_attempts(state, hooks, {}),
               CheckpointAttemptResult::Failed);
    REQUIRE_EQ(checkpoints, 0);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_indeterminate_fails_and_skips_final_checkpoint) {
    MaintenanceState state{};
    REQUIRE(!myrd::apply_checkpoint_outcome(
        state, result(CheckpointDispositionAdapter::CommitIndeterminate), {}, false));
    REQUIRE(state.exit_failure);
    REQUIRE(state.skip_final_checkpoint);
}

TEST_CASE(m4_run_policy_committed_needs_reconciliation_is_terminal_for_this_attempt) {
    MaintenanceState state{};
    const auto now = std::chrono::steady_clock::time_point{} + 13s;
    REQUIRE(!myrd::apply_checkpoint_outcome(
        state,
        result(CheckpointDispositionAdapter::CommittedNeedsReconciliation, 9),
        now,
        false));
    REQUIRE(state.exit_failure);
    REQUIRE(!state.skip_final_checkpoint);
    REQUIRE_EQ(state.last_checkpoint, std::chrono::steady_clock::time_point{});
    REQUIRE_EQ(state.checkpoint_suppressed_until,
               std::chrono::steady_clock::time_point{});
}

TEST_CASE(m4_run_policy_capacity_exhaustion_is_terminal_without_retry) {
    MaintenanceState state{};
    const auto now = std::chrono::steady_clock::time_point{} + 4s;
    REQUIRE(!myrd::apply_checkpoint_outcome(
        state, capacity_outcome(), now, false));
    REQUIRE(state.checkpoints_disabled_for_full_journal);
    REQUIRE(state.skip_final_checkpoint);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_typed_m3_capacity_error_and_context_is_terminal) {
    MaintenanceState state{};
    const auto now = std::chrono::steady_clock::time_point{} + 19s;
    const auto capacity = capacity_outcome(true, false);
    REQUIRE(capacity.m3_outcome.error.has_value());
    REQUIRE(capacity.m3_outcome.error_context.has_value());
    REQUIRE_EQ(capacity.m3_outcome.error->domain, myrd::ErrorDomain::persistence);
    REQUIRE_EQ(capacity.m3_outcome.error->code,
               static_cast<std::uint32_t>(kJournalCapacityErrorCode));
    REQUIRE_EQ(capacity.m3_outcome.error_context->code,
               kJournalCapacityErrorCode);
    REQUIRE(!myrd::apply_checkpoint_outcome(state, capacity, now, false));
    REQUIRE(state.checkpoints_disabled_for_full_journal);
    REQUIRE(state.skip_final_checkpoint);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_capacity_attempt_calls_checkpoint_once_and_disables_future_attempts) {
    MaintenanceState state{};
    int flushes{};
    int checkpoints{};
    const CheckpointAttemptHooks hooks{
        [&] { ++flushes; return myrd::Result<void>::success(); },
        [&] { ++checkpoints; return capacity_outcome(); }};
    REQUIRE_EQ(myrd::perform_checkpoint_attempts(state, hooks, {}),
               CheckpointAttemptResult::Failed);
    REQUIRE_EQ(flushes, 1);
    REQUIRE_EQ(checkpoints, 1);
    REQUIRE(state.checkpoints_disabled_for_full_journal);
    REQUIRE_EQ(myrd::perform_checkpoint_attempts(state, hooks, 1s),
               CheckpointAttemptResult::Suppressed);
    REQUIRE_EQ(checkpoints, 1);
}

TEST_CASE(m4_run_policy_indeterminate_attempt_calls_checkpoint_once_without_retry) {
    MaintenanceState state{};
    int flushes{};
    int checkpoints{};
    const CheckpointAttemptHooks hooks{
        [&] { ++flushes; return myrd::Result<void>::success(); },
        [&] { ++checkpoints; return result(CheckpointDispositionAdapter::CommitIndeterminate); }};
    REQUIRE_EQ(myrd::perform_checkpoint_attempts(state, hooks, {}),
               CheckpointAttemptResult::Failed);
    REQUIRE_EQ(flushes, 1);
    REQUIRE_EQ(checkpoints, 1);
    REQUIRE(state.skip_final_checkpoint);
}

TEST_CASE(m4_run_policy_busy_shutdown_clears_request_without_cleanup) {
    MaintenanceState state{};
    std::atomic_bool requested{true};
    std::vector<int> calls;
    const myrd::ShutdownHooks hooks{
        [&] { calls.push_back(1); return myrd::ShutdownLockResult::Busy; },
        [&] { calls.push_back(2); return myrd::Result<void>::success(); },
        [&] { calls.push_back(3); return myrd::Result<void>::success(); },
        [&] { calls.push_back(4); return result(CheckpointDispositionAdapter::Committed); },
        [&] { calls.push_back(5); return myrd::Result<void>::success(); },
        [&] { calls.push_back(6); return myrd::Result<void>::success(); },
        [&] { calls.push_back(7); return myrd::Result<void>::success(); }};
    REQUIRE(myrd::perform_shutdown(state, requested, hooks) != 0);
    REQUIRE(!requested.load());
    REQUIRE(!state.exit_failure);
    REQUIRE(calls == std::vector<int>({1}));
}

TEST_CASE(m4_run_policy_loop_reads_stats_and_runs_due_checkpoint) {
    RunOptions run_options = options();
    std::atomic_bool requested{false};
    MaintenanceState state{};
    int ticks{};
    int checkpoints{};
    std::vector<int> calls;
    const auto zero = std::chrono::steady_clock::time_point{};
    auto now = zero;
    const myrd::RunLoopHooks hooks{
        [&] { return now; },
        [&] {
            calls.push_back(1);
            now += 10s;
            if (++ticks == 2) requested.store(true);
        },
        [&] { return MaintenanceStatsAdapter{}; },
        CheckpointAttemptHooks{
            [&] { calls.push_back(2); return myrd::Result<void>::success(); },
            [&] {
                calls.push_back(3);
                ++checkpoints;
                return result(CheckpointDispositionAdapter::Committed, 1);
            }},
        [&](MaintenanceState&, std::atomic_bool&) {
            calls.push_back(4);
            return 0;
        }};
    REQUIRE_EQ(myrd::run_loop(run_options, requested, hooks), 0);
    REQUIRE_EQ(checkpoints, 1);
    REQUIRE(calls == std::vector<int>({1, 2, 3, 1, 4}));
}

TEST_CASE(m4_run_policy_console_request_only_sets_shutdown_flag) {
    std::atomic_bool requested{false};
    REQUIRE(myrd::request_console_shutdown(requested, 0));
    REQUIRE(requested.load());
    requested.store(false);
    REQUIRE(!myrd::request_console_shutdown(requested, 99));
    REQUIRE(!requested.load());
}

TEST_CASE(m4_run_policy_final_shutdown_order_is_lock_verify_flush_checkpoint_cleanup_stop) {
    MaintenanceState state{};
    std::atomic_bool requested{true};
    std::vector<int> calls;
    const myrd::ShutdownHooks hooks{
        [&] { calls.push_back(1); return myrd::ShutdownLockResult::Locked; },
        [&] { calls.push_back(2); return myrd::Result<void>::success(); },
        [&] { calls.push_back(3); return myrd::Result<void>::success(); },
        [&] { calls.push_back(4); return result(CheckpointDispositionAdapter::Committed, 8); },
        [&] { calls.push_back(5); return myrd::Result<void>::success(); },
        [&] { calls.push_back(6); return myrd::Result<void>::success(); },
        [&] { calls.push_back(7); return myrd::Result<void>::success(); }};
    REQUIRE_EQ(myrd::perform_shutdown(state, requested, hooks), 0);
    REQUIRE(calls == std::vector<int>({1, 2, 3, 4, 5, 6, 7}));
}

TEST_CASE(m4_run_policy_shutdown_skips_final_checkpoint_after_verify_or_flush_failure) {
    for (const int failing_stage : {2, 3}) {
        MaintenanceState state{};
        std::atomic_bool requested{true};
        std::vector<int> calls;
        int final_checkpoints{};
        const myrd::ShutdownHooks hooks{
            [&] { calls.push_back(1); return myrd::ShutdownLockResult::Locked; },
            [&] {
                calls.push_back(2);
                return failing_stage == 2
                    ? myrd::Result<void>::failure(
                        myrd::Error{myrd::ErrorDomain::win32, 1, L"verify", L"fake"})
                    : myrd::Result<void>::success();
            },
            [&] {
                calls.push_back(3);
                return failing_stage == 3
                    ? myrd::Result<void>::failure(
                        myrd::Error{myrd::ErrorDomain::win32, 1, L"flush", L"fake"})
                    : myrd::Result<void>::success();
            },
            [&] {
                calls.push_back(4);
                ++final_checkpoints;
                return result(CheckpointDispositionAdapter::Committed);
            },
            [&] { calls.push_back(5); return myrd::Result<void>::success(); },
            [&] { calls.push_back(6); return myrd::Result<void>::success(); },
            [&] { calls.push_back(7); return myrd::Result<void>::success(); }};
        REQUIRE(myrd::perform_shutdown(state, requested, hooks) != 0);
        REQUIRE_EQ(final_checkpoints, 0);
        REQUIRE(calls == std::vector<int>({1, 2, 3, 5, 6, 7}));
    }
}
