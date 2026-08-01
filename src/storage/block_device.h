#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <span>

namespace myrd {

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual Result<std::size_t> read(
        std::uint64_t offset, std::span<std::byte> output) = 0;
    virtual Result<std::size_t> write(
        std::uint64_t offset, std::span<const std::byte> input) = 0;
};

} // namespace myrd
