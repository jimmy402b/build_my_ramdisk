/// @file m3_checksum_codec_tests.cpp
/// @brief Task 10 — CRC32C, streaming SHA-256 and byte codec golden-vector tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

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

std::uint64_t process_private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    REQUIRE(GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)) != FALSE);
    return static_cast<std::uint64_t>(counters.PrivateUsage);
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
    REQUIRE(stream.update(std::span(data).subspan(0, 4)).ok());
    REQUIRE(stream.update(std::span(data).subspan(4)).ok());
    const auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), crc32c(data));
}

TEST_CASE(m3_crc32c_streaming_byte_at_a_time) {
    auto data = bytes("123456789");
    Crc32cStream stream;
    for (auto b : data) {
        REQUIRE(stream.update(std::span(&b, 1)).ok());
    }
    const auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), crc32c(data));
}

TEST_CASE(m3_crc32c_all_zeroes) {
    std::vector<std::byte> z(256);
    auto result = crc32c(z);
    REQUIRE(result != 0U);
}

TEST_CASE(m3_crc32c_stream_unaligned_large_split_matches_one_shot) {
    std::vector<std::byte> data((1U << 20U) + 3U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>((i * 131U + 17U) & 0xFFU);
    }
    const auto unaligned = std::span<const std::byte>(data).subspan(1);
    Crc32cStream stream;
    REQUIRE(stream.update(unaligned.subspan(0, 257)).ok());
    REQUIRE(stream.update(unaligned.subspan(257, 65537)).ok());
    REQUIRE(stream.update(unaligned.subspan(65794)).ok());
    const auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), crc32c(unaligned));
}

TEST_CASE(m3_crc32c_stream_finalize_is_single_use_with_one_stable_error) {
    Crc32cStream stream;
    const auto data = bytes("lifecycle");
    REQUIRE(stream.update(data).ok());
    REQUIRE(stream.finalize().ok());
    const auto second_finalize = stream.finalize();
    const auto update_after_finalize = stream.update(data);
    REQUIRE(!second_finalize.ok());
    REQUIRE(!update_after_finalize.ok());
    REQUIRE_EQ(second_finalize.error().domain, ErrorDomain::internal);
    REQUIRE_EQ(update_after_finalize.error().domain, ErrorDomain::internal);
    REQUIRE_EQ(second_finalize.error().code, update_after_finalize.error().code);
    REQUIRE_EQ(second_finalize.error().code,
               static_cast<std::uint32_t>(
                   Crc32cErrorCode::already_finalized));
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

TEST_CASE(m3_sha256_stream_abc_matches_independent_standard_vector) {
    Sha256Stream stream;
    const auto data = bytes("abc");
    REQUIRE(stream.update(data).ok());
    const auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(),
        "BA7816BF8F01CFEA414140DE5DAE2223"
        "B00361A396177A9CB410FF61F20015AD");
}

TEST_CASE(m3_sha256_stream_unaligned_large_split_matches_one_shot) {
    std::vector<std::byte> data((1U << 20U) + 3U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>((i * 197U + 23U) & 0xFFU);
    }
    const auto unaligned = std::span<const std::byte>(data).subspan(1);
    const auto one_shot = sha256_hex(unaligned);
    REQUIRE(one_shot.ok());

    Sha256Stream stream;
    REQUIRE(stream.update(unaligned.subspan(0, 257)).ok());
    REQUIRE(stream.update(unaligned.subspan(257, 65537)).ok());
    REQUIRE(stream.update(unaligned.subspan(65794)).ok());
    const auto result = stream.finalize();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value(), one_shot.value());
}

TEST_CASE(m3_sha256_stream_memory_is_bounded_by_chunk_not_total_input) {
    std::vector<std::byte> chunk(1U << 20U, std::byte{0x5A});
    Sha256Stream stream;
    const auto before = process_private_bytes();
    for (std::size_t i = 0; i < 256; ++i) {
        REQUIRE(stream.update(chunk).ok());
    }
    const auto after = process_private_bytes();
    REQUIRE(after <= before + (32ULL << 20U));
    REQUIRE(stream.finalize().ok());
}

