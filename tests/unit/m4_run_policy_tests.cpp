#include "test_framework.h"
#include "src/run/run_loop.h"
#include "src/run/run_loop.cpp"

#include <sstream>
#include <shellapi.h>

using namespace myrd;

namespace {
using namespace std::chrono_literals;

myrd::RunOptions options() {
    myrd::RunOptions value{};
    value.checkpoint_interval = 10s;
    value.checkpoint_dirty_bytes = 100;
    return value;
}

myrd::m3::M3CheckpointOutcome outcome(
    myrd::persistence::CommitDisposition disposition,
    std::optional<myrd::persistence::PersistenceErrorCode> code = std::nullopt,
    std::uint64_t generation = 0) {
    myrd::m3::M3CheckpointOutcome value{};
    value.disposition = disposition;
    value.generation = generation;
    if (code) value.error_context = myrd::persistence::PersistenceErrorContext{.code = *code};
    return value;
}

myrd::m3::M3CheckpointOutcome capacity_outcome(bool valid = true) {
    auto value = outcome(myrd::persistence::CommitDisposition::NotCommitted,
        myrd::persistence::PersistenceErrorCode::JournalCapacityExhausted);
    value.error = myrd::Error{valid ? myrd::ErrorDomain::persistence : myrd::ErrorDomain::internal,
        valid ? static_cast<std::uint32_t>(myrd::persistence::PersistenceErrorCode::JournalCapacityExhausted) : 99,
        L"checkpoint", L"failure"};
    return value;
}

}

TEST_CASE(m4_run_policy_due_at_exact_time_or_dirty_threshold) {
    const auto zero = std::chrono::steady_clock::time_point{};
    myrd::MaintenanceState state{};
    state.last_checkpoint = zero;
    auto stats = myrd::m3::M3MaintenanceStats{};
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 9s));
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 10s));
    stats.dirty_bytes = 99;
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 1s));
    stats.dirty_bytes = 100;
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 1s));
}

TEST_CASE(m4_run_policy_suppression_prevents_due_until_exactly_expired) {
    const auto zero = std::chrono::steady_clock::time_point{};
    myrd::MaintenanceState state{};
    state.checkpoint_suppressed_until = zero + 60s;
    auto stats = myrd::m3::M3MaintenanceStats{.dirty_bytes = 100};
    REQUIRE(!myrd::checkpoint_due(options(), stats, state, zero + 59s));
    REQUIRE(myrd::checkpoint_due(options(), stats, state, zero + 60s));
}

TEST_CASE(m4_run_policy_not_committed_requests_one_immediate_retry_then_suppresses) {
    const auto zero = std::chrono::steady_clock::time_point{};
    myrd::MaintenanceState state{};
    REQUIRE(myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::NotCommitted), zero, false));
    REQUIRE(!myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::NotCommitted), zero, true));
    REQUIRE_EQ(state.checkpoint_suppressed_until, zero + 60s);
}

TEST_CASE(m4_run_policy_second_not_committed_warns_once_and_suppresses_for_sixty_seconds) {
    const auto zero = std::chrono::steady_clock::time_point{};
    myrd::MaintenanceState state{};
    int flushes{};
    int checkpoints{};
    int warnings{};
    do_checkpoint_attempts(state, zero,
        [&] { ++flushes; return myrd::Result<void>::success(); },
        [&] { ++checkpoints; return outcome(myrd::persistence::CommitDisposition::NotCommitted); },
        [&] { ++warnings; });
    REQUIRE_EQ(flushes, 2);
    REQUIRE_EQ(checkpoints, 2);
    REQUIRE_EQ(warnings, 1);
    REQUIRE_EQ(state.checkpoint_suppressed_until, zero + 60s);
}

TEST_CASE(m4_run_policy_second_failure_suppresses_from_second_result_time) {
    myrd::MaintenanceState state{};
    const auto first = std::chrono::steady_clock::time_point{} + 7s;
    const auto second = std::chrono::steady_clock::time_point{} + 19s;
    int checkpoints{};
    do_checkpoint_attempts(state,
        [&] { return ++checkpoints == 1 ? first : second; },
        [] { return myrd::Result<void>::success(); },
        [] { return outcome(myrd::persistence::CommitDisposition::NotCommitted); },
        [] {});
    REQUIRE_EQ(state.checkpoint_suppressed_until, second + 60s);
}

