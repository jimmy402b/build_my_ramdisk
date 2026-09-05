#include "src/run/run_loop.h"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace myrd {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kRetrySuppression = std::chrono::seconds{60};

#ifdef _WIN32
std::atomic_bool* g_console_shutdown_request{};

BOOL WINAPI console_handler(DWORD event) noexcept {
    auto* request = g_console_shutdown_request;
    if (request == nullptr) return FALSE;
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT) {
        request->store(true);
        return TRUE;
    }
    return FALSE;
}
#endif

} // namespace

bool checkpoint_due(
    const RunOptions& options, const MaintenanceStatsAdapter& stats,
    const MaintenanceState& state, Clock::time_point now) noexcept {
    if (state.checkpoints_disabled_for_full_journal || state.exit_failure ||
        state.skip_final_checkpoint ||
        now < state.checkpoint_suppressed_until) {
        return false;
    }

    if (options.checkpoint_dirty_bytes != 0 &&
        stats.dirty_bytes >= options.checkpoint_dirty_bytes) {
        return true;
    }

    if (options.checkpoint_interval <= std::chrono::seconds::zero()) {
        return true;
    }
    if (now < state.last_checkpoint) {
        return false;
    }
    return now - state.last_checkpoint >= options.checkpoint_interval;
}

bool apply_checkpoint_outcome(
    MaintenanceState& state, const CheckpointOutcomeAdapter& outcome,
    Clock::time_point now, bool retry_already_used) noexcept {
    const auto disposition = outcome.m3_outcome.disposition;
    if (disposition == CheckpointDispositionAdapter::Committed) {
        state.last_checkpoint = now;
        state.checkpoint_suppressed_until = {};
        return false;
    }

    if (disposition == CheckpointDispositionAdapter::NotCommitted) {
        if (outcome.capacity_exhausted()) {
            state.checkpoints_disabled_for_full_journal = true;
            state.skip_final_checkpoint = true;
            state.exit_failure = true;
            return false;
        }
        if (!retry_already_used) {
            return true;
        }
        state.checkpoint_suppressed_until = now + kRetrySuppression;
        state.exit_failure = true;
        return false;
    }

    if (disposition == CheckpointDispositionAdapter::CommitIndeterminate) {
        state.exit_failure = true;
        state.skip_final_checkpoint = true;
        return false;
    }

    // A committed generation exists, but the provider could not reconcile its
    // post-commit state.  Do not retry blindly; leave final shutdown eligible
    // to perform its normal cleanup/reconciliation path.
    if (disposition ==
        CheckpointDispositionAdapter::CommittedNeedsReconciliation) {
        state.exit_failure = true;
        return false;
    }

    state.exit_failure = true;
    return false;
}

void mark_maintenance_stats_failure(MaintenanceState& state) noexcept {
    state.exit_failure = true;
}

CheckpointAttemptResult perform_checkpoint_attempts(
    MaintenanceState& state, const CheckpointAttemptHooks& hooks,
    Clock::time_point now) noexcept {
    if (state.checkpoints_disabled_for_full_journal || state.skip_final_checkpoint ||
        state.exit_failure || now < state.checkpoint_suppressed_until) {
        return CheckpointAttemptResult::Suppressed;
    }
    if (!hooks.flush_volume || !hooks.checkpoint) {
        mark_maintenance_stats_failure(state);
        return CheckpointAttemptResult::Failed;
    }

    bool retry_used = false;
    for (;;) {
        const auto flushed = hooks.flush_volume();
        if (!flushed.ok()) {
            mark_maintenance_stats_failure(state);
            return CheckpointAttemptResult::Failed;
        }

        const auto outcome = hooks.checkpoint();
        const bool retry = apply_checkpoint_outcome(
            state, outcome, now, retry_used);
        if (outcome.m3_outcome.disposition ==
            CheckpointDispositionAdapter::Committed) {
            return CheckpointAttemptResult::Committed;
        }
        if (!retry) {
            return CheckpointAttemptResult::Failed;
        }
        retry_used = true;
    }
}

int perform_shutdown(
    MaintenanceState& state, std::atomic_bool& requested,
    const ShutdownHooks& hooks) noexcept {
    if (!requested.exchange(false)) {
        return 0;
    }
    if (!hooks.lock_volume) {
        state.exit_failure = true;
        return 1;
    }
    const auto lock = hooks.lock_volume();
    if (lock == ShutdownLockResult::Busy) {
        return 1;
    }
    if (lock == ShutdownLockResult::Failed) {
        state.exit_failure = true;
        return 1;
    }

    int status = 0;
    const auto run = [&](const auto& operation) {
        if (!operation || !operation().ok()) {
            state.exit_failure = true;
            status = 1;
        }
    };

    run(hooks.verify_volume);
    run(hooks.flush_volume);

    // A final checkpoint is only safe after both native volume verification
    // and the pre-checkpoint filesystem flush have succeeded.  If either
    // operation failed, cleanup may continue but no durability claim may be
    // attempted on a volume whose identity/cache state was not established.
    if (status == 0 && !state.skip_final_checkpoint) {
        if (!hooks.final_checkpoint) {
            state.exit_failure = true;
            status = 1;
        } else {
            const auto outcome = hooks.final_checkpoint();
            (void)apply_checkpoint_outcome(state, outcome, Clock::now(), true);
            if (outcome.m3_outcome.disposition !=
                    CheckpointDispositionAdapter::Committed ||
                state.exit_failure) {
                status = 1;
            }
        }
    }

    run(hooks.dismount_volume);
    run(hooks.remove_drive_letter);
    run(hooks.stop_runtime);
    return status;
}

