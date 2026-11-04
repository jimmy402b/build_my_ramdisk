#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/m3/m3_lifecycle_runner.h"
#include "src/persistence/recovery_engine.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace myrd {

// Composition-only options for one M4 persistence bundle.  The adapter owns
// no runtime/device state; it only translates product options to the accepted
// M3 persistence requests.
struct M4PersistenceOptions final {
    std::filesystem::path bundle_root;
    std::uint64_t virtual_bytes{};
    std::string manifest_name{"disk.rdm"};
    // The image the generation-zero request must be created *with*, when the
    // caller already knows which image this bundle is meant to hold.  The
    // service derives the image id from the create request before the bundle
    // exists and validates the bundle's evidence against it afterwards, so a
    // randomly generated identity could never be recorded.  Absent means the
    // caller does not care -- which is what the foreground M4 path is.
    //
    // The persistence layer's own uuid type, because this is the
    // `GenerationZeroCreateRequest` field: carrying the product uuid here would
    // only add a conversion between two sixteen-byte encodings.
    std::optional<persistence::ImageUuid> image_uuid;
    // The store budget this bundle's writer was opened with, when the caller
    // carries one.  It is not used by the adapter itself; it decides the
    // *reader* bound, because a checkpoint's journal is as large as the dirty
    // set the budget allows.  Zero keeps the unbudgeted production floor.
    std::uint64_t ram_limit_bytes{};
};

class M4PersistenceAdapter final {
public:
    explicit M4PersistenceAdapter(M4PersistenceOptions options) noexcept;

    [[nodiscard]] Result<persistence::RecoveredImage>
    create_generation_zero(const OperationContext& context) const;

    [[nodiscard]] Result<persistence::RecoveredImage>
    recover_complete(const OperationContext& context) const;

    [[nodiscard]] m3::M3LifecycleOutcome compact_offline(
        const OperationContext& context) const;

private:
    M4PersistenceOptions options_;
};

[[nodiscard]] Result<persistence::RecoveredImage>
m4_create_generation_zero(const OperationContext& context,
                          const std::filesystem::path& bundle_root,
                          std::uint64_t virtual_bytes);

[[nodiscard]] Result<persistence::RecoveredImage>
m4_recover_complete(const OperationContext& context,
                    const std::filesystem::path& bundle_root,
                    std::uint64_t virtual_bytes);

[[nodiscard]] m3::M3LifecycleOutcome
m4_compact_offline(const OperationContext& context,
                   const std::filesystem::path& bundle_root);

} // namespace myrd
