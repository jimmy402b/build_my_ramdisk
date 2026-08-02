#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <cstdint>
#include <span>

namespace myrd {

struct DataRange {
    std::uint64_t offset;
    std::uint64_t length;
};

struct DeviceFeatures {
    bool read_only;
    bool supports_unmap;
    bool supports_zero;
};

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual DeviceFeatures features() const noexcept = 0;
    virtual Result<void> read(const OperationContext& context,
        std::uint64_t offset, std::span<std::byte> output) = 0;
    virtual Result<void> write(const OperationContext& context,
        std::uint64_t offset, std::span<const std::byte> input) = 0;
    virtual Result<void> unmap(const OperationContext& context,
        std::span<const DataRange> ranges) = 0;
    virtual Result<void> zero(const OperationContext& context,
        std::span<const DataRange> ranges) = 0;
};

} // namespace myrd
