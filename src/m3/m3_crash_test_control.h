#pragma once

#include "src/common/result.h"
#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/commit_resolver.h"
#include "src/persistence/journal_writer.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace myrd::m3 {

#if defined(MYRD_ENABLE_TEST_SEAMS)

enum class M3CrashTestPhase : std::uint8_t {
    CheckpointPrepareBefore = 0,
    JournalFlushBefore,
    JournalFlushAfter,
    ManifestSlotWriteBefore,
    ManifestSlotWriteAfter,
    ManifestFlushBefore,
    ManifestFlushAfter,
    ManifestReadbackAfter,
    ReconcileBeforeRecoverView,
    ReconcileBeforeReportSuccess,
};

[[nodiscard]] Result<M3CrashTestPhase> parse_m3_crash_test_phase(
    std::string_view name);
[[nodiscard]] std::string_view to_string(M3CrashTestPhase phase) noexcept;
[[nodiscard]] std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    persistence::JournalWriterPhase phase) noexcept;
[[nodiscard]] std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    persistence::ManifestPublishTestPhase phase) noexcept;
[[nodiscard]] std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    persistence::CheckpointReconcileTestPhase phase) noexcept;

class M3CrashTestPauseController final {
public:
    static Result<M3CrashTestPauseController> create(
        M3CrashTestPhase target,
        std::filesystem::path signal_path,
        std::filesystem::path release_path,
        std::chrono::milliseconds timeout);

    [[nodiscard]] Result<void> observe(M3CrashTestPhase phase) const;

private:
    M3CrashTestPauseController(
        M3CrashTestPhase target,
        std::filesystem::path signal_path,
        std::filesystem::path release_path,
        std::chrono::milliseconds timeout) noexcept;

    M3CrashTestPhase target_{};
    std::filesystem::path signal_path_{};
    std::filesystem::path release_path_{};
    std::chrono::milliseconds timeout_{};
};

#endif

}  // namespace myrd::m3
