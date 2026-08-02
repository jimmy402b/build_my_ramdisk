#include "test_framework.h"

#include "src/proxy/range_normalizer.h"
#include "src/proxy/imdproxy_protocol.h"

#include <array>
#include <cstring>

using namespace myrd;
using namespace myrd::proxy;

namespace {

std::span<const std::byte> as_bytes_span(const void* data, std::size_t size) {
    return std::span<const std::byte>(
        static_cast<const std::byte*>(data), size);
}

} // anonymous namespace

TEST_CASE(m1_range_normalizer_empty_data) {
    std::array<std::byte, 0> buf{};
    auto r = decode_and_normalize_ranges(buf, 0, 64 * 1024, 512);
    REQUIRE(r.ok());
    REQUIRE(r.value().empty());
}

TEST_CASE(m1_range_normalizer_rejects_non_multiple_of_16) {
    std::array<std::byte, 32> buf{};
    std::memset(buf.data(), 0, buf.size());
    auto r = decode_and_normalize_ranges(buf, 17, 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_rejects_encoded_length_beyond_data) {
    std::array<std::byte, 16> buf{};
    auto r = decode_and_normalize_ranges(buf, 32, 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_ignores_zero_length_entry) {
    const DataSetRangeWire wire = {4096, 0};
    auto bytes = as_bytes_span(&wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.ok());
    REQUIRE(r.value().empty());
}

TEST_CASE(m1_range_normalizer_rejects_misaligned_offset) {
    const DataSetRangeWire wire = {1, 512};
    auto bytes = as_bytes_span(&wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_rejects_misaligned_length) {
    const DataSetRangeWire wire = {0, 1};
    auto bytes = as_bytes_span(&wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_rejects_offset_overflow) {
    const DataSetRangeWire wire = {0xFFFFFFFFFFFFF000ull, 4096};
    auto bytes = as_bytes_span(&wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_rejects_device_end_overflow) {
    const DataSetRangeWire wire = {64 * 1024 - 512, 1024};
    auto bytes = as_bytes_span(&wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_range_normalizer_sorts_and_merges_overlap) {
    const DataSetRangeWire wire[] = {
        {4096, 4096}, {0, 4096}, {2048, 4096}, {16384, 0}
    };
    auto bytes = as_bytes_span(wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().size(), 1u);
    REQUIRE_EQ(r.value()[0].offset, 0u);
    REQUIRE_EQ(r.value()[0].length, 8192u);
}

TEST_CASE(m1_range_normalizer_merges_adjacent) {
    const DataSetRangeWire wire[] = {{0, 4096}, {4096, 4096}};
    auto bytes = as_bytes_span(wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().size(), 1u);
    REQUIRE_EQ(r.value()[0].offset, 0u);
    REQUIRE_EQ(r.value()[0].length, 8192u);
}

TEST_CASE(m1_range_normalizer_keeps_separated) {
    const DataSetRangeWire wire[] = {{0, 4096}, {16384, 4096}};
    auto bytes = as_bytes_span(wire, sizeof(wire));
    auto r = decode_and_normalize_ranges(bytes, sizeof(wire), 64 * 1024, 512);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().size(), 2u);
}
