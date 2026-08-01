#pragma once

#include "raw_round_trip.h"
#include "src/common/result.h"
#include "src/common/unique_handle.h"

#include <cstdint>
#include <optional>
#include <string>

namespace myrd::poc {

std::uint32_t raw_identity_open_access() noexcept;

struct DiskAttributeUpdate {
    std::uint64_t attributes;
    std::uint64_t attributes_mask;
    bool persist;
};

std::optional<DiskAttributeUpdate> writable_disk_attribute_update(
    std::uint64_t disk_attributes) noexcept;

Result<void> validate_disk_write_preflight(
    std::uint64_t disk_attributes,
    bool disk_stack_reports_writable,
    std::uint32_t disk_stack_error);

class WindowsRawDiskIo final : public IRawDiskIo {
public:
    WindowsRawDiskIo() noexcept = default;
    Result<void> verify_identity(const aim::LocatedDisk& disk) override;
    Result<void> seek(std::uint64_t offset) override;
    Result<void> write_exact(std::span<const std::byte> data) override;
    Result<void> flush() override;
    Result<void> read_exact(std::span<std::byte> data) override;

private:
    UniqueHandle rw_handle_;
};

} // namespace myrd::poc
