#include "test_framework.h"

#include "src/proxy/imdproxy_protocol.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstring>

TEST_CASE(m1_protocol_codes_and_flags_match_pinned_aim) {
    REQUIRE_EQ(myrd::proxy::kReqInfo, 1u);
    REQUIRE_EQ(myrd::proxy::kReqRead, 2u);
    REQUIRE_EQ(myrd::proxy::kReqWrite, 3u);
    REQUIRE_EQ(myrd::proxy::kReqClose, 5u);
    REQUIRE_EQ(myrd::proxy::kReqUnmap, 6u);
    REQUIRE_EQ(myrd::proxy::kReqZero, 7u);
    REQUIRE_EQ(myrd::proxy::kFlagReadOnly, 0x01u);
    REQUIRE_EQ(myrd::proxy::kFlagSupportsUnmap, 0x02u);
    REQUIRE_EQ(myrd::proxy::kFlagSupportsZero, 0x04u);
}

TEST_CASE(m1_protocol_unmap_zero_layout_matches_pinned_aim) {
    REQUIRE_EQ(sizeof(myrd::proxy::UnmapRequest), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::UnmapResponse), 8u);
    REQUIRE_EQ(sizeof(myrd::proxy::ZeroRequest), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::ZeroResponse), 8u);
    REQUIRE_EQ(sizeof(myrd::proxy::DataSetRangeWire),
               sizeof(DEVICE_DATA_SET_RANGE));
    REQUIRE_EQ(offsetof(myrd::proxy::DataSetRangeWire, offset),
               offsetof(DEVICE_DATA_SET_RANGE, StartingOffset));
    REQUIRE_EQ(offsetof(myrd::proxy::DataSetRangeWire, length),
               offsetof(DEVICE_DATA_SET_RANGE, LengthInBytes));
}

TEST_CASE(m1_protocol_read_wire_rejects_truncated_unmap) {
    std::array<std::byte, 15> buf{};
    auto r = myrd::proxy::read_wire<myrd::proxy::UnmapRequest>(
        std::span<const std::byte>(buf), 0);
    REQUIRE(r.has_error());
}

TEST_CASE(m1_protocol_read_wire_accepts_full_unmap) {
    std::array<std::byte, 16> buf{};
    std::memset(buf.data(), 0, 16);
    auto r = myrd::proxy::read_wire<myrd::proxy::UnmapRequest>(
        std::span<const std::byte>(buf), 0);
    REQUIRE(r.ok());
}

TEST_CASE(m1_protocol_unmap_preserves_64bit_length) {
    std::array<std::byte, 16> buf{};
    myrd::proxy::UnmapRequest req{myrd::proxy::kReqUnmap, 0x123456789ABCDEF0ull};
    myrd::proxy::write_wire(std::span<std::byte>(buf), 0, req);
    auto r = myrd::proxy::read_wire<myrd::proxy::UnmapRequest>(
        std::span<const std::byte>(buf), 0);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().length, 0x123456789ABCDEF0ull);
}

TEST_CASE(m1_protocol_zero_response_write_does_not_touch_sentinel) {
    std::array<std::byte, 9> buf{};
    buf[8] = std::byte{0xAA};
    myrd::proxy::ZeroResponse resp{0};
    auto w = myrd::proxy::write_wire(std::span<std::byte>(buf), 0, resp);
    REQUIRE(w.ok());
    REQUIRE(buf[8] == std::byte{0xAA});
}
