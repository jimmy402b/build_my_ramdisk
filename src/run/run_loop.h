#pragma once

#include "src/m3/m3_windows_acceptance.h"
#include "src/run/cli.h"
#include "src/run/windows_volume.h"

#include <atomic>
#include <chrono>

namespace myrd {

struct MaintenanceState final {
    std::chrono::steady_clock::time_point last_checkpoint{};
    std::chrono::steady_clock::time_point checkpoint_suppressed_until{};
    bool journal_warning_printed{};
    bool checkpoints_disabled_for_full_journal{};
    bool skip_final_checkpoint{};
    bool exit_failure{};
};

[[nodiscard]] bool checkpoint_due(const RunOptions&, const m3::M3MaintenanceStats&,
                                  const MaintenanceState&,
                                  std::chrono::steady_clock::time_point) noexcept;
[[nodiscard]] bool apply_checkpoint_outcome(MaintenanceState&,
    const m3::M3CheckpointOutcome&, std::chrono::steady_clock::time_point,
    bool retry_already_used) noexcept;
void mark_maintenance_stats_failure(MaintenanceState&) noexcept;
bool shutdown_busy_resume(std::atomic_bool&) noexcept;
int run_loop(m3::M3AimRuntime&, ProductVolume&, const RunOptions&, std::atomic_bool&);

} // namespace myrd
