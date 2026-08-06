#pragma once

#include "src/common/operation_context.h"
#include "src/persistence/composite_backing_store.h"
#include "src/persistence/manifest_selector.h"
#include "src/persistence/persistence_types.h"
#include "src/storage/dynamic_block_store.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace myrd::persistence {

struct RecoveryOptions final {
    // No production default is intentionally provided: all startup scans and
    // caches are bounded by caller-validated finite limits.
    PersistenceLimits limits{};
};

// Public diagnostics retain stable machine-readable provenance but omit the
// source Error message and every path. safe_context is a fixed token string.
struct RecoveryCandidateDiagnostic final {
    std::uint8_t slot_index{};
    std::optional<CheckpointGeneration> generation{};
    std::optional<ManifestCandidateCheck> first_failed_check{};
    ManifestCandidateCheckStatus status{
        ManifestCandidateCheckStatus::NotRun};
    ErrorDomain error_domain{ErrorDomain::internal};
    std::uint32_t error_code{};
    std::uint32_t native_code{};
    bool has_error_provenance{};
    bool selected{};
    std::string safe_context{};
    std::uint64_t base_structural_read_calls{};
    std::uint64_t base_structural_read_bytes{};
    std::uint64_t base_bat_read_calls_at_open{};
    std::uint64_t base_payload_read_calls_at_open{};
};

struct RecoveredImage final {
    ImageGeometry geometry{};
    ImageUuid image_uuid{};
    CheckpointGeneration checkpoint_generation{};
    BaseGeneration base_generation{};
    JournalEpoch journal_epoch{};
    std::uint8_t selected_slot{};
    bool fallback{};
    ManifestSlot selected_manifest{};
    std::array<RecoveryCandidateDiagnostic, 2> diagnostics{};
    CompositeBackingIdentity backing_identity{};
    BackingView backing_view{};
};

// Unlike Result<RecoveredImage>, this preserves rejected-slot diagnostics when
// no candidate is recoverable. It remains an internal M3 recovery boundary;
// the later public open_image API may adapt it without losing the report.
struct RecoveryOutcome final {
    std::optional<RecoveredImage> image{};
    std::array<RecoveryCandidateDiagnostic, 2> diagnostics{};
    std::optional<Error> error{};

    [[nodiscard]] bool ok() const noexcept {
        return image.has_value() && !error.has_value();
    }
};

[[nodiscard]] RecoveryOutcome recover_complete_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name,
    const RecoveryOptions& options);

}  // namespace myrd::persistence