TEST_CASE(m4_run_policy_committed_records_result_time) {
    myrd::MaintenanceState state{};
    const auto committed = std::chrono::steady_clock::time_point{} + 23s;
    do_checkpoint_attempts(state, [&] { return committed; },
        [] { return myrd::Result<void>::success(); },
        [] { return outcome(myrd::persistence::CommitDisposition::Committed); }, [] {});
    REQUIRE_EQ(state.last_checkpoint, committed);
}

TEST_CASE(m4_run_policy_reconciliation_retries_only_after_a_second_flush) {
    myrd::MaintenanceState state{};
    std::vector<int> calls;
    do_checkpoint_attempts(state, {},
        [&] { calls.push_back(1); return myrd::Result<void>::success(); },
        [&] { calls.push_back(2); return outcome(calls.size() == 2
            ? myrd::persistence::CommitDisposition::CommittedNeedsReconciliation
            : myrd::persistence::CommitDisposition::Committed); },
        [] {});
    REQUIRE_EQ(calls.size(), std::size_t{4});
    REQUIRE_EQ(calls[0], 1); REQUIRE_EQ(calls[1], 2);
    REQUIRE_EQ(calls[2], 1); REQUIRE_EQ(calls[3], 2);
}

TEST_CASE(m4_run_policy_reconciled_commit_uses_second_result_time) {
    myrd::MaintenanceState state{};
    const auto first = std::chrono::steady_clock::time_point{} + 5s;
    const auto second = std::chrono::steady_clock::time_point{} + 29s;
    int checkpoints{};
    int times{};
    do_checkpoint_attempts(state, [&] { return ++times == 1 ? first : second; },
        [] { return myrd::Result<void>::success(); },
        [&] { return ++checkpoints == 1
            ? outcome(myrd::persistence::CommitDisposition::CommittedNeedsReconciliation)
            : outcome(myrd::persistence::CommitDisposition::Committed); }, [] {});
    REQUIRE_EQ(state.last_checkpoint, second);
}

TEST_CASE(m4_run_policy_committed_checkpoint_prints_its_generation_once) {
    myrd::MaintenanceState state{};
    std::wostringstream captured;
    auto* previous = std::wcout.rdbuf(captured.rdbuf());
    do_checkpoint_attempts(state, {},
        [] { return myrd::Result<void>::success(); },
        [] { return outcome(myrd::persistence::CommitDisposition::Committed, {}, 41); },
        [] {});
    std::wcout.rdbuf(previous);
    REQUIRE_EQ(captured.str(), std::wstring(L"Checkpoint committed generation 41.\n"));
}

TEST_CASE(m4_run_policy_reconciled_commit_prints_only_final_generation_once) {
    myrd::MaintenanceState state{};
    int checkpoints{};
    std::wostringstream captured;
    auto* previous = std::wcout.rdbuf(captured.rdbuf());
    do_checkpoint_attempts(state, {},
        [] { return myrd::Result<void>::success(); },
        [&] { return ++checkpoints == 1
            ? outcome(myrd::persistence::CommitDisposition::CommittedNeedsReconciliation, {}, 41)
            : outcome(myrd::persistence::CommitDisposition::Committed, {}, 42); },
        [] {});
    std::wcout.rdbuf(previous);
    REQUIRE_EQ(captured.str(), std::wstring(L"Checkpoint committed generation 42.\n"));
}

TEST_CASE(m4_run_policy_unreconciled_not_committed_requests_controlled_failure_without_third_attempt) {
    myrd::MaintenanceState state{};
    std::vector<int> calls;
    do_checkpoint_attempts(state, {},
        [&] { calls.push_back(1); return myrd::Result<void>::success(); },
        [&] { calls.push_back(2); return outcome(calls.size() == 2
            ? myrd::persistence::CommitDisposition::CommittedNeedsReconciliation
            : myrd::persistence::CommitDisposition::NotCommitted); },
        [] {});
    REQUIRE_EQ(calls, std::vector<int>({1, 2, 1, 2}));
    REQUIRE(state.exit_failure);
    REQUIRE(!state.skip_final_checkpoint);
    REQUIRE_EQ(state.checkpoint_suppressed_until, std::chrono::steady_clock::time_point{});
}

TEST_CASE(m4_run_policy_unreconciled_indeterminate_requests_controlled_failure_and_skips_final) {
    myrd::MaintenanceState state{};
    std::vector<int> calls;
    do_checkpoint_attempts(state, {},
        [&] { calls.push_back(1); return myrd::Result<void>::success(); },
        [&] { calls.push_back(2); return outcome(calls.size() == 2
            ? myrd::persistence::CommitDisposition::CommittedNeedsReconciliation
            : myrd::persistence::CommitDisposition::CommitIndeterminate); },
        [] {});
    REQUIRE_EQ(calls, std::vector<int>({1, 2, 1, 2}));
    REQUIRE(state.exit_failure);
    REQUIRE(state.skip_final_checkpoint);
}

