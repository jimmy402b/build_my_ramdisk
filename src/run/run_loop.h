#pragma once

#include "src/common/result.h"
#include "src/m3/m3_provider.h"
#include "src/run/cli.h"
#include "src/run/disk_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace myrd {

// The loop consumes runtime observations through these adapter aliases.  The
// commit state remains the M3 persistence contract; M4 must not redefine it.
using MaintenanceStatsAdapter = DiskMaintenanceStats;
struct CheckpointOutcomeAdapter final {
    m3::M3CheckpointOutcome m3_outcome{};
    // Classification supplied by the runtime's typed persistence mapping;
    // commit disposition itself remains M3-owned.
    bool journal_capacity_exhausted{};

    // Keep persistence-specific capacity classification at the M4/M3
    // boundary.  Callers of the policy do not need to inspect raw error
    // domains or numeric codes.
    [[nodiscard]] bool capacity_exhausted() const noexcept {
        if (journal_capacity_exhausted) return true;
        if (m3_outcome.error_context.has_value() &&
            m3_outcome.error_context->code ==
                persistence::PersistenceErrorCode::DiskFull) {
            return true;
        }
        return m3_outcome.error.has_value() &&
               m3_outcome.error->domain == ErrorDomain::persistence &&
               m3_outcome.error->code == static_cast<std::uint32_t>(
                   persistence::PersistenceErrorCode::DiskFull);
    }
};
using CheckpointDispositionAdapter = persistence::CommitDisposition;

struct MaintenanceState final {
    std::chrono::steady_clock::time_point last_checkpoint{};
    std::chrono::steady_clock::time_point checkpoint_suppressed_until{};
    bool journal_warning_printed{};
    bool checkpoints_disabled_for_full_journal{};
    bool skip_final_checkpoint{};
    bool exit_failure{};
};

[[nodiscard]] bool checkpoint_due(
    const RunOptions&, const MaintenanceStatsAdapter&, const MaintenanceState&,
    std::chrono::steady_clock::time_point now) noexcept;

// Returns true only when one immediate retry of an ordinary NotCommitted
// result is permitted.  Indeterminate and capacity outcomes are terminal for
// this run and must never be retried blindly.
[[nodiscard]] bool apply_checkpoint_outcome(
    MaintenanceState&, const CheckpointOutcomeAdapter&,
    std::chrono::steady_clock::time_point now, bool retry_already_used) noexcept;

void mark_maintenance_stats_failure(MaintenanceState&) noexcept;

struct CheckpointAttemptHooks final {
    std::function<Result<void>()> flush_volume;
    std::function<CheckpointOutcomeAdapter()> checkpoint;
};

enum class CheckpointAttemptResult : std::uint8_t {
    Committed,
    Suppressed,
    Failed,
};

// Deterministic seam used by unit tests and by the concrete loop.  Every
// attempt flushes before invoking checkpoint; ordinary NotCommitted may be
// retried once, while capacity exhaustion disables subsequent attempts.
[[nodiscard]] CheckpointAttemptResult perform_checkpoint_attempts(
    MaintenanceState&, const CheckpointAttemptHooks&,
    std::chrono::steady_clock::time_point now) noexcept;

// Convenience seam for deterministic tests that express the clock value as
// a duration from the steady-clock epoch (for example, `1s`).  The concrete
// implementation remains time_point-based so callers cannot accidentally mix
// clocks; this overload performs the explicit conversion at the boundary.
template <class Rep, class Period>
[[nodiscard]] CheckpointAttemptResult perform_checkpoint_attempts(
    MaintenanceState& state, const CheckpointAttemptHooks& hooks,
    std::chrono::duration<Rep, Period> now) noexcept {
    return perform_checkpoint_attempts(
        state, hooks,
        std::chrono::steady_clock::time_point{
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                now)});
}

enum class ShutdownLockResult : std::uint8_t { Locked, Busy, Failed };

struct ShutdownHooks final {
    std::function<ShutdownLockResult()> lock_volume;
    std::function<Result<void>()> verify_volume;
    std::function<Result<void>()> flush_volume;
    std::function<CheckpointOutcomeAdapter()> final_checkpoint;
    std::function<Result<void>()> dismount_volume;
    std::function<Result<void>()> remove_drive_letter;
    std::function<Result<void>()> stop_runtime;
};

// Runtime loop dependencies are injectable so policy tests can advance a
// fake clock and request shutdown without sleeping or touching a device.
struct RunLoopHooks final {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void()> wait_one_second;
    std::function<MaintenanceStatsAdapter()> maintenance_stats;
    CheckpointAttemptHooks checkpoint;
    std::function<int(MaintenanceState&, std::atomic_bool&)> shutdown;
};

// Runs maintenance until the caller requests shutdown.  One iteration reads
// maintenance stats, evaluates checkpoint_due, and performs the durable
// flush/checkpoint policy.  The default wait is one second.
[[nodiscard]] int run_loop(
    const RunOptions&, std::atomic_bool&, const RunLoopHooks&) noexcept;

// Concrete DiskRuntime adapter used by the product entry point.  Tests should
// prefer the hook-based overload above to avoid devices and real sleeping.
[[nodiscard]] int run_loop(
    DiskRuntime&, const RunOptions&, std::atomic_bool&) noexcept;

// Console-control boundary.  The handler only records a shutdown request;
// all volume/checkpoint cleanup remains on the run-loop thread.
[[nodiscard]] bool request_console_shutdown(
    std::atomic_bool&, unsigned long event) noexcept;
[[nodiscard]] bool install_console_shutdown_handler(
    std::atomic_bool&) noexcept;
[[nodiscard]] bool uninstall_console_shutdown_handler() noexcept;

// Shutdown policy is deliberately expressed as callbacks so order and busy
// retry behavior can be tested without a device, filesystem, or sleeping.
[[nodiscard]] int perform_shutdown(
    MaintenanceState&, std::atomic_bool&, const ShutdownHooks&) noexcept;

} // namespace myrd
