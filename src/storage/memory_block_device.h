#pragma once

#include "block_device.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace myrd {

class MemoryBlockDevice final : public IBlockDevice {
public:
    MemoryBlockDevice(std::uint64_t size_bytes, std::uint32_t sector_size,
                      bool read_only = false);
    std::uint64_t size_bytes() const noexcept override;
    std::uint32_t sector_size() const noexcept override;
    DeviceFeatures features() const noexcept override;
    Result<void> read(const OperationContext& context,
        std::uint64_t offset, std::span<std::byte> output) override;
    Result<void> write(const OperationContext& context,
        std::uint64_t offset, std::span<const std::byte> input) override;
    Result<void> unmap(const OperationContext& context,
        std::span<const DataRange> ranges) override;
    Result<void> zero(const OperationContext& context,
        std::span<const DataRange> ranges) override;

private:
    Result<void> validate(std::uint64_t offset, std::size_t length) const;
    Result<void> apply_zero_ranges(const OperationContext& context,
                                   std::span<const DataRange> ranges);
    std::vector<std::byte> bytes_;
    std::uint32_t sector_size_;
    bool read_only_;
    std::mutex mutex_;
};

} // namespace myrd