// ---- Byte codec round-trip -------------------------------------------------

TEST_CASE(m3_byte_codec_emits_independent_exact_little_endian_golden_bytes) {
    std::array<std::byte, 14> encoded{};
    std::size_t offset = 0;
    REQUIRE_EQ(write_u16_le(encoded, offset, 0x1234U), ByteCodecError::none);
    REQUIRE_EQ(write_u32_le(encoded, offset, 0x89ABCDEFU), ByteCodecError::none);
    REQUIRE_EQ(write_u64_le(encoded, offset, 0x0123456789ABCDEFULL),
               ByteCodecError::none);

    constexpr std::array<std::byte, 14> golden{
        std::byte{0x34}, std::byte{0x12},
        std::byte{0xEF}, std::byte{0xCD}, std::byte{0xAB}, std::byte{0x89},
        std::byte{0xEF}, std::byte{0xCD}, std::byte{0xAB}, std::byte{0x89},
        std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x01}};
    REQUIRE_EQ(encoded, golden);

    offset = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    REQUIRE_EQ(read_u16_le(golden, offset, u16), ByteCodecError::none);
    REQUIRE_EQ(read_u32_le(golden, offset, u32), ByteCodecError::none);
    REQUIRE_EQ(read_u64_le(golden, offset, u64), ByteCodecError::none);
    REQUIRE_EQ(u16, 0x1234U);
    REQUIRE_EQ(u32, 0x89ABCDEFU);
    REQUIRE_EQ(u64, 0x0123456789ABCDEFULL);
}

