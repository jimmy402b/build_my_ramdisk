#pragma once

#include "src/storage/block_device.h"

#include <cstdint>
#include <vector>

namespace myrd::test {

struct FakeBlockDevice final : myrd::IBlockDevice {
    std::uint64_t size = 64ull * 1024ull * 1024ull;
    std::uint32_t sector = 512;
    myrd::DeviceFeatures advertised{false, true, true};
    myrd::Result<void> next_result = myrd::Result<void>::success();
    std::vector<std::byte> bytes =
        std::vector<std::byte>(static_cast<std::size_t>(size));
    std::uint64_t read_calls = 0;
    std::uint64_t write_calls = 0;
    std::uint64_t unmap_calls = 0;
    std::uint64_t zero_calls = 0;
    std::vector<myrd::DataRange> last_ranges;
    std::uint64_t invalid_call_count = 0;

    std::uint64_t size_bytes() const noexcept override { return size; }
    std::uint32_t sector_size() const noexcept override { return sector; }
    myrd::DeviceFeatures features() const noexcept override {
        return advertised;
    }

    std::uint64_t total_calls() const noexcept {
        return read_calls + write_calls + unmap_calls + zero_calls;
    }

    Result<void> read(const OperationContext& /*context*/,
        std::uint64_t offset, std::span<std::byte> output) override {
        if (offset > bytes.size() ||
            output.size() > bytes.size() - static_cast<std::size_t>(offset)) {
            invalid_call_count++;
            return Result<void>::failure({ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"FakeBlockDevice::read", L"invalid bounds"});
        }
        read_calls++;
        if (next_result.has_error()) return Result<void>::failure(next_result.error());
        std::memcpy(output.data(), bytes.data() + offset, output.size());
        return Result<void>::success();
    }

    Result<void> write(const OperationContext& /*context*/,
        std::uint64_t offset, std::span<const std::byte> input) override {
        if (offset > bytes.size() ||
            input.size() > bytes.size() - static_cast<std::size_t>(offset)) {
            invalid_call_count++;
            return Result<void>::failure({ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"FakeBlockDevice::write", L"invalid bounds"});
        }
        write_calls++;
        if (next_result.has_error()) return Result<void>::failure(next_result.error());
        std::memcpy(bytes.data() + offset, input.data(), input.size());
        return Result<void>::success();
    }

    Result<void> unmap(const OperationContext& /*context*/,
        std::span<const DataRange> ranges) override {
        unmap_calls++;
        last_ranges.assign(ranges.begin(), ranges.end());
        if (next_result.has_error()) return Result<void>::failure(next_result.error());
        for (const auto& r : ranges) {
            std::fill_n(bytes.begin() + static_cast<std::size_t>(r.offset),
                        static_cast<std::size_t>(r.length), std::byte{0});
        }
        return Result<void>::success();
    }

    Result<void> zero(const OperationContext& /*context*/,
        std::span<const DataRange> ranges) override {
        zero_calls++;
        last_ranges.assign(ranges.begin(), ranges.end());
        if (next_result.has_error()) return Result<void>::failure(next_result.error());
        for (const auto& r : ranges) {
            std::fill_n(bytes.begin() + static_cast<std::size_t>(r.offset),
                        static_cast<std::size_t>(r.length), std::byte{0});
        }
        return Result<void>::success();
    }
};

} // namespace myrd::test
