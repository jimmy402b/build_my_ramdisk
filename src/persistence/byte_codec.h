#pragma once

/// @file byte_codec.h
/// @brief Checked little-endian fixed-buffer reads/writes; no file I/O.
///
/// All helpers return false on overflow/underflow rather than throwing.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace myrd {
namespace persistence {

// ---- Little-endian integer codec -------------------------------------------

inline bool read_u16_le(
    std::span<const std::byte> buf, std::size_t& offset,
    std::uint16_t& value) noexcept {
    if (offset + sizeof(std::uint16_t) > buf.size()) return false;
    std::memcpy(&value, buf.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

inline bool read_u32_le(
    std::span<const std::byte> buf, std::size_t& offset,
    std::uint32_t& value) noexcept {
    if (offset + sizeof(std::uint32_t) > buf.size()) return false;
    std::memcpy(&value, buf.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

inline bool read_u64_le(
    std::span<const std::byte> buf, std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (offset + sizeof(std::uint64_t) > buf.size()) return false;
    std::memcpy(&value, buf.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

// ---- Raw bytes -------------------------------------------------------------

inline bool read_bytes(
    std::span<const std::byte> buf, std::size_t& offset,
    std::span<std::byte> dest) noexcept {
    if (offset + dest.size() > buf.size()) return false;
    std::memcpy(dest.data(), buf.data() + offset, dest.size());
    offset += dest.size();
    return true;
}

inline bool skip_bytes(
    std::span<const std::byte> buf, std::size_t& offset,
    std::size_t count) noexcept {
    if (offset + count > buf.size()) return false;
    offset += count;
    return true;
}

// ---- Write helpers ---------------------------------------------------------

inline bool write_u16_le(
    std::span<std::byte> buf, std::size_t& offset,
    std::uint16_t value) noexcept {
    if (offset + sizeof(value) > buf.size()) return false;
    std::memcpy(buf.data() + offset, &value, sizeof(value));
    offset += sizeof(value);
    return true;
}

inline bool write_u32_le(
    std::span<std::byte> buf, std::size_t& offset,
    std::uint32_t value) noexcept {
    if (offset + sizeof(value) > buf.size()) return false;
    std::memcpy(buf.data() + offset, &value, sizeof(value));
    offset += sizeof(value);
    return true;
}

inline bool write_u64_le(
    std::span<std::byte> buf, std::size_t& offset,
    std::uint64_t value) noexcept {
    if (offset + sizeof(value) > buf.size()) return false;
    std::memcpy(buf.data() + offset, &value, sizeof(value));
    offset += sizeof(value);
    return true;
}

inline bool write_bytes(
    std::span<std::byte> buf, std::size_t& offset,
    std::span<const std::byte> src) noexcept {
    if (offset + src.size() > buf.size()) return false;
    std::memcpy(buf.data() + offset, src.data(), src.size());
    offset += src.size();
    return true;
}

inline bool write_zeros(
    std::span<std::byte> buf, std::size_t& offset,
    std::size_t count) noexcept {
    if (offset + count > buf.size()) return false;
    std::memset(buf.data() + offset, 0, count);
    offset += count;
    return true;
}

// ---- Checked advance -------------------------------------------------------

inline bool checked_advance(
    std::size_t& offset, std::size_t increment,
    std::size_t limit) noexcept {
    if (increment > limit || offset > limit - increment) return false;
    offset += increment;
    return true;
}

}  // namespace persistence
}  // namespace myrd
