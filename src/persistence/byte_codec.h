#pragma once

/// @file byte_codec.h
/// @brief Checked little-endian fixed-buffer reads and writes; no file I/O.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace myrd::persistence {

enum class ByteCodecError : std::uint8_t {
    none,
    out_of_bounds,
    arithmetic_overflow,
    invalid_alignment,
    invalid_utf8,
    length_exceeds_limit,
    nonzero_reserved,
};

using UuidBytes = std::array<std::byte, 16>;

inline ByteCodecError checked_range(
    std::size_t offset, std::size_t length, std::size_t limit) noexcept {
    if (offset > limit || length > limit - offset) {
        return ByteCodecError::out_of_bounds;
    }
    return ByteCodecError::none;
}

inline ByteCodecError checked_advance(
    std::size_t& offset, std::size_t increment,
    std::size_t limit) noexcept {
    if (checked_range(offset, increment, limit) != ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    offset += increment;
    return ByteCodecError::none;
}

inline ByteCodecError checked_align_up(
    std::size_t value, std::size_t alignment, std::size_t& aligned) noexcept {
    if (alignment == 0) {
        return ByteCodecError::invalid_alignment;
    }
    const auto remainder = value % alignment;
    const auto increment = remainder == 0 ? 0 : alignment - remainder;
    if (increment > std::numeric_limits<std::size_t>::max() - value) {
        return ByteCodecError::arithmetic_overflow;
    }
    aligned = value + increment;
    return ByteCodecError::none;
}

inline ByteCodecError read_u16_le(
    std::span<const std::byte> buffer, std::size_t& offset,
    std::uint16_t& value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    const auto decoded = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(buffer[base]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(buffer[base + 1]))
         << 8U));
    value = decoded;
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError read_u32_le(
    std::span<const std::byte> buffer, std::size_t& offset,
    std::uint32_t& value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    std::uint32_t decoded = 0;
    for (std::size_t i = 0; i < sizeof(decoded); ++i) {
        decoded |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(buffer[base + i])) << (i * 8U);
    }
    value = decoded;
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError read_u64_le(
    std::span<const std::byte> buffer, std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    std::uint64_t decoded = 0;
    for (std::size_t i = 0; i < sizeof(decoded); ++i) {
        decoded |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(buffer[base + i])) << (i * 8U);
    }
    value = decoded;
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError read_bytes(
    std::span<const std::byte> buffer, std::size_t& offset,
    std::span<std::byte> destination) noexcept {
    if (checked_range(offset, destination.size(), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = buffer[base + i];
    }
    offset = base + destination.size();
    return ByteCodecError::none;
}

inline ByteCodecError skip_bytes(
    std::span<const std::byte> buffer, std::size_t& offset,
    std::size_t count) noexcept {
    if (checked_range(offset, count, buffer.size()) != ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    offset += count;
    return ByteCodecError::none;
}

inline ByteCodecError write_u16_le(
    std::span<std::byte> buffer, std::size_t& offset,
    std::uint16_t value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    buffer[base] = static_cast<std::byte>(value & 0xFFU);
    buffer[base + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError write_u32_le(
    std::span<std::byte> buffer, std::size_t& offset,
    std::uint32_t value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        buffer[base + i] =
            static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError write_u64_le(
    std::span<std::byte> buffer, std::size_t& offset,
    std::uint64_t value) noexcept {
    if (checked_range(offset, sizeof(value), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        buffer[base + i] =
            static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
    offset = base + sizeof(value);
    return ByteCodecError::none;
}

inline ByteCodecError write_bytes(
    std::span<std::byte> buffer, std::size_t& offset,
    std::span<const std::byte> source) noexcept {
    if (checked_range(offset, source.size(), buffer.size()) !=
        ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    for (std::size_t i = 0; i < source.size(); ++i) {
        buffer[base + i] = source[i];
    }
    offset = base + source.size();
    return ByteCodecError::none;
}

inline ByteCodecError write_zeros(
    std::span<std::byte> buffer, std::size_t& offset,
    std::size_t count) noexcept {
    if (checked_range(offset, count, buffer.size()) != ByteCodecError::none) {
        return ByteCodecError::out_of_bounds;
    }
    const auto base = offset;
    for (std::size_t i = 0; i < count; ++i) {
        buffer[base + i] = std::byte{0};
    }
    offset = base + count;
    return ByteCodecError::none;
}

inline ByteCodecError write_uuid(
    std::span<std::byte> buffer, std::size_t& offset,
    const UuidBytes& uuid) noexcept {
    return write_bytes(buffer, offset, uuid);
}

inline ByteCodecError read_uuid(
    std::span<const std::byte> buffer, std::size_t& offset,
    UuidBytes& uuid) noexcept {
    UuidBytes decoded{};
    auto local_offset = offset;
    const auto result = read_bytes(buffer, local_offset, decoded);
    if (result != ByteCodecError::none) {
        return result;
    }
    uuid = decoded;
    offset = local_offset;
    return ByteCodecError::none;
}

inline ByteCodecError validate_bounded_utf8(
    std::span<const std::byte> data, std::size_t max_bytes) noexcept {
    if (data.size() > max_bytes) {
        return ByteCodecError::length_exceeds_limit;
    }

    std::size_t index = 0;
    while (index < data.size()) {
        const auto lead = std::to_integer<std::uint8_t>(data[index]);
        std::size_t continuation_count = 0;
        std::uint8_t first_min = 0x80U;
        std::uint8_t first_max = 0xBFU;
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            continuation_count = 1;
        } else if (lead == 0xE0U) {
            continuation_count = 2;
            first_min = 0xA0U;
        } else if (lead >= 0xE1U && lead <= 0xECU) {
            continuation_count = 2;
        } else if (lead == 0xEDU) {
            continuation_count = 2;
            first_max = 0x9FU;
        } else if (lead >= 0xEEU && lead <= 0xEFU) {
            continuation_count = 2;
        } else if (lead == 0xF0U) {
            continuation_count = 3;
            first_min = 0x90U;
        } else if (lead >= 0xF1U && lead <= 0xF3U) {
            continuation_count = 3;
        } else if (lead == 0xF4U) {
            continuation_count = 3;
            first_max = 0x8FU;
        } else {
            return ByteCodecError::invalid_utf8;
        }

        if (data.size() - index - 1 < continuation_count) {
            return ByteCodecError::invalid_utf8;
        }
        const auto first = std::to_integer<std::uint8_t>(data[index + 1]);
        if (first < first_min || first > first_max) {
            return ByteCodecError::invalid_utf8;
        }
        for (std::size_t i = 2; i <= continuation_count; ++i) {
            const auto continuation =
                std::to_integer<std::uint8_t>(data[index + i]);
            if (continuation < 0x80U || continuation > 0xBFU) {
                return ByteCodecError::invalid_utf8;
            }
        }
        index += continuation_count + 1;
    }
    return ByteCodecError::none;
}

inline ByteCodecError require_zero(
    std::span<const std::byte> data) noexcept {
    for (const auto byte : data) {
        if (byte != std::byte{0}) {
            return ByteCodecError::nonzero_reserved;
        }
    }
    return ByteCodecError::none;
}

}  // namespace myrd::persistence
