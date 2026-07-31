#pragma once

#include "block_device.h"

#include <cstdint>
#include <vector>

namespace myrd {

class MemoryBlockDevice final : public IBlockDevice {
public:
    MemoryBlockDevice(std::uint64_t size_bytes, std::uint32_t sector_size);
    std::uint64_t size_bytes() const noexcept override;
    std::uint32_t sector_size() const noexcept override;
    Result<std::size_t> read(
        std::uint64_t offset, std::span<std::byte> output) override;
    Result<std::size_t> write(
        std::uint64_t offset, std::span<const std::byte> input) override;

private:
    Result<void> validate(std::uint64_t offset, std::size_t length) const;
    std::vector<std::byte> bytes_;
    std::uint32_t sector_size_;
};

} // namespace myrd
