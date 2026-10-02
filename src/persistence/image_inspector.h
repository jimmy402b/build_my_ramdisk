#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/composite_backing_store.h"
#include "src/persistence/recovery_engine.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace myrd::persistence {

inline constexpr std::uint32_t kImageInspectionSchemaVersion = 1;

struct ImageInspectOptions final {
    std::string manifest_relative_name{};
    RecoveryOptions recovery{};
};

struct SelectedImageInspection final {
    ImageGeometry geometry{};
    ImageUuid image_uuid{};
    CheckpointGeneration checkpoint_generation{};
    BaseGeneration base_generation{};
    JournalEpoch journal_epoch{};
    std::uint8_t selected_slot{};
    bool fallback{};
    CompositeBackingIdentity backing_identity{};
};

// A corruption/no-selection result is still a successful inspection: callers
// receive both slot diagnostics and a stable rejection error without any
// repair attempt. Operational failures such as access denial/cancellation stay
// in the outer Result.
struct ImageInspection final {
    std::uint32_t schema_version{kImageInspectionSchemaVersion};
    std::array<RecoveryCandidateDiagnostic, 2> diagnostics{};
    std::optional<SelectedImageInspection> selected{};
    std::optional<Error> rejection{};
};

enum class VerificationDepth : std::uint8_t {
    Structural,
    Full,
};

struct ImageVerificationReport final {
    std::uint32_t schema_version{kImageInspectionSchemaVersion};
    VerificationDepth depth{VerificationDepth::Structural};
    bool verified{};
    ImageInspection inspection{};
    std::uint64_t candidate_blocks{};
    std::uint64_t data_blocks{};
    std::uint64_t zero_blocks{};
    std::uint64_t data_bytes_hashed{};
    std::string logical_sha256{};
    std::optional<Error> failure{};
};

[[nodiscard]] Result<ImageInspection> inspect_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const ImageInspectOptions& options);

[[nodiscard]] Result<ImageVerificationReport> verify_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    VerificationDepth depth,
    const ImageInspectOptions& options);

// ---------------------------------------------------------------------------
// M5 two-phase checkpoint recovery inspection
// ---------------------------------------------------------------------------
// What the service durably recorded at P4 when it accepted a prepare.  It is
// the only thing a fresh worker may judge ownership against: the originating
// worker's in-process transaction died with it, so "did this candidate reach
// the bundle?" has to be answered from bytes on disk.
struct CheckpointRecoveryCorrelation final {
    ImageUuid image_uuid{};
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t native_operation_id{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};

    bool operator==(const CheckpointRecoveryCorrelation&) const noexcept =
        default;
};

enum class CheckpointRecoveryVerdict : std::uint8_t {
    // The bundle does not support either claim.  This is not a failure to be
    // retried and not a licence to roll back: it forbids confirming the
    // operation, deleting its resources, or replaying start/checkpoint/stop.
    OwnershipUnprovable = 0,
    // The bundle is still at the generation the candidate was built on, so the
    // checkpoint definitively did not land.
    PriorGenerationProved = 1,
    // The committed generation is this operation's candidate.
    CandidateGenerationProved = 2,
};

// One bit per condition that must hold for CandidateGenerationProved
// (§2.6).  Zero means every condition held.
enum class CheckpointRecoveryMismatch : std::uint32_t {
    None = 0,
    ImageUuid = 1U << 0,
    PriorGeneration = 1U << 1,
    CandidateGeneration = 1U << 2,
    NativeOperationId = 1U << 3,
    ManifestSlot = 1U << 4,
    ManifestDigest = 1U << 5,
};

inline constexpr std::uint32_t kCheckpointCandidateConditions =
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::ImageUuid) |
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::PriorGeneration) |
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::CandidateGeneration) |
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::NativeOperationId) |
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::ManifestSlot) |
    static_cast<std::uint32_t>(CheckpointRecoveryMismatch::ManifestDigest);

// What the bundle actually shows.  Every field is read from the committed
// manifest slot, the committed journal commit record, or the selected slot's
// own bytes -- never from a writer's plan object.
struct CheckpointRecoveryInspection final {
    ImageUuid image_uuid{};
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t journal_operation_id{};
    std::uint8_t selected_manifest_slot{};
    Sha256Digest selected_manifest_digest{};
    CheckpointRecoveryVerdict verdict{
        CheckpointRecoveryVerdict::OwnershipUnprovable};
    // Which of the six candidate conditions failed.  Zero exactly when the
    // verdict is CandidateGenerationProved; all six when the bundle could not
    // be read far enough to verify any of them.
    std::uint32_t candidate_mismatch_mask{kCheckpointCandidateConditions};
};

// Read-only.  Opens no runtime, holds no writer transaction and modifies
// nothing.  The outer Result fails only for operational or format errors that
// make the bundle unreadable; a readable bundle that does not match `expected`
// is a successful inspection whose verdict is OwnershipUnprovable, so the
// caller can see *what* was observed rather than only that it disagreed.
[[nodiscard]] Result<CheckpointRecoveryInspection> inspect_checkpoint(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const ImageInspectOptions& options,
    const CheckpointRecoveryCorrelation& expected);

} // namespace myrd::persistence