TEST_CASE(m3_byte_codec_size_max_offset_never_wraps_or_advances) {
    const std::array<std::byte, 1> input{std::byte{0xA5}};
    std::size_t offset = std::numeric_limits<std::size_t>::max();

    REQUIRE_EQ(skip_bytes(input, offset, 1), ByteCodecError::out_of_bounds);
    REQUIRE_EQ(offset, std::numeric_limits<std::size_t>::max());

    std::uint16_t value = 0xBEEFU;
    REQUIRE_EQ(read_u16_le(input, offset, value),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(offset, std::numeric_limits<std::size_t>::max());
    REQUIRE_EQ(value, 0xBEEFU);

    REQUIRE_EQ(checked_advance(offset, 1,
                               std::numeric_limits<std::size_t>::max()),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(offset, std::numeric_limits<std::size_t>::max());
}

TEST_CASE(m3_byte_codec_failure_preserves_offset_values_and_destinations) {
    const std::array<std::byte, 2> input{std::byte{0x10}, std::byte{0x20}};
    std::array<std::byte, 4> destination{
        std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}, std::byte{0xA4}};
    const auto original_destination = destination;
    std::size_t read_offset = 1;
    std::uint32_t value = 0xDEADBEEFU;
    REQUIRE_EQ(read_u32_le(input, read_offset, value),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(read_offset, 1U);
    REQUIRE_EQ(value, 0xDEADBEEFU);
    REQUIRE_EQ(read_bytes(input, read_offset, destination),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(read_offset, 1U);
    REQUIRE_EQ(destination, original_destination);
    std::uint64_t wide_value = 0x0123456789ABCDEFULL;
    REQUIRE_EQ(read_u64_le(input, read_offset, wide_value),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(read_offset, 1U);
    REQUIRE_EQ(wide_value, 0x0123456789ABCDEFULL);
    UuidBytes decoded_uuid{};
    decoded_uuid.fill(std::byte{0xCC});
    const auto original_uuid = decoded_uuid;
    REQUIRE_EQ(read_uuid(input, read_offset, decoded_uuid),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(read_offset, 1U);
    REQUIRE_EQ(decoded_uuid, original_uuid);

    std::array<std::byte, 2> output{std::byte{0xB1}, std::byte{0xB2}};
    const auto original_output = output;
    std::size_t write_offset = 1;
    REQUIRE_EQ(write_bytes(output, write_offset, input),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(write_offset, 1U);
    REQUIRE_EQ(output, original_output);
    REQUIRE_EQ(write_u16_le(output, write_offset, 0x1234U),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(write_offset, 1U);
    REQUIRE_EQ(output, original_output);
    REQUIRE_EQ(write_zeros(output, write_offset, 2),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(write_offset, 1U);
    REQUIRE_EQ(output, original_output);
    REQUIRE_EQ(write_u64_le(output, write_offset,
                            0x0123456789ABCDEFULL),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(write_offset, 1U);
    REQUIRE_EQ(output, original_output);
    UuidBytes uuid{};
    uuid.fill(std::byte{0xDD});
    REQUIRE_EQ(write_uuid(output, write_offset, uuid),
               ByteCodecError::out_of_bounds);
    REQUIRE_EQ(write_offset, 1U);
    REQUIRE_EQ(output, original_output);
}

TEST_CASE(m3_byte_codec_uuid_has_exact_sixteen_wire_bytes) {
    const UuidBytes uuid{
        std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
        std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
        std::byte{0x88}, std::byte{0x99}, std::byte{0xAA}, std::byte{0xBB},
        std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE}, std::byte{0xFF}};
    std::array<std::byte, 16> buffer{};
    std::size_t offset = 0;
    REQUIRE_EQ(write_uuid(buffer, offset, uuid), ByteCodecError::none);
    REQUIRE_EQ(buffer, uuid);
    offset = 0;
    UuidBytes decoded{};
    REQUIRE_EQ(read_uuid(buffer, offset, decoded), ByteCodecError::none);
    REQUIRE_EQ(decoded, uuid);
}

TEST_CASE(m3_byte_codec_bounded_utf8_and_reserved_zero_are_fail_closed) {
    const std::array<std::byte, 7> valid{
        std::byte{'r'}, std::byte{'a'}, std::byte{'m'}, std::byte{'-'},
        std::byte{0xE4}, std::byte{0xB8}, std::byte{0xAD}};
    REQUIRE_EQ(validate_bounded_utf8(valid, 32), ByteCodecError::none);
    REQUIRE_EQ(validate_bounded_utf8(valid, valid.size() - 1),
               ByteCodecError::length_exceeds_limit);
    const std::array<std::byte, 3> malformed{
        std::byte{0xE2}, std::byte{0x28}, std::byte{0xA1}};
    REQUIRE_EQ(validate_bounded_utf8(malformed, 32), ByteCodecError::invalid_utf8);
    const std::array<std::byte, 2> overlong{
        std::byte{0xC0}, std::byte{0x80}};
    REQUIRE_EQ(validate_bounded_utf8(overlong, 32), ByteCodecError::invalid_utf8);
    const std::array<std::byte, 3> surrogate{
        std::byte{0xED}, std::byte{0xA0}, std::byte{0x80}};
    REQUIRE_EQ(validate_bounded_utf8(surrogate, 32), ByteCodecError::invalid_utf8);
    const std::array<std::byte, 4> beyond_unicode{
        std::byte{0xF4}, std::byte{0x90}, std::byte{0x80}, std::byte{0x80}};
    REQUIRE_EQ(validate_bounded_utf8(beyond_unicode, 32),
               ByteCodecError::invalid_utf8);
    const std::array<std::byte, 2> truncated{
        std::byte{0xE4}, std::byte{0xB8}};
    REQUIRE_EQ(validate_bounded_utf8(truncated, 32), ByteCodecError::invalid_utf8);

    const std::array<std::byte, 4> zeros{};
    REQUIRE_EQ(require_zero(zeros), ByteCodecError::none);
    const std::array<std::byte, 4> nonzero{
        std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}};
    REQUIRE_EQ(require_zero(nonzero), ByteCodecError::nonzero_reserved);
}

TEST_CASE(m3_byte_codec_checked_alignment_is_overflow_safe) {
    std::size_t aligned = 0xCCU;
    REQUIRE_EQ(checked_align_up(17, 8, aligned), ByteCodecError::none);
    REQUIRE_EQ(aligned, 24U);
    REQUIRE_EQ(checked_align_up(17, 0, aligned), ByteCodecError::invalid_alignment);
    REQUIRE_EQ(aligned, 24U);
    aligned = 0xCCU;
    REQUIRE_EQ(checked_align_up(std::numeric_limits<std::size_t>::max(), 8,
                                aligned),
               ByteCodecError::arithmetic_overflow);
    REQUIRE_EQ(aligned, 0xCCU);
}

TEST_CASE(m3_sha256_stream_finalize_is_single_use_with_one_stable_error) {
    Sha256Stream stream;
    const auto data = bytes("lifecycle");
    REQUIRE(stream.update(data).ok());

    const auto digest = stream.finalize();
    REQUIRE(digest.ok());

    const auto second_finalize = stream.finalize();
    REQUIRE(!second_finalize.ok());
    const auto update_after_finalize = stream.update(data);
    REQUIRE(!update_after_finalize.ok());
    REQUIRE_EQ(second_finalize.error().domain, ErrorDomain::internal);
    REQUIRE_EQ(update_after_finalize.error().domain, ErrorDomain::internal);
    REQUIRE_EQ(second_finalize.error().code, update_after_finalize.error().code);
    REQUIRE_EQ(second_finalize.error().code,
               static_cast<std::uint32_t>(
                   Sha256StreamErrorCode::already_finalized));
}

TEST_CASE(m3_sha256_stream_chunk_failure_is_permanent_and_fail_closed) {
    detail::Sha256StreamState stream;
    std::size_t calls = 0;
    const auto data = bytes("eight888");

    const auto first_failure = stream.update(
        data, 3, [&calls](std::span<const std::byte>) {
            if (calls++ == 1) {
                return Result<void>::failure({
                    ErrorDomain::ntstatus,
                    static_cast<std::uint32_t>(
                        Sha256StreamErrorCode::hash_update_failed),
                    L"test", L"injected update failure", 0xC0000001U});
            }
            return Result<void>::success();
        });
    REQUIRE(!first_failure.ok());
    REQUIRE_EQ(first_failure.error().code,
               static_cast<std::uint32_t>(
                   Sha256StreamErrorCode::hash_update_failed));
    REQUIRE_EQ(stream.chunk_calls(), 2U);

    const auto later_update = stream.update(
        bytes("ignored"), 3, [](std::span<const std::byte>) {
            return Result<void>::success();
        });
    const auto later_finalize = stream.finalize([] {
        return Result<std::string>::success("must-not-be-used");
    });
    REQUIRE(!later_update.ok());
    REQUIRE(!later_finalize.ok());
    REQUIRE_EQ(later_update.error().domain, first_failure.error().domain);
    REQUIRE_EQ(later_update.error().code, first_failure.error().code);
    REQUIRE_EQ(later_update.error().native_code,
               first_failure.error().native_code);
    REQUIRE_EQ(later_finalize.error().domain, first_failure.error().domain);
    REQUIRE_EQ(later_finalize.error().code, first_failure.error().code);
    REQUIRE_EQ(later_finalize.error().native_code,
               first_failure.error().native_code);
}

TEST_CASE(m3_sha256_stream_test_chunk_limit_exercises_multi_chunk_digest) {
    const auto data = bytes("abcdefgh");
    detail::Sha256StreamState stream;
    std::vector<std::byte> collected;
    REQUIRE(stream.update(
        data, 3, [&collected](std::span<const std::byte> chunk) {
            collected.insert(collected.end(), chunk.begin(), chunk.end());
            return Result<void>::success();
        }).ok());
    REQUIRE_EQ(stream.chunk_calls(), 3U);
    const auto digest = stream.finalize([&collected] {
        return sha256_hex(collected);
    });
    const auto oracle = sha256_hex(data);
    REQUIRE(digest.ok());
    REQUIRE(oracle.ok());
    REQUIRE_EQ(digest.value(), oracle.value());
}

TEST_CASE(m3_sha256_stream_finish_failure_is_permanent_and_fail_closed) {
    detail::Sha256StreamState stream;
    const auto data = bytes("finish-failure");
    REQUIRE(stream.update(
        data, 3, [](std::span<const std::byte>) {
            return Result<void>::success();
        }).ok());
    const auto first_failure = stream.finalize([] {
        return Result<std::string>::failure({
            ErrorDomain::ntstatus,
            static_cast<std::uint32_t>(
                Sha256StreamErrorCode::hash_finalize_failed),
            L"test", L"injected finish failure", 0xC0000002U});
    });
    REQUIRE(!first_failure.ok());

    const auto later_update = stream.update(
        data, 3, [](std::span<const std::byte>) {
            return Result<void>::success();
        });
    const auto later_finalize = stream.finalize([] {
        return Result<std::string>::success("must-not-be-used");
    });
    REQUIRE(!later_update.ok());
    REQUIRE(!later_finalize.ok());
    REQUIRE_EQ(later_update.error().code, first_failure.error().code);
    REQUIRE_EQ(later_update.error().native_code,
               first_failure.error().native_code);
    REQUIRE_EQ(later_finalize.error().code, first_failure.error().code);
    REQUIRE_EQ(later_finalize.error().native_code,
               first_failure.error().native_code);
}

TEST_CASE(m3_byte_codec_u16_round_trip) {
    std::array<std::byte, 2> buf{};
    std::size_t off = 0;
    REQUIRE_EQ(write_u16_le(buf, off, 0xABCD), ByteCodecError::none);
    off = 0;
    std::uint16_t val = 0;
    REQUIRE_EQ(read_u16_le(buf, off, val), ByteCodecError::none);
    REQUIRE_EQ(val, 0xABCDU);
}

TEST_CASE(m3_byte_codec_u32_round_trip) {
    std::array<std::byte, 4> buf{};
    std::size_t off = 0;
    REQUIRE_EQ(write_u32_le(buf, off, 0xDEADBEEFU), ByteCodecError::none);
    off = 0;
    std::uint32_t val = 0;
    REQUIRE_EQ(read_u32_le(buf, off, val), ByteCodecError::none);
    REQUIRE_EQ(val, 0xDEADBEEFU);
}

TEST_CASE(m3_byte_codec_u64_round_trip) {
    std::array<std::byte, 8> buf{};
    std::size_t off = 0;
    REQUIRE_EQ(write_u64_le(buf, off, 0x0123456789ABCDEFULL),
               ByteCodecError::none);
    off = 0;
    std::uint64_t val = 0;
    REQUIRE_EQ(read_u64_le(buf, off, val), ByteCodecError::none);
    REQUIRE_EQ(val, 0x0123456789ABCDEFULL);
}

TEST_CASE(m3_byte_codec_read_overflow_fails) {
    std::array<std::byte, 3> buf{};
    std::size_t off = 0;
    std::uint32_t val = 0;
    REQUIRE_EQ(read_u32_le(buf, off, val), ByteCodecError::out_of_bounds);
}

TEST_CASE(m3_byte_codec_write_overflow_fails) {
    std::array<std::byte, 1> buf{};
    std::size_t off = 0;
    REQUIRE_EQ(write_u16_le(buf, off, 1), ByteCodecError::out_of_bounds);
}

TEST_CASE(m3_byte_codec_checked_advance) {
    std::size_t off = 100;
    REQUIRE_EQ(checked_advance(off, 1, 100), ByteCodecError::out_of_bounds);
    off = 99;
    REQUIRE_EQ(checked_advance(off, 1, 100), ByteCodecError::none);
    REQUIRE_EQ(off, 100U);
}

TEST_CASE(m3_byte_codec_write_zeros_and_read_bytes) {
    std::array<std::byte, 8> buf{};
    std::size_t off = 0;
    REQUIRE_EQ(write_zeros(buf, off, 4), ByteCodecError::none);
    REQUIRE_EQ(write_u32_le(buf, off, 42), ByteCodecError::none);
    off = 0;
    std::array<std::byte, 4> zb{};
    REQUIRE_EQ(read_bytes(std::as_bytes(std::span(buf)), off, zb),
               ByteCodecError::none);
    for (auto b : zb) REQUIRE(static_cast<int>(b) == 0);
    std::uint32_t v = 0;
    REQUIRE_EQ(read_u32_le(std::as_bytes(std::span(buf)), off, v),
               ByteCodecError::none);
    REQUIRE_EQ(v, 42U);
}
