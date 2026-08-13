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

bool shutdown_busy_resume(std::atomic_bool& shutdown_requested) noexcept {
    shutdown_requested.store(false, std::memory_order_relaxed);
    return true;
}

namespace {
enum class ShutdownLock { Locked, Busy, Failed };
template <typename Lock, typename Verify, typename Flush, typename Checkpoint, typename Dismount, typename Remove, typename Stop>
int do_shutdown_actions(MaintenanceState& state, std::atomic_bool& requested, Lock&& lock,
    Verify&& verify, Flush&& flush, Checkpoint&& checkpoint, Dismount&& dismount, Remove&& remove, Stop&& stop) {
    const auto held = lock();
    if (held == ShutdownLock::Busy) { shutdown_busy_resume(requested); return -1; }
    if (held == ShutdownLock::Failed) { state.exit_failure = true; shutdown_busy_resume(requested); return -1; }
    bool failed = state.exit_failure;
    if (held == ShutdownLock::Locked) {
        if (!verify().ok()) { state.exit_failure = true; shutdown_busy_resume(requested); return -1; }
        const auto flushed = flush();
        if (!flushed.ok()) failed = true;
        if (flushed.ok() && !state.skip_final_checkpoint) {
            for (bool retry = false;; retry = true) {
                const auto result = checkpoint();
                if (result.disposition == persistence::CommitDisposition::Committed && result.ok()) break;
                if (journal_capacity_exhausted(result)) {
                    state.checkpoints_disabled_for_full_journal = true;
                    state.skip_final_checkpoint = true;
                    state.exit_failure = true;
                    failed = true;
                    break;
                }
                if ((result.disposition == persistence::CommitDisposition::NotCommitted ||
                    result.disposition == persistence::CommitDisposition::CommittedNeedsReconciliation) && !retry) {
                    if (!flush().ok()) { failed = true; break; }
                    continue;
                }
                if (result.disposition != persistence::CommitDisposition::NotCommitted || retry) { failed = true; break; }
            }
        } else if (state.skip_final_checkpoint) failed = true;
        if (const auto result = dismount(); !result.ok()) { failed = true; if (result.has_error()) std::wcerr << L"Error: " << result.error().operation << L": " << result.error().message << L"\n"; }
        if (const auto result = remove(); !result.ok()) { failed = true; if (result.has_error()) std::wcerr << L"Error: " << result.error().operation << L": " << result.error().message << L"\n"; }
    }
    if (const auto result = stop(); !result.ok()) { failed = true; if (result.has_error()) std::wcerr << L"Error: " << result.error().operation << L": " << result.error().message << L"\n"; }
    return failed ? 1 : 0;
}
int do_shutdown(m3::M3AimRuntime& runtime, ProductVolume& volume,
                MaintenanceState& state, std::atomic_bool& requested) {
    auto locked = lock_volume(volume);
    if (!locked.ok()) { std::wcerr << L"Error: volume lock failed: " << locked.error().message << L"\n"; state.exit_failure = true; shutdown_busy_resume(requested); return -1; }
    if (locked.value().busy) {
        std::wcerr << L"Close applications using the RAM disk and press Ctrl+C again. If the volume remains busy, temporarily pause or exclude Windows Defender real-time scanning and Windows Search indexing for this drive, then retry. Closing the terminal or killing `myrd.exe` is an unclean termination and requires a Windows reboot before the same bundle is used again.\n";
        shutdown_busy_resume(requested); return -1;
    }
    auto held = std::move(*locked.value().locked);
    const OperationContext context{{}, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
    const int result = do_shutdown_actions(state, requested, [] { return ShutdownLock::Locked; },
        [] { return Result<void>::success(); }, [&] { return held.flush(); }, [&] { return runtime.checkpoint(context, persistence::VolumeBarrierKind::WindowsVolumeFlushed); },
        [&] { return held.dismount(); }, [&] { return remove_drive_letter(volume); }, [&] { return runtime.stop(); });
    if (result == 0) std::wcout << L"Clean shutdown. Goodbye.\n";
    return result;
}
}

int run_loop(m3::M3AimRuntime& runtime, ProductVolume& volume,
             const RunOptions& options, std::atomic_bool& shutdown_requested) {
    MaintenanceState state{};
    state.last_checkpoint = std::chrono::steady_clock::now();
    const auto maintenance = [&] {
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
    };
    while (!shutdown_requested.load(std::memory_order_relaxed)) {
        wait_then_work(shutdown_requested,
            [] { std::this_thread::sleep_for(std::chrono::seconds(1)); }, maintenance);
    }
    for (;;) {
        const int shutdown = do_shutdown(runtime, volume, state, shutdown_requested);
        if (shutdown >= 0) return shutdown;
        while (!shutdown_requested.load(std::memory_order_relaxed)) {
            wait_then_work(shutdown_requested,
                [] { std::this_thread::sleep_for(std::chrono::seconds(1)); }, maintenance);
        }
    }
}

} // namespace myrd
