#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/m3/m3_lifecycle_runner.h"
#include "src/persistence/recovery_engine.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace myrd {

// Composition-only options for one M4 persistence bundle.  The adapter owns
// no runtime/device state; it only translates product options to the accepted
// M3 persistence requests.
struct M4PersistenceOptions final {
    std::filesystem::path bundle_root;
    std::uint64_t virtual_bytes{};
    std::string manifest_name{"disk.rdm"};
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
