/// @file m3_checksum_codec_tests.cpp
/// @brief Task 10 — CRC32C, streaming SHA-256 and byte codec golden-vector tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "src/common/crc32c.h"
#include "src/common/sha256.h"
#include "src/common/sha256_stream.h"
#include "src/persistence/byte_codec.h"
#include "test_framework.h"

using namespace myrd;
using namespace myrd::persistence;

namespace {

std::vector<std::byte> bytes(std::string_view s) {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        v[i] = static_cast<std::byte>(s[i]);
    return v;
}

}  // namespace

// ---- CRC32C golden vectors -------------------------------------------------

TEST_CASE(m3_crc32c_empty_returns_zero) {
    std::vector<std::byte> empty;
    REQUIRE_EQ(crc32c(empty), 0U);
}

TEST_CASE(m3_crc32c_standard_vector_123456789) {
    auto data = bytes("123456789");
    REQUIRE_EQ(crc32c(data), 0xE3069283U);
}

TEST_CASE(m3_crc32c_streaming_matches_one_shot) {
    auto data = bytes("123456789");
    Crc32cStream stream;
    stream.update(std::span(data).subspan(0, 4));
    stream.update(std::span(data).subspan(4));
    REQUIRE_EQ(stream.finalize(), crc32c(data));
}

TEST_CASE(m3_crc32c_streaming_byte_at_a_time) {
    auto data = bytes("123456789");
    Crc32cStream stream;
    for (auto b : data) {
        stream.update(std::span(&b, 1));
    }
    REQUIRE_EQ(stream.finalize(), crc32c(data));
}

TEST_CASE(m3_crc32c_all_zeroes) {
    std::vector<std::byte> z(256);
    auto result = crc32c(z);
    REQUIRE(result != 0U);
}

// ---- Streaming SHA-256 golden vectors --------------------------------------

TEST_CASE(m3_sha256_stream_empty_hash_is_known) {
    Sha256Stream stream;
    auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(),
        "E3B0C44298FC1C149AFBF4C8996FB924"
        "27AE41E4649B934CA495991B7852B855");
}

TEST_CASE(m3_sha256_stream_matches_one_shot_split) {
    auto data = bytes("The quick brown fox jumps over the lazy dog");
    auto one_shot = sha256_hex(data);
    REQUIRE(one_shot.ok());

    Sha256Stream stream;
    auto mid = data.size() / 2;
    REQUIRE(stream.update(std::span(data).subspan(0, mid)).ok());
    REQUIRE(stream.update(std::span(data).subspan(mid)).ok());
    auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), one_shot.value());
}

TEST_CASE(m3_sha256_stream_byte_by_byte_matches_one_shot) {
    auto data = bytes("abc");
    auto one_shot = sha256_hex(data);
    REQUIRE(one_shot.ok());

    Sha256Stream stream;
    for (auto b : data) {
        REQUIRE(stream.update(std::span(&b, 1)).ok());
    }
    auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), one_shot.value());
}

// ---- Byte codec round-trip -------------------------------------------------

TEST_CASE(m3_byte_codec_u16_round_trip) {
    std::array<std::byte, 2> buf{};
    std::size_t off = 0;
    REQUIRE(write_u16_le(buf, off, 0xABCD));
    off = 0;
    std::uint16_t val = 0;
    REQUIRE(read_u16_le(buf, off, val));
    REQUIRE_EQ(val, 0xABCDU);
}

TEST_CASE(m3_byte_codec_u32_round_trip) {
    std::array<std::byte, 4> buf{};
    std::size_t off = 0;
    REQUIRE(write_u32_le(buf, off, 0xDEADBEEFU));
    off = 0;
    std::uint32_t val = 0;
    REQUIRE(read_u32_le(buf, off, val));
    REQUIRE_EQ(val, 0xDEADBEEFU);
}

TEST_CASE(m3_byte_codec_u64_round_trip) {
    std::array<std::byte, 8> buf{};
    std::size_t off = 0;
    REQUIRE(write_u64_le(buf, off, 0x0123456789ABCDEFULL));
    off = 0;
    std::uint64_t val = 0;
    REQUIRE(read_u64_le(buf, off, val));
    REQUIRE_EQ(val, 0x0123456789ABCDEFULL);
}

TEST_CASE(m3_byte_codec_read_overflow_fails) {
    std::array<std::byte, 3> buf{};
    std::size_t off = 0;
    std::uint32_t val = 0;
    REQUIRE(!read_u32_le(buf, off, val));
}

TEST_CASE(m3_byte_codec_write_overflow_fails) {
    std::array<std::byte, 1> buf{};
    std::size_t off = 0;
    REQUIRE(!write_u16_le(buf, off, 1));
}

TEST_CASE(m3_byte_codec_checked_advance) {
    std::size_t off = 100;
    REQUIRE(!checked_advance(off, 1, 100));
    off = 99;
    REQUIRE(checked_advance(off, 1, 100));
    REQUIRE_EQ(off, 100U);
}

TEST_CASE(m3_byte_codec_write_zeros_and_read_bytes) {
    std::array<std::byte, 8> buf{};
    std::size_t off = 0;
    REQUIRE(write_zeros(buf, off, 4));
    REQUIRE(write_u32_le(buf, off, 42));
    off = 0;
    std::array<std::byte, 4> zb{};
    REQUIRE(read_bytes(std::as_bytes(std::span(buf)), off, zb));
    for (auto b : zb) REQUIRE(static_cast<int>(b) == 0);
    std::uint32_t v = 0;
    REQUIRE(read_u32_le(std::as_bytes(std::span(buf)), off, v));
    REQUIRE_EQ(v, 42U);
}
