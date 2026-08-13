#include "src/run/run_loop.h"

#include "src/persistence/persistence_types.h"

#include <iostream>
#include <thread>

namespace myrd {
namespace {
constexpr std::uint64_t kJournalCeiling = 2147483648ull;
constexpr std::uint64_t kJournalWarning = 1717986919ull;

template <typename Wait, typename Work>
bool wait_then_work(std::atomic_bool& shutdown_requested, Wait&& wait, Work&& work) {
    wait();
    if (shutdown_requested.load(std::memory_order_relaxed)) return false;
    work();
    return true;
}

bool journal_capacity_exhausted(const m3::M3CheckpointOutcome& outcome) noexcept {
    return outcome.error.has_value() &&
        outcome.error->domain == ErrorDomain::persistence &&
        outcome.error->code == static_cast<std::uint32_t>(
            persistence::PersistenceErrorCode::JournalCapacityExhausted) &&
        outcome.error_context.has_value() &&
        outcome.error_context->code ==
            persistence::PersistenceErrorCode::JournalCapacityExhausted;
}

std::wstring quote_windows_argument(const std::wstring_view argument) {
    std::wstring quoted{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') quoted.append(slashes * 2 + 1, L'\\');
        else quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring compact_command_for(const std::wstring_view bundle) {
    return L"compact --bundle " + quote_windows_argument(bundle);
}

void warn_once(MaintenanceState& state, const m3::M3MaintenanceStats& stats,
               const RunOptions& options, std::wostream& output = std::wcerr) {
    if (state.journal_warning_printed || stats.journal_committed_bytes < kJournalWarning) return;
    state.journal_warning_printed = true;
    output << L"Warning: journal is " << stats.journal_committed_bytes
               << L" of " << kJournalCeiling
               << L" bytes. Press Ctrl+C for a clean shutdown, then run: myrd "
               << compact_command_for(options.bundle.wstring()) << L"\n";
}

void print_capacity_error(const RunOptions& options) {
    std::wcerr << L"Error: journal capacity is exhausted; new changes are not durable. Press Ctrl+C, then run: myrd "
               << compact_command_for(options.bundle.wstring())
               << L". Restart with the original myrd run command.\n";
}

template <typename Now, typename Flush, typename Checkpoint, typename Warning>
void do_checkpoint_attempts(MaintenanceState& state, Now&& now,
                            Flush&& flush, Checkpoint&& checkpoint,
                            Warning&& warning) {
    for (bool retried = false;; retried = true) {
        const auto flushed = flush();
        if (!flushed.ok()) {
            mark_maintenance_stats_failure(state);
            return;
        }
        const auto result = checkpoint();
        const auto result_time = now();
        if (result.disposition == persistence::CommitDisposition::CommittedNeedsReconciliation) {
            const auto flushed_again = flush();
            if (!flushed_again.ok()) { mark_maintenance_stats_failure(state); return; }
            const auto reconciled = checkpoint();
            const auto reconciled_time = now();
            if (journal_capacity_exhausted(reconciled)) {
                const bool retry_unused = apply_checkpoint_outcome(state, reconciled, reconciled_time, true);
                (void)retry_unused;
                return;
            }
            if (reconciled.disposition == persistence::CommitDisposition::Committed) {
                const bool retry_unused = apply_checkpoint_outcome(state, reconciled, reconciled_time, true);
                (void)retry_unused;
                std::wcout << L"Checkpoint committed generation "
                           << reconciled.generation << L".\n";
                return;
            }
            state.exit_failure = true;
            if (reconciled.disposition == persistence::CommitDisposition::CommitIndeterminate)
                state.skip_final_checkpoint = true;
            return;
        }
        const bool retry = apply_checkpoint_outcome(state, result, result_time, retried);
        if (!retry) {
            if (result.disposition == persistence::CommitDisposition::Committed)
                std::wcout << L"Checkpoint committed generation " << result.generation << L".\n";
            if (retried && result.disposition == persistence::CommitDisposition::NotCommitted) warning();
            return;
        }
    }
}

template <typename Flush, typename Checkpoint, typename Warning>
void do_checkpoint_attempts(MaintenanceState& state,
                            const std::chrono::steady_clock::time_point now,
                            Flush&& flush, Checkpoint&& checkpoint,
                            Warning&& warning) {
    do_checkpoint_attempts(state, [now] { return now; }, std::forward<Flush>(flush),
        std::forward<Checkpoint>(checkpoint), std::forward<Warning>(warning));
}

void do_checkpoint(m3::M3AimRuntime& runtime, ProductVolume& volume,
                   const RunOptions& options, MaintenanceState& state,
                   std::atomic_bool& shutdown_requested) {
    const OperationContext context{{}, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
    do_checkpoint_attempts(state, [] { return std::chrono::steady_clock::now(); },
        [&] { return flush_volume(volume); },
        [&] { return runtime.checkpoint(context, persistence::VolumeBarrierKind::WindowsVolumeFlushed); },
        [] { std::wcerr << L"Warning: checkpoint failed twice; suppressing checkpoints for 60 seconds.\n"; });
    if (state.checkpoints_disabled_for_full_journal) print_capacity_error(options);
    if (state.exit_failure && !state.checkpoints_disabled_for_full_journal)
        shutdown_requested.store(true, std::memory_order_relaxed);
}
} // namespace

bool checkpoint_due(const RunOptions& options, const m3::M3MaintenanceStats& stats,
                    const MaintenanceState& state,
                    const std::chrono::steady_clock::time_point now) noexcept {
    if (state.checkpoints_disabled_for_full_journal || now < state.checkpoint_suppressed_until)
        return false;
    return now - state.last_checkpoint >= options.checkpoint_interval ||
           stats.dirty_bytes >= options.checkpoint_dirty_bytes;
}

bool apply_checkpoint_outcome(MaintenanceState& state,
                              const m3::M3CheckpointOutcome& outcome,
                              const std::chrono::steady_clock::time_point now,
                              const bool retry_already_used) noexcept {
    if (journal_capacity_exhausted(outcome)) {
        state.checkpoints_disabled_for_full_journal = true;
        state.skip_final_checkpoint = true;
        state.exit_failure = true;
        return false;
    }
    if (outcome.disposition == persistence::CommitDisposition::Committed) {
        state.last_checkpoint = now;
        return false;
    }
    if (outcome.disposition == persistence::CommitDisposition::CommitIndeterminate) {
        state.skip_final_checkpoint = true;
        state.exit_failure = true;
        return false;
    }
    if (outcome.disposition == persistence::CommitDisposition::CommittedNeedsReconciliation) {
        state.exit_failure = true;
        return false;
    }
    if (outcome.disposition == persistence::CommitDisposition::NotCommitted && !retry_already_used)
        return true;
    state.checkpoint_suppressed_until = now + std::chrono::seconds(60);
    return false;
}

void mark_maintenance_stats_failure(MaintenanceState& state) noexcept {
    state.exit_failure = true;
}

int run_loop(m3::M3AimRuntime& runtime, ProductVolume& volume,
             const RunOptions& options, std::atomic_bool& shutdown_requested) {
    MaintenanceState state{};
    state.last_checkpoint = std::chrono::steady_clock::now();
    while (!shutdown_requested.load(std::memory_order_relaxed)) {
        wait_then_work(shutdown_requested,
            [] { std::this_thread::sleep_for(std::chrono::seconds(1)); }, [&] {
                const auto stats = runtime.maintenance_stats();
                if (!stats.ok()) {
                    std::wcerr << L"Error: maintenance statistics failed: " << stats.error().message << L"\n";
                    mark_maintenance_stats_failure(state);
                    shutdown_requested.store(true, std::memory_order_relaxed);
                    return;
                }
                warn_once(state, stats.value(), options);
                const auto now = std::chrono::steady_clock::now();
                if (checkpoint_due(options, stats.value(), state, now))
                    do_checkpoint(runtime, volume, options, state, shutdown_requested);
            });
    }
    return state.exit_failure ? 1 : 0;
}

} // namespace myrd
