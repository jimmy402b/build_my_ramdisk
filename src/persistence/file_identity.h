#pragma once

#include "src/common/result.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace myrd::persistence {

struct JournalFileIdentity final {
    std::array<std::byte, 16> journal_uuid{};

    bool operator==(const JournalFileIdentity&) const noexcept = default;
};

struct VhdxFileIdentity final {
    std::array<std::byte, 16> file_write_guid{};
    std::array<std::byte, 16> data_write_guid{};

    bool operator==(const VhdxFileIdentity&) const noexcept = default;
};

struct WindowsFileIdentity final {
    std::uint64_t volume_serial_number{};
    std::array<std::byte, 16> file_id{};
    std::uint64_t size{};
    std::uint32_t attributes{};
    std::uint32_t reparse_tag{};
    std::uint32_t hard_link_count{};

    bool operator==(const WindowsFileIdentity&) const noexcept = default;
};

[[nodiscard]] bool same_file_object(
    const WindowsFileIdentity& left,
    const WindowsFileIdentity& right) noexcept;

[[nodiscard]] Result<WindowsFileIdentity>
query_windows_file_identity(HANDLE handle);

} // namespace myrd::persistence