int run_loop(
    const RunOptions& options, std::atomic_bool& requested,
    const RunLoopHooks& hooks) noexcept {
    MaintenanceState state{};
    const auto now_fn = hooks.now ? hooks.now : [] {
        return Clock::now();
    };
    const auto wait_fn = hooks.wait_one_second ? hooks.wait_one_second : [] {
        std::this_thread::sleep_for(std::chrono::seconds{1});
    };
    const auto stats_fn = hooks.maintenance_stats;
    while (!requested.load()) {
        const auto now = now_fn();
        const auto stats = stats_fn ? stats_fn() : MaintenanceStatsAdapter{};
        if (checkpoint_due(options, stats, state, now)) {
            (void)perform_checkpoint_attempts(state, hooks.checkpoint, now);
        }
        if (requested.load()) break;
        wait_fn();
    }
    if (hooks.shutdown) return hooks.shutdown(state, requested);
    return state.exit_failure ? 1 : 0;
}

bool request_console_shutdown(
    std::atomic_bool& requested, unsigned long event) noexcept {
#ifdef _WIN32
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT) {
        requested.store(true);
        return true;
    }
#else
    (void)event;
#endif
    return false;
}

bool install_console_shutdown_handler(std::atomic_bool& requested) noexcept {
#ifdef _WIN32
    g_console_shutdown_request = &requested;
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        g_console_shutdown_request = nullptr;
        return false;
    }
    return true;
#else
    (void)requested;
    return false;
#endif
}

bool uninstall_console_shutdown_handler() noexcept {
#ifdef _WIN32
    const bool removed = SetConsoleCtrlHandler(console_handler, FALSE) != 0;
    g_console_shutdown_request = nullptr;
    return removed;
#else
    return false;
#endif
}

int run_loop(
    DiskRuntime& runtime, const RunOptions& options,
    std::atomic_bool& requested) noexcept {
    RunLoopHooks hooks{};
    hooks.maintenance_stats = [&runtime] { return runtime.maintenance_stats(); };
    hooks.checkpoint.flush_volume = [&runtime] { return runtime.flush_volume(); };
    hooks.checkpoint.checkpoint = [&runtime] {
        CheckpointOutcomeAdapter outcome{};
        const auto checkpoint = runtime.checkpoint();
        if (!checkpoint.ok()) {
            outcome.m3_outcome.error = checkpoint.error();
            return outcome;
        }
        switch (checkpoint.value().disposition) {
        case DiskCheckpointDisposition::committed:
            outcome.m3_outcome.disposition =
                CheckpointDispositionAdapter::Committed;
            break;
        case DiskCheckpointDisposition::commit_indeterminate:
            outcome.m3_outcome.disposition =
                CheckpointDispositionAdapter::CommitIndeterminate;
            break;
        case DiskCheckpointDisposition::not_committed:
            outcome.m3_outcome.disposition =
                CheckpointDispositionAdapter::NotCommitted;
            break;
        }
        outcome.m3_outcome.generation = checkpoint.value().generation;
        return outcome;
    };
    hooks.shutdown = [&runtime](MaintenanceState& state, std::atomic_bool&) {
        int status = 0;

        // The concrete product must preserve the same shutdown durability
        // boundary as the policy seam above: a final native-volume flush is
        // followed by one M3 checkpoint, and only then may shutdown_clean()
        // dismount/stop the runtime.  A maintenance failure (including an
        // indeterminate or capacity checkpoint) makes the final attempt
        // ineligible; cleanup still runs and reports its own failure.
        if (!state.exit_failure && !state.skip_final_checkpoint) {
            const auto flushed = runtime.flush_volume();
            if (!flushed.ok()) {
                state.exit_failure = true;
                status = 1;
            } else {
                CheckpointOutcomeAdapter outcome{};
                const auto checkpoint = runtime.checkpoint();
                if (!checkpoint.ok()) {
                    outcome.m3_outcome.error = checkpoint.error();
                    outcome.m3_outcome.disposition =
                        CheckpointDispositionAdapter::NotCommitted;
                } else {
                    outcome.m3_outcome.generation = checkpoint.value().generation;
                    switch (checkpoint.value().disposition) {
                    case DiskCheckpointDisposition::committed:
                        outcome.m3_outcome.disposition =
                            CheckpointDispositionAdapter::Committed;
                        break;
                    case DiskCheckpointDisposition::commit_indeterminate:
                        outcome.m3_outcome.disposition =
                            CheckpointDispositionAdapter::CommitIndeterminate;
                        break;
                    case DiskCheckpointDisposition::not_committed:
                        outcome.m3_outcome.disposition =
                            CheckpointDispositionAdapter::NotCommitted;
                        break;
                    }
                }
                (void)apply_checkpoint_outcome(
                    state, outcome, Clock::now(), true);
                if (outcome.m3_outcome.disposition !=
                        CheckpointDispositionAdapter::Committed ||
                    state.exit_failure) {
                    status = 1;
                }
            }
        }

        auto cleaned = runtime.shutdown_clean();
        // Console-close delivery can race volume teardown with the shell or
        // filesystem briefly releasing its last handle.  DiskRuntime keeps
        // exact ownership state and is explicitly retryable, so give a
        // transient cleanup failure a bounded opportunity to finish before
        // the process exits and strands an AIM device.
        for (unsigned attempt = 0; !cleaned.ok() && attempt != 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            cleaned = runtime.shutdown_clean();
        }
        if (!cleaned.ok()) status = 1;
        return status;
    };
    return run_loop(options, requested, hooks);
}

} // namespace myrd
