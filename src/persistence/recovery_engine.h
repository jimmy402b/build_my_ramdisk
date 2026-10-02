#pragma once

#include "src/common/operation_context.h"
#include "src/persistence/composite_backing_store.h"
#include "src/persistence/manifest_format.h"
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
    // The native operation id that the committed checkpoint recorded for this
    // generation.  It is decoded from the journal's own commit record
    // (`JournalRecordCommon::operation_id`) on the read path, exactly like the
    // rest of recovery, and never from the write-side JournalIndexBuildPlan.
    // It is the only read-side source of that id, and the M5 recovery proof
    // depends on it.
    std::uint64_t journal_operation_id{};
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

// Re-reads one manifest slot image, byte for byte, exactly as recovery read it.
//
// This exists so the M5 recovery inspection can hash the committed candidate's
// exact on-disk bytes rather than a re-encoded copy: the digest it produces has
// to be comparable with what the write side froze at prepare.  It is read-only
// — it opens the bundle for inspection, takes no writer transaction, no delete
// guard and no runtime, and it never writes.
[[nodiscard]] Result<ManifestSlotBytes> read_manifest_slot_bytes(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name,
    std::uint8_t slot_index);

}  // namespace myrd::persistence
