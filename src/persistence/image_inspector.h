#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
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

} // namespace myrd::persistence