TEST_CASE(m4_run_policy_unreconciled_capacity_keeps_mounted_capacity_policy) {
    myrd::MaintenanceState state{};
    std::vector<int> calls;
    do_checkpoint_attempts(state, {},
        [&] { calls.push_back(1); return myrd::Result<void>::success(); },
        [&] {
            calls.push_back(2);
            return calls.size() == 2
                ? outcome(myrd::persistence::CommitDisposition::CommittedNeedsReconciliation)
                : capacity_outcome();
        },
        [] {});
    REQUIRE_EQ(calls, std::vector<int>({1, 2, 1, 2}));
    REQUIRE(state.checkpoints_disabled_for_full_journal);
    REQUIRE(state.skip_final_checkpoint);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_capacity_disables_all_checkpoints_without_retry) {
    const auto zero = std::chrono::steady_clock::time_point{};
    myrd::MaintenanceState state{};
    REQUIRE(!myrd::apply_checkpoint_outcome(state, capacity_outcome(), zero, false));
    REQUIRE(state.checkpoints_disabled_for_full_journal);
    REQUIRE(state.skip_final_checkpoint);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_invalid_capacity_context_keeps_ordinary_retry) {
    myrd::MaintenanceState state{};
    REQUIRE(myrd::apply_checkpoint_outcome(state, capacity_outcome(false), {}, false));
    REQUIRE(!state.checkpoints_disabled_for_full_journal);
}

TEST_CASE(m4_run_policy_wait_shutdown_skips_work_after_wait) {
    std::atomic_bool shutdown{false};
    int work{};
    const bool worked = wait_then_work(shutdown,
        [&] { shutdown.store(true, std::memory_order_relaxed); },
        [&] { ++work; });
    REQUIRE(!worked);
    REQUIRE_EQ(work, 0);
}

TEST_CASE(m4_shutdown_busy_clears_request_without_cleanup) {
    std::atomic_bool requested{true};
    REQUIRE(shutdown_busy_resume(requested));
    REQUIRE(!requested.load(std::memory_order_relaxed));
}

TEST_CASE(m4_shutdown_actions_freeze_order_retry_and_cleanup_after_failure) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    const auto exit = do_shutdown_actions(state, requested,
        [&] { calls.push_back(1); return ShutdownLock::Locked; },
        [&] { calls.push_back(2); return Result<void>::success(); },
        [&] { calls.push_back(3); return Result<void>::success(); },
        [&] { calls.push_back(4); return outcome(calls.size() == 4
            ? persistence::CommitDisposition::NotCommitted
            : persistence::CommitDisposition::Committed); },
        [&] { calls.push_back(5); return Result<void>::success(); },
        [&] { calls.push_back(6); return Result<void>::success(); },
        [&] { calls.push_back(7); return Result<void>::success(); });
    REQUIRE_EQ(exit, 0); REQUIRE_EQ(calls, std::vector<int>({1,2,3,4,3,4,5,6,7}));
}

TEST_CASE(m4_shutdown_actions_flush_failure_skips_checkpoint_but_continues_cleanup) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    const auto exit = do_shutdown_actions(state, requested, [&] { calls.push_back(1); return ShutdownLock::Locked; },
        [&] { calls.push_back(2); return Result<void>::success(); }, [&] { calls.push_back(3); return Result<void>::failure({}); }, [&] { calls.push_back(4); return outcome(persistence::CommitDisposition::Committed); },
        [&] { calls.push_back(5); return Result<void>::success(); }, [&] { calls.push_back(6); return Result<void>::success(); }, [&] { calls.push_back(7); return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(calls, std::vector<int>({1,2,3,5,6,7}));
}

TEST_CASE(m4_shutdown_actions_verify_failure_resumes_without_any_cleanup) {
    MaintenanceState state{}; std::atomic_bool requested{true}; int mutations{};
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [] { return Result<void>::failure({}); }, [&] { ++mutations; return Result<void>::success(); },
        [&] { ++mutations; return outcome(persistence::CommitDisposition::Committed); }, [&] { ++mutations; return Result<void>::success(); },
        [&] { ++mutations; return Result<void>::success(); }, [&] { ++mutations; return Result<void>::success(); });
    REQUIRE_EQ(exit, -1); REQUIRE(state.exit_failure); REQUIRE(!requested.load()); REQUIRE_EQ(mutations, 0);
}

