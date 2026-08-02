#include "test_framework.h"

#include "src/storage/memory_block_device.h"

#include <array>
#include <chrono>
#include <cstring>

TEST_CASE(m1_contract_device_reports_features) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    const auto features = device.features();
    REQUIRE(!features.read_only);
    REQUIRE(features.supports_unmap);
    REQUIRE(features.supports_zero);
}

TEST_CASE(m1_contract_read_write_one_sector) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    myrd::OperationContext context{};
    std::array<std::byte, 512> source{};
    source.fill(std::byte{0x5A});
    REQUIRE(device.write(context, 0, source).ok());

    std::array<std::byte, 512> destination{};
    REQUIRE(device.read(context, 0, destination).ok());
    REQUIRE(destination == source);
}

TEST_CASE(m1_contract_zero_one_sector) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    myrd::OperationContext context{};
    std::array<std::byte, 512> source{};
    source.fill(std::byte{0x5A});
    REQUIRE(device.write(context, 0, source).ok());

    const myrd::DataRange range{0, 512};
    REQUIRE(device.zero(context, std::span<const myrd::DataRange>(&range, 1)).ok());
    std::array<std::byte, 512> destination{};
    REQUIRE(device.read(context, 0, destination).ok());
    REQUIRE(destination[0] == std::byte{0});
}

TEST_CASE(m1_contract_unmap_one_sector) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    myrd::OperationContext context{};
    std::array<std::byte, 512> source{};
    source.fill(std::byte{0x5A});
    REQUIRE(device.write(context, 0, source).ok());

    const myrd::DataRange range{0, 512};
    REQUIRE(device.unmap(context, std::span<const myrd::DataRange>(&range, 1)).ok());
    std::array<std::byte, 512> destination{};
    REQUIRE(device.read(context, 0, destination).ok());
    REQUIRE(destination[0] == std::byte{0});
}

TEST_CASE(m1_contract_rejects_misalignment) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    myrd::OperationContext context{};
    std::array<std::byte, 512> buf{};
    auto result = device.read(context, 1, buf);
    REQUIRE(result.has_error());
}

TEST_CASE(m1_contract_rejects_overflow) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    myrd::OperationContext context{};
    std::array<std::byte, 512> buf{};
    auto result = device.read(context, 1024 * 1024 - 256, buf);
    REQUIRE(result.has_error());
}

TEST_CASE(m1_contract_rejects_read_only_write) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, true);
    myrd::OperationContext context{};
    std::array<std::byte, 512> buf{};
    buf.fill(std::byte{0x5A});
    auto result = device.write(context, 0, buf);
    REQUIRE(result.has_error());
}

TEST_CASE(m1_contract_rejects_cancelled_operation) {
    myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
    std::stop_source source;
    source.request_stop();
    myrd::OperationContext context{source.get_token()};
    std::array<std::byte, 512> buf{};
    auto result = device.read(context, 0, buf);
    REQUIRE(result.has_error());
}
