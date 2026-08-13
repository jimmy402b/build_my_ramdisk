#pragma once

#include "src/common/result.h"
#include "src/m3/m3_windows_acceptance.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <windows.h>

namespace myrd {

struct ProductVolume final {
    std::wstring volume_guid_path;
    std::uint32_t physical_drive_number{};
    wchar_t drive_letter{};
};

struct VolumeLockAttempt;

class LockedVolume final {
public:
    LockedVolume(const LockedVolume&) = delete;
    LockedVolume& operator=(const LockedVolume&) = delete;
    LockedVolume(LockedVolume&&) noexcept;
    LockedVolume& operator=(LockedVolume&&) noexcept;
    ~LockedVolume() noexcept;

    Result<void> flush();
    Result<void> dismount();

private:
    friend Result<VolumeLockAttempt> lock_volume(const ProductVolume&);
    LockedVolume(HANDLE, std::wstring, std::uint32_t) noexcept;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::wstring identity_;
    std::uint32_t physical_drive_number_{};
};

struct VolumeLockAttempt final {
    bool busy{};
    std::optional<LockedVolume> locked;
};

Result<ProductVolume> provision_new_volume(const m3::M3AimRuntimeInfo&, wchar_t,
                                           std::chrono::milliseconds);
Result<ProductVolume> find_recovered_volume(const m3::M3AimRuntimeInfo&, wchar_t,
                                            std::chrono::milliseconds);
Result<void> flush_volume(const ProductVolume&);
[[nodiscard]] bool volume_lock_busy_error(DWORD) noexcept;
Result<VolumeLockAttempt> lock_volume(const ProductVolume&);
Result<void> assign_drive_letter(ProductVolume&);
Result<void> remove_drive_letter(const ProductVolume&);

} // namespace myrd
