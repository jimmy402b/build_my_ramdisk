#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace myrd::persistence {

// Stable, ordered audit points for one compaction operation. Before/after
// pairs intentionally reuse the frozen Task13 fault-registry identifiers where
// those identifiers already exist.
enum class CompactionPhase : std::uint8_t {
    OperationGateAcquired = 0,
    DurableViewCaptured,
    VhdxCreateBefore,
    VhdxCreateAfter,
    VhdxWriteBefore,
    VhdxWriteAfter,
    VhdxFlushBefore,
    VhdxFlushAfter,
    VhdxReadbackBefore,
    VhdxReadbackAfter,
    VhdxValidateBefore,
    VhdxValidateAfter,
    VhdxWindowsValidateBefore,
    VhdxWindowsValidateAfter,
    VhdxRenameBefore,
    VhdxRenameAfter,
    JournalCreateBefore,
    JournalCreateAfter,
    JournalFlushBefore,
    JournalFlushAfter,
    FilesReady,
    BarrierValidateBefore,
    BarrierValidateAfter,
    SnapshotBeginBefore,
    SnapshotBeginAfter,
    DeltaWriteBefore,
    DeltaWriteAfter,
    ManifestPublishBefore,
    ManifestPublishAfter,
    BackingInstallBefore,
    BackingInstallAfter,
    ConditionalAckBefore,
    ConditionalAckAfter,
    SnapshotFinishBefore,
    SnapshotFinishAfter,
    Complete,
};

[[nodiscard]] constexpr std::size_t compaction_phase_count() noexcept {
    return static_cast<std::size_t>(CompactionPhase::Complete) + 1;
}

[[nodiscard]] constexpr std::string_view to_string(
    CompactionPhase phase) noexcept;

struct CompactionRatio final {
    std::uint64_t numerator{};
    std::uint64_t denominator{};

    bool operator==(const CompactionRatio&) const noexcept = default;
};

// Measurements are supplied explicitly by the management layer. This type
// neither reads files nor owns a timer, scheduler or ImageSession.
struct CompactionMetrics final {
    std::uint64_t journal_physical_bytes{};
    std::uint64_t base_physical_bytes{};
    std::uint64_t checkpoints_since_compaction{};
    std::uint32_t index_depth{};

    bool operator==(const CompactionMetrics&) const noexcept = default;
};

struct CompactionPolicy final {
    std::optional<std::uint64_t> journal_physical_bytes_at_or_above{};
    std::optional<CompactionRatio> journal_to_base_ratio_at_or_above{};
    std::optional<std::uint64_t> checkpoints_at_or_above{};
    std::optional<std::uint32_t> index_depth_at_or_above{};

    bool operator==(const CompactionPolicy&) const noexcept = default;
};

struct CompactionRecommendation final {
    bool recommended{};
    bool policy_valid{true};
    bool journal_physical_bytes{};
    bool journal_to_base_ratio{};
    bool checkpoint_count{};
    bool index_depth{};

    bool operator==(const CompactionRecommendation&) const noexcept = default;
};

// Pure policy evaluation only. It never begins a session operation and never
// schedules compaction. An invalid configured ratio is reported fail-closed.
[[nodiscard]] CompactionRecommendation should_compact(
    const CompactionMetrics& metrics,
    const CompactionPolicy& policy) noexcept;

constexpr std::string_view to_string(const CompactionPhase phase) noexcept {
    switch (phase) {
    case CompactionPhase::OperationGateAcquired:
        return "operation-gate-acquired";
    case CompactionPhase::DurableViewCaptured:
        return "durable-view-captured";
    case CompactionPhase::VhdxCreateBefore:
        return "compaction.vhdx.create.before";
    case CompactionPhase::VhdxCreateAfter:
        return "compaction.vhdx.create.after";
    case CompactionPhase::VhdxWriteBefore:
        return "compaction.vhdx.write.before";
    case CompactionPhase::VhdxWriteAfter:
        return "compaction.vhdx.write.after";
    case CompactionPhase::VhdxFlushBefore:
        return "compaction.vhdx.flush.before";
    case CompactionPhase::VhdxFlushAfter:
        return "compaction.vhdx.flush.after";
    case CompactionPhase::VhdxReadbackBefore:
        return "compaction.vhdx.readback.before";
    case CompactionPhase::VhdxReadbackAfter:
        return "compaction.vhdx.readback.after";
    case CompactionPhase::VhdxValidateBefore:
        return "compaction.vhdx.validate.before";
    case CompactionPhase::VhdxValidateAfter:
        return "compaction.vhdx.validate.after";
    case CompactionPhase::VhdxWindowsValidateBefore:
        return "compaction.vhdx.windows_validate.before";
    case CompactionPhase::VhdxWindowsValidateAfter:
        return "compaction.vhdx.windows_validate.after";
    case CompactionPhase::VhdxRenameBefore:
        return "compaction.vhdx.rename.before";
    case CompactionPhase::VhdxRenameAfter:
        return "compaction.vhdx.rename.after";
    case CompactionPhase::JournalCreateBefore:
        return "compaction.journal.create.before";
    case CompactionPhase::JournalCreateAfter:
        return "compaction.journal.create.after";
    case CompactionPhase::JournalFlushBefore:
        return "compaction.journal.flush.before";
    case CompactionPhase::JournalFlushAfter:
        return "compaction.journal.flush.after";
    case CompactionPhase::FilesReady:
        return "files-ready";
    case CompactionPhase::BarrierValidateBefore:
        return "barrier-validate-before";
    case CompactionPhase::BarrierValidateAfter:
        return "barrier-validate-after";
    case CompactionPhase::SnapshotBeginBefore:
        return "snapshot-begin-before";
    case CompactionPhase::SnapshotBeginAfter:
        return "snapshot-begin-after";
    case CompactionPhase::DeltaWriteBefore:
        return "delta-write-before";
    case CompactionPhase::DeltaWriteAfter:
        return "delta-write-after";
    case CompactionPhase::ManifestPublishBefore:
        return "manifest-publish-before";
    case CompactionPhase::ManifestPublishAfter:
        return "manifest-publish-after";
    case CompactionPhase::BackingInstallBefore:
        return "backing-install-before";
    case CompactionPhase::BackingInstallAfter:
        return "backing-install-after";
    case CompactionPhase::ConditionalAckBefore:
        return "conditional-ack-before";
    case CompactionPhase::ConditionalAckAfter:
        return "conditional-ack-after";
    case CompactionPhase::SnapshotFinishBefore:
        return "snapshot-finish-before";
    case CompactionPhase::SnapshotFinishAfter:
        return "snapshot-finish-after";
    case CompactionPhase::Complete:
        return "complete";
    }
    return "unknown";
}

}  // namespace myrd::persistence
