#include "test_framework.h"

#include "src/proxy/request_processor.h"
#include "src/storage/memory_block_device.h"

#include <cstring>
#include <vector>

constexpr std::uint64_t k64MiB = 64ull * 1024ull * 1024ull;

// Helper: create a mapping of the correct size filled with 0xA5
static std::vector<std::byte> make_mapping() {
    return std::vector<std::byte>(myrd::proxy::kMappingSize,
                                  std::byte{0xA5});
}

TEST_CASE(processor_rejects_mapping_smaller_than_protocol_header) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    std::vector<std::byte> small_map(100, std::byte{0});
    auto outcome = proc.process(std::span<std::byte>(small_map));
    REQUIRE(outcome.action == myrd::proxy::ProcessorAction::fatal);
    REQUIRE_EQ(proc.counters().fatal_protocol_errors, 1u);
}

TEST_CASE(processor_info_reports_64_mib) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto mapping = make_mapping();
    // Write INFO request code at offset 0
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping), 0, myrd::proxy::kReqInfo);
    REQUIRE(w.ok());
    auto outcome = proc.process(std::span<std::byte>(mapping));
    REQUIRE(outcome.action == myrd::proxy::ProcessorAction::respond);
    auto resp = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(mapping.data(),
                                   myrd::proxy::kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().file_size, k64MiB);
    REQUIRE_EQ(proc.counters().info, 1u);
}

TEST_CASE(processor_info_reports_512_alignment) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto mapping = make_mapping();
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping), 0, myrd::proxy::kReqInfo);
    REQUIRE(w.ok());
    proc.process(std::span<std::byte>(mapping));
    auto resp = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(mapping.data(),
                                   myrd::proxy::kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().req_alignment, 512u);
}

TEST_CASE(processor_info_advertises_zero_optional_flags) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto mapping = make_mapping();
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping), 0, myrd::proxy::kReqInfo);
    REQUIRE(w.ok());
    proc.process(std::span<std::byte>(mapping));
    auto resp = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(mapping.data(),
                                   myrd::proxy::kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().flags, 0u);
}

TEST_CASE(processor_info_increments_only_info_counter) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto mapping = make_mapping();
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping), 0, myrd::proxy::kReqInfo);
    proc.process(std::span<std::byte>(mapping));
    auto c = proc.counters();
    REQUIRE_EQ(c.info, 1u);
    REQUIRE_EQ(c.read, 0u);
    REQUIRE_EQ(c.write, 0u);
    REQUIRE_EQ(c.close, 0u);
    REQUIRE_EQ(c.fatal_protocol_errors, 0u);
}

TEST_CASE(processor_info_does_not_touch_data_area) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto mapping = make_mapping();
    // Capture original data area bytes
    std::vector<std::byte> original(
        mapping.begin() + myrd::proxy::kHeaderSize, mapping.end());
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping), 0, myrd::proxy::kReqInfo);
    proc.process(std::span<std::byte>(mapping));
    // Verify data area unchanged
    for (std::size_t i = myrd::proxy::kHeaderSize; i < mapping.size(); ++i) {
        REQUIRE(mapping[i] == original[i - myrd::proxy::kHeaderSize]);
    }
}
