#pragma once

#include "src/common/result.h"
#include "src/m3/m3_windows_acceptance.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace myrd {

struct ProductVolume final {
    std::wstring volume_guid_path;
    std::uint32_t physical_drive_number{};
    wchar_t drive_letter{};
};

Result<ProductVolume> provision_new_volume(const m3::M3AimRuntimeInfo&, wchar_t,
                                           std::chrono::milliseconds);
Result<ProductVolume> find_recovered_volume(const m3::M3AimRuntimeInfo&, wchar_t,
                                            std::chrono::milliseconds);
Result<void> flush_volume(const ProductVolume&);
Result<void> assign_drive_letter(ProductVolume&);
Result<void> remove_drive_letter(const ProductVolume&);

} // namespace myrd