TEST_CASE(m4_shutdown_actions_reconciliation_retries_after_a_second_flush) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    const auto exit = do_shutdown_actions(state, requested, [&] { calls.push_back(1); return ShutdownLock::Locked; },
        [&] { calls.push_back(2); return Result<void>::success(); }, [&] { calls.push_back(3); return Result<void>::success(); },
        [&] { calls.push_back(4); return outcome(calls.size() == 4 ? persistence::CommitDisposition::CommittedNeedsReconciliation : persistence::CommitDisposition::Committed); },
        [&] { calls.push_back(5); return Result<void>::success(); }, [&] { calls.push_back(6); return Result<void>::success(); }, [&] { calls.push_back(7); return Result<void>::success(); });
    REQUIRE_EQ(exit, 0); REQUIRE_EQ(calls, std::vector<int>({1,2,3,4,3,4,5,6,7}));
}

TEST_CASE(m4_shutdown_actions_rejects_committed_outcome_with_error) {
    MaintenanceState state{}; std::atomic_bool requested{true}; int cleanup{};
    auto invalid = outcome(persistence::CommitDisposition::Committed);
    invalid.error = Error{ErrorDomain::persistence, 1, L"checkpoint", L"error"};
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [] { return Result<void>::success(); }, [] { return Result<void>::success(); }, [&] { return invalid; },
        [&] { ++cleanup; return Result<void>::success(); }, [&] { ++cleanup; return Result<void>::success(); }, [&] { ++cleanup; return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(cleanup, 3);
}

TEST_CASE(m4_shutdown_actions_capacity_outcome_does_not_retry) {
    MaintenanceState state{}; std::atomic_bool requested{true}; int checkpoints{};
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [] { return Result<void>::success(); }, [] { return Result<void>::success(); }, [&] { ++checkpoints; return capacity_outcome(); },
        [] { return Result<void>::success(); }, [] { return Result<void>::success(); }, [] { return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(checkpoints, 1); REQUIRE(state.skip_final_checkpoint);
}

TEST_CASE(m4_shutdown_actions_busy_has_no_mutation_and_resumes) {
    MaintenanceState state{}; std::atomic_bool requested{true}; int mutations{};
    const auto exit = do_shutdown_actions(state, requested,
        [] { return ShutdownLock::Busy; }, [&] { ++mutations; return Result<void>::success(); }, [&] { ++mutations; return Result<void>::success(); },
        [] { return outcome(persistence::CommitDisposition::Committed); }, [&] { ++mutations; return Result<void>::success(); },
        [&] { ++mutations; return Result<void>::success(); }, [&] { ++mutations; return Result<void>::success(); });
    REQUIRE_EQ(exit, -1); REQUIRE_EQ(mutations, 0); REQUIRE(!requested.load());
}

TEST_CASE(m4_shutdown_actions_indeterminate_skips_final_but_cleans_up_and_fails) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    REQUIRE(!apply_checkpoint_outcome(state, outcome(persistence::CommitDisposition::CommitIndeterminate), {}, false));
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [&] { calls.push_back(2); return Result<void>::success(); }, [&] { calls.push_back(3); return Result<void>::success(); }, [&] { calls.push_back(4); return outcome(persistence::CommitDisposition::Committed); },
        [&] { calls.push_back(5); return Result<void>::success(); }, [&] { calls.push_back(6); return Result<void>::success(); }, [&] { calls.push_back(7); return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(calls, std::vector<int>({2,3,5,6,7}));
}

TEST_CASE(m4_shutdown_actions_capacity_state_skips_final_but_cleans_up_and_fails) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    REQUIRE(!apply_checkpoint_outcome(state, capacity_outcome(), {}, false));
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [&] { calls.push_back(1); return Result<void>::success(); }, [&] { calls.push_back(2); return Result<void>::success(); },
        [&] { calls.push_back(3); return outcome(persistence::CommitDisposition::Committed); }, [&] { calls.push_back(4); return Result<void>::success(); },
        [&] { calls.push_back(5); return Result<void>::success(); }, [&] { calls.push_back(6); return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(calls, std::vector<int>({1,2,4,5,6}));
}

TEST_CASE(m4_shutdown_actions_lock_failure_resumes_without_cleanup_then_preserves_failure) {
    MaintenanceState state{}; std::atomic_bool requested{true}; int mutations{};
    const auto first = do_shutdown_actions(state, requested, [] { return ShutdownLock::Failed; },
        [&] { ++mutations; return Result<void>::success(); }, [&] { ++mutations; return Result<void>::success(); },
        [] { return outcome(persistence::CommitDisposition::Committed); }, [&] { ++mutations; return Result<void>::success(); },
        [&] { ++mutations; return Result<void>::success(); }, [&] { ++mutations; return Result<void>::success(); });
    REQUIRE_EQ(first, -1); REQUIRE_EQ(mutations, 0); REQUIRE(!requested.load()); REQUIRE(state.exit_failure);
}

TEST_CASE(m4_shutdown_actions_cleanup_failure_wins_over_checkpoint_success) {
    MaintenanceState state{}; std::atomic_bool requested{true}; std::vector<int> calls;
    const auto exit = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [] { return Result<void>::success(); }, [] { return Result<void>::success(); }, [] { return outcome(persistence::CommitDisposition::Committed); },
        [&] { calls.push_back(4); return Result<void>::failure({}); }, [&] { calls.push_back(5); return Result<void>::success(); },
        [&] { calls.push_back(6); return Result<void>::success(); });
    REQUIRE_EQ(exit, 1); REQUIRE_EQ(calls, std::vector<int>({4,5,6}));
}

TEST_CASE(m4_run_policy_other_persistence_failure_keeps_normal_retry) {
    myrd::MaintenanceState state{};
    REQUIRE(myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::NotCommitted,
        myrd::persistence::PersistenceErrorCode::SnapshotResourceExhausted), {}, false));
    REQUIRE(!state.checkpoints_disabled_for_full_journal);
}

