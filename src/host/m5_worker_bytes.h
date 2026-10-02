#pragma once

// Byte primitives shared by the private worker protocol and the worker's
// startup channel.
//
// They were originally private to `m5_worker_protocol.cpp`.  The launch spec is
// a second thing that has to cross this boundary, and a second hand-written
// little-endian reader would be a second place for the conventions below to
// drift -- so there is one.
//
// Conventions, all of them deliberate:
//
//   * Little-endian, fixed width, no padding and no alignment.
//   * Every length is written before the bytes it describes, so a reader never
//     has to trust a value it has not yet bounded.
//   * The reader is a cursor with a sticky failure flag.  Once anything is out
//     of range every later read returns a default and `ok()` stays false, so a
//     caller cannot accidentally act on a half-decoded message by forgetting to
//     check one intermediate step.

#include "src/common/sha256.h"
#include "src/persistence/persistence_types.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::m5::detail {

class WorkerBytesWriter final {
public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }
    void u16(const std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void u32(const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void u64(const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void raw(const std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void uuid(const product::Uuid& value) { raw(value.bytes()); }
    // Written as its sixteen bytes, like the product uuid above: an image uuid
    // has no canonical-text form on this channel, so the decoder never has to
    // parse one back.
    void image_uuid(const persistence::ImageUuid& value) { raw(value.bytes); }
    void digest(const Sha256Digest& value) {
        raw(std::span<const std::byte>(value.data(), value.size()));
    }
    // A boolean is written as one explicit byte rather than packed into a flag
    // word, so a decoder that dropped a bit cannot turn an absent proof into a
    // present one.
    void boolean(const bool value) { u8(value ? 1 : 0); }
    // Stored as UTF-16LE code units with a code-unit count, so the decoder never
    // has to trust a byte length to describe a character count.
    void text(const std::wstring& value) {
        u16(static_cast<std::uint16_t>(value.size()));
        for (const wchar_t unit : value) {
            const auto code_unit = static_cast<std::uint16_t>(unit);
            u8(static_cast<std::uint8_t>(code_unit & 0xFFU));
            u8(static_cast<std::uint8_t>((code_unit >> 8) & 0xFFU));
        }
    }

    std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class WorkerBytesReader final {
public:
    explicit WorkerBytesReader(const std::span<const std::byte> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool exhausted() const noexcept {
        return offset_ == bytes_.size();
    }

    std::uint8_t u8() {
        if (!ok_ || offset_ + 1 > bytes_.size()) return fail();
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }
    std::uint16_t u16() {
        std::uint16_t value = 0;
        for (int shift = 0; shift < 16; shift += 8)
            value |= static_cast<std::uint16_t>(u8()) << shift;
        return value;
    }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(u8()) << shift;
        return value;
    }
    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(u8()) << shift;
        return value;
    }
    std::span<const std::byte> raw(const std::size_t count) {
        if (!ok_ || offset_ + count > bytes_.size() ||
            offset_ + count < offset_) {
            (void)fail();
            return {};
        }
        const auto slice = bytes_.subspan(offset_, count);
        offset_ += count;
        return slice;
    }
    bool boolean() { return u8() != 0; }
    // product::Uuid has a private default constructor and only a canonical-text
    // factory, so a decoded id comes back through parse() or not at all.
    std::optional<product::Uuid> uuid() {
        const auto slice = raw(16);
        if (!ok_) return std::nullopt;
        std::string text_value;
        text_value.reserve(36);
        static constexpr char kHex[] = "0123456789abcdef";
        // Canonical 8-4-4-4-12 lowercase form, byte-identical to the helper the
        // public pipe codec already uses.
        for (std::size_t index = 0; index < slice.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                text_value.push_back('-');
            const auto byte = std::to_integer<std::uint8_t>(slice[index]);
            text_value.push_back(kHex[byte >> 4]);
            text_value.push_back(kHex[byte & 0x0FU]);
        }
        auto parsed = product::Uuid::parse(text_value);
        if (!parsed.ok()) {
            (void)fail();
            return std::nullopt;
        }
        return parsed.value();
    }
    persistence::ImageUuid image_uuid() {
        persistence::ImageUuid value{};
        const auto slice = raw(value.bytes.size());
        if (!ok_) return value;
        std::memcpy(value.bytes.data(), slice.data(), value.bytes.size());
        return value;
    }
    Sha256Digest digest() {
        Sha256Digest value{};
        const auto slice = raw(value.size());
        if (!ok_) return value;
        std::memcpy(value.data(), slice.data(), value.size());
        return value;
    }
    std::wstring text() {
        const auto count = u16();
        std::wstring value;
        value.reserve(count);
        for (std::uint16_t index = 0; index < count && ok_; ++index) {
            const auto low = static_cast<std::uint16_t>(u8());
            const auto high = static_cast<std::uint16_t>(u8());
            value.push_back(static_cast<wchar_t>(low | (high << 8)));
        }
        return value;
    }

private:
    std::uint8_t fail() {
        ok_ = false;
        return 0;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    bool ok_{true};
};

} // namespace myrd::m5::detail
