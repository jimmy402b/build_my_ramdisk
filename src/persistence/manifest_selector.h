#pragma once

#include "src/common/error.h"
#include "src/persistence/manifest_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace myrd::persistence {

// Stable validation order.  Task 26 must provide one explicit outcome for
// every boundary; a skipped (NotRun) boundary can never become selectable.
enum class ManifestCandidateCheck : std::uint8_t {
    SlotDecode = 0,
    SlotSupport,
    SlotSemantics,
    BasePath,
    JournalPath,
    BasePresence,
    BaseOpen,
    BaseIdentity,
    BaseExpectedLength,
    BaseProfile,
    BaseLayout,
    JournalPresence,
    JournalOpen,
    JournalIdentity,
    JournalHeader,
    JournalCommittedLength,
    JournalCommit,
    JournalRoot,
    JournalDigest,
    Count,
};

inline constexpr std::size_t kManifestCandidateCheckCount =
    static_cast<std::size_t>(ManifestCandidateCheck::Count);

enum class ManifestCandidateCheckStatus : std::uint8_t {
    NotRun = 0,
    Passed,
    Missing,
    Corrupt,
    Unsupported,
    AccessDenied,
    IoError,
    Cancelled,
    DeadlineExceeded,
    Conflict,
};

struct ManifestCandidateCheckEvidence final {
    ManifestCandidateCheckStatus status{
        ManifestCandidateCheckStatus::NotRun};
    std::optional<Error> provenance{};
};

struct ManifestCandidateEvidence final {
    std::uint8_t slot_index{};
    // A decoded slot may be retained for diagnostics.  Its generation becomes
    // selection authority only after decode, support and semantics all pass.
    std::optional<ManifestSlot> slot{};
    std::optional<CheckpointGeneration> trusted_generation{};
    std::array<ManifestCandidateCheckEvidence,
               kManifestCandidateCheckCount> checks{};
};

struct ManifestCandidateDiagnostic final {
    std::uint8_t slot_index{};
    std::optional<CheckpointGeneration> generation{};
    std::optional<ManifestCandidateCheck> first_failed_check{};
    ManifestCandidateCheckStatus status{
        ManifestCandidateCheckStatus::NotRun};
    std::optional<Error> provenance{};
    bool selected{};
};

struct ManifestSelection final {
    std::uint8_t slot_index{};
    ManifestSlot slot{};
    // True only when a decoded higher generation was rejected before this
    // complete predecessor was selected.
    bool fallback{};
};

struct ManifestSelectionOutcome final {
    std::optional<ManifestSelection> selection{};
    std::array<ManifestCandidateDiagnostic, 2> diagnostics{};
    std::optional<Error> error{};

    [[nodiscard]] bool ok() const noexcept {
        return selection.has_value() && !error.has_value();
    }
};

[[nodiscard]] bool same_durable_manifest(
    const ManifestSlot& left, const ManifestSlot& right) noexcept;

[[nodiscard]] ManifestSelectionOutcome
select_complete_manifest_generation(
    const std::array<ManifestCandidateEvidence, 2>& candidates);

}  // namespace myrd::persistence