TEST_CASE(m4_run_policy_indeterminate_requests_failure_and_skips_final_checkpoint) {
    myrd::MaintenanceState state{};
    REQUIRE(!myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::CommitIndeterminate), {}, false));
    REQUIRE(state.skip_final_checkpoint);
    REQUIRE(state.exit_failure);
}

TEST_CASE(m4_run_policy_unresolved_reconciliation_requests_controlled_failure) {
    myrd::MaintenanceState state{};
    REQUIRE(!myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::CommittedNeedsReconciliation), {}, false));
    REQUIRE(state.exit_failure);
    REQUIRE(!state.skip_final_checkpoint);
}

TEST_CASE(m4_run_policy_committed_updates_last_checkpoint) {
    const auto now = std::chrono::steady_clock::time_point{} + 17s;
    myrd::MaintenanceState state{};
    REQUIRE(!myrd::apply_checkpoint_outcome(state, outcome(
        myrd::persistence::CommitDisposition::Committed), now, false));
    REQUIRE_EQ(state.last_checkpoint, now);
}

TEST_CASE(m4_run_policy_maintenance_stats_failure_preserves_final_checkpoint) {
    myrd::MaintenanceState state{};
    myrd::mark_maintenance_stats_failure(state);
    REQUIRE(state.exit_failure);
    REQUIRE(!state.skip_final_checkpoint);
}

TEST_CASE(m4_run_policy_journal_warning_is_thresholded_once_and_uses_actual_bundle) {
    myrd::MaintenanceState state{};
    myrd::RunOptions value{};
    value.bundle = L"C:\\bundles\\actual";
    std::wostringstream output;
    warn_once(state, {.journal_committed_bytes = 1717986918ull}, value, output);
    REQUIRE(output.str().empty());
    warn_once(state, {.journal_committed_bytes = 1717986919ull}, value, output);
    warn_once(state, {.journal_committed_bytes = 2147483648ull}, value, output);
    REQUIRE_EQ(output.str(), std::wstring(L"Warning: journal is 1717986919 of 2147483648 bytes. Press Ctrl+C for a clean shutdown, then run: myrd compact --bundle \"C:\\bundles\\actual\"\n"));
}

TEST_CASE(m4_run_policy_compact_command_quotes_windows_bundle_path_exactly) {
    const std::wstring path = L"C:\\bundle dir\\tail\\\"quoted\"";
    const auto command = compact_command_for(path);
    int argc{};
    auto* argv = CommandLineToArgvW((L"myrd " + command).c_str(), &argc);
    REQUIRE(argv != nullptr);
    REQUIRE_EQ(argc, 4);
    REQUIRE_EQ(std::wstring(argv[1]), std::wstring(L"compact"));
    REQUIRE_EQ(std::wstring(argv[2]), std::wstring(L"--bundle"));
    REQUIRE_EQ(std::wstring(argv[3]), path);
    LocalFree(argv);
}
