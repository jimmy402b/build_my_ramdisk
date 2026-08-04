/// @file crc32c.cpp
/// @brief CRC32C Castagnoli — table-driven, no hardware acceleration required.

#include "crc32c.h"

#include <array>
#include <cstdint>
#include <span>

namespace myrd {
namespace {

// CRC32C Castagnoli polynomial 0x1EDC6F41 reflected.
constexpr std::uint32_t kCrc32cPoly = 0x82F63B78U;

constexpr auto kTable = []() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1) ? (kCrc32cPoly ^ (crc >> 1)) : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}();

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto idx = static_cast<std::uint8_t>(
            (crc ^ static_cast<std::uint32_t>(data[i])) & 0xFFU);
        crc = kTable[idx] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

Crc32cStream::Crc32cStream() noexcept : crc_(0xFFFFFFFFU) {}

Result<void> Crc32cStream::update(std::span<const std::byte> data) {
    if (finalized_) {
        return Result<void>::failure(
            {ErrorDomain::internal,
             static_cast<std::uint32_t>(Crc32cErrorCode::already_finalized),
             L"Crc32cStream::update", L"stream already finalized"});
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto idx = static_cast<std::uint8_t>(
            (crc_ ^ static_cast<std::uint32_t>(data[i])) & 0xFFU);
        crc_ = kTable[idx] ^ (crc_ >> 8);
    }
    return Result<void>::success();
}

Result<std::uint32_t> Crc32cStream::finalize() {
    if (finalized_) {
        return Result<std::uint32_t>::failure(
            {ErrorDomain::internal,
             static_cast<std::uint32_t>(Crc32cErrorCode::already_finalized),
             L"Crc32cStream::finalize", L"stream already finalized"});
    }
    finalized_ = true;
    return Result<std::uint32_t>::success(crc_ ^ 0xFFFFFFFFU);
}

}  // namespace myrd
