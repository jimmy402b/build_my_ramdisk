#pragma once

#include "raw_round_trip.h"
#include "src/common/result.h"
#include "src/common/unique_handle.h"

#include <cstdint>
#include <string>

namespace myrd::poc {

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
