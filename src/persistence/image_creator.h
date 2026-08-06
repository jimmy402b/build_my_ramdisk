#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/vhdx_writer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#if defined(MYRD_ENABLE_TEST_SEAMS)
#include <functional>
#endif

namespace myrd::persistence {

struct GenerationZeroCreateRequest final {
    std::filesystem::path bundle_root;
    std::string manifest_relative_name;
    std::string base_temporary_name;
    std::string base_relative_name;
    std::string journal_relative_name;
    ImageGeometry geometry{};
    ImageUuid image_uuid{};
    VhdxWriterIdentity base_identity{};
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{1};
    std::uint64_t operation_id{1};
    std::uint64_t created_time_100ns{};
    // Optional product reserve retained after accounting for the exact empty
    // VHDX, genesis journal and manifest bytes. Tests use this to exercise the
    // low-space gate without consuming the host volume.
    std::uint64_t free_space_reserve_bytes{};
    VhdxWriterLimits vhdx_limits{};
    RecoveryOptions recovery{};
};

// Claims a previously absent manifest name and creates a completely empty
// generation-zero image. Success is returned only after all creation handles
// are closed and Task 26 recovery independently reopens generation zero.
[[nodiscard]] Result<RecoveredImage> create_generation_zero_image(
    const OperationContext& context,
    const GenerationZeroCreateRequest& request);

#if defined(MYRD_ENABLE_TEST_SEAMS)
enum class GenerationZeroCreatePhase : std::uint8_t {
    BeforeManifestClaim,
    AfterManifestClaim,
    BeforeBasePublish,
    AfterBasePublish,
    BeforeJournalCreate,
    BeforeJournalFlush,
    BeforeJournalReadback,
    BeforeManifestInitialization,
    BeforeManifestSlotWrite,
    BeforeManifestSlotFlush,
    BeforeManifestSlotReadback,
    BeforeRecovery,
};

using GenerationZeroCreateHook =
    std::function<Result<void>(GenerationZeroCreatePhase)>;

[[nodiscard]] Result<RecoveredImage>
create_generation_zero_image_for_testing(
    const OperationContext& context,
    const GenerationZeroCreateRequest& request,
    GenerationZeroCreateHook hook);
#endif

}  // namespace myrd::persistence
