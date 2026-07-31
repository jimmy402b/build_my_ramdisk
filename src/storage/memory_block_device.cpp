#include "memory_block_device.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd {

MemoryBlockDevice::MemoryBlockDevice(
    std::uint64_t size_bytes, std::uint32_t sector_size)
    : bytes_(static_cast<std::size_t>(size_bytes)), sector_size_(sector_size) {
    if (size_bytes == 0) {
        throw std::invalid_argument("size_bytes must be non-zero");
    }
    if (sector_size == 0) {
        throw std::invalid_argument("sector_size must be non-zero");
    }
    if (size_bytes % sector_size != 0) {
        throw std::invalid_argument(
            "size_bytes must be divisible by sector_size");
    }
}

std::uint64_t MemoryBlockDevice::size_bytes() const noexcept {
    return bytes_.size();
}

std::uint32_t MemoryBlockDevice::sector_size() const noexcept {
    return sector_size_;
}

Result<void> MemoryBlockDevice::validate(
    std::uint64_t offset, std::size_t length) const {
    if (length == 0) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"zero-length I/O"});
    }
    if ((offset % sector_size_) != 0 || (length % sector_size_) != 0) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"unaligned I/O"});
    }
    if (offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"I/O outside device"});
    }
    return Result<void>::success();
}

Result<std::size_t> MemoryBlockDevice::read(
    std::uint64_t offset, std::span<std::byte> output) {
    auto v = validate(offset, output.size());
    if (!v) return Result<std::size_t>::failure(v.error());

    std::memcpy(output.data(), bytes_.data() + offset, output.size());
    return Result<std::size_t>::success(output.size());
}

Result<std::size_t> MemoryBlockDevice::write(
    std::uint64_t offset, std::span<const std::byte> input) {
    auto v = validate(offset, input.size());
    if (!v) return Result<std::size_t>::failure(v.error());

    std::memcpy(bytes_.data() + offset, input.data(), input.size());
    return Result<std::size_t>::success(input.size());
}

} // namespace myrd
