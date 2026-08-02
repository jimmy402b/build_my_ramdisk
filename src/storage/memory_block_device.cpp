#include "memory_block_device.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd {

MemoryBlockDevice::MemoryBlockDevice(
    std::uint64_t size_bytes, std::uint32_t sector_size, bool read_only)
    : bytes_(static_cast<std::size_t>(size_bytes)), sector_size_(sector_size),
      read_only_(read_only) {
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

DeviceFeatures MemoryBlockDevice::features() const noexcept {
    return DeviceFeatures{read_only_, true, true};
}

Result<void> MemoryBlockDevice::validate(
    std::uint64_t offset, std::size_t length) const {
    if (length == 0) {
        return Result<void>::failure({ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"MemoryBlockDevice::validate", L"zero-length I/O"});
    }
    if ((offset % sector_size_) != 0 || (length % sector_size_) != 0) {
        return Result<void>::failure({ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::misaligned),
            L"MemoryBlockDevice::validate", L"unaligned I/O"});
    }
    if (offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
        return Result<void>::failure({ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"MemoryBlockDevice::validate", L"I/O outside device"});
    }
    return Result<void>::success();
}

Result<void> MemoryBlockDevice::read(
    const OperationContext& context,
    std::uint64_t offset, std::span<std::byte> output) {
    auto live = check_operation_context(context);
    if (!live) return live;
    auto v = validate(offset, output.size());
    if (!v) return v;

    std::scoped_lock lock(mutex_);
    std::memcpy(output.data(), bytes_.data() + offset, output.size());
    return Result<void>::success();
}

Result<void> MemoryBlockDevice::write(
    const OperationContext& context,
    std::uint64_t offset, std::span<const std::byte> input) {
    auto live = check_operation_context(context);
    if (!live) return live;
    auto v = validate(offset, input.size());
    if (!v) return v;
    if (read_only_) {
        return Result<void>::failure({ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::read_only),
            L"MemoryBlockDevice::write", L"device is read-only"});
    }

    std::scoped_lock lock(mutex_);
    std::memcpy(bytes_.data() + offset, input.data(), input.size());
    return Result<void>::success();
}

Result<void> MemoryBlockDevice::apply_zero_ranges(
    const OperationContext& context, std::span<const DataRange> ranges) {
    auto live = check_operation_context(context);
    if (!live) return live;
    for (const auto& range : ranges) {
        auto valid = validate(range.offset, static_cast<std::size_t>(range.length));
        if (!valid) return valid;
    }
    if (read_only_) {
        return Result<void>::failure({ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::read_only),
            L"MemoryBlockDevice::apply_zero_ranges", L"device is read-only"});
    }
    std::scoped_lock lock(mutex_);
    for (const auto& range : ranges) {
        std::fill_n(bytes_.begin() + static_cast<std::size_t>(range.offset),
                    static_cast<std::size_t>(range.length), std::byte{0});
    }
    return Result<void>::success();
}

Result<void> MemoryBlockDevice::unmap(
    const OperationContext& context,
    std::span<const DataRange> ranges) {
    return apply_zero_ranges(context, ranges);
}

Result<void> MemoryBlockDevice::zero(
    const OperationContext& context,
    std::span<const DataRange> ranges) {
    return apply_zero_ranges(context, ranges);
}

} // namespace myrd
