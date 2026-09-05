#include "src/product/uuid.h"

#include <windows.h>
#include <bcrypt.h>

namespace myrd::product {

namespace {

constexpr std::size_t kCanonicalLength = 36;
constexpr std::uint32_t kRngFailureCode = 0xE001;

int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool is_hyphen_at(std::string_view s, std::size_t pos) noexcept {
    return s[pos] == '-';
}

} // anonymous namespace

Result<Uuid> Uuid::parse(std::string_view canonical) {
    if (canonical.size() != kCanonicalLength) {
        return Result<Uuid>::failure(Error{
            ErrorDomain::usage, 0,
            L"Uuid::parse",
            L"UUID must be exactly 36 characters"});
    }

    if (!is_hyphen_at(canonical, 8) ||
        !is_hyphen_at(canonical, 13) ||
        !is_hyphen_at(canonical, 18) ||
        !is_hyphen_at(canonical, 23)) {
        return Result<Uuid>::failure(Error{
            ErrorDomain::usage, 0,
            L"Uuid::parse",
            L"UUID has incorrect hyphen positions"});
    }

    Uuid uuid;
    for (std::size_t i = 0, byte_idx = 0; i < kCanonicalLength; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;

        int nibble = hex_nibble(canonical[i]);
        if (nibble < 0) {
            return Result<Uuid>::failure(Error{
                ErrorDomain::usage, 0,
                L"Uuid::parse",
                L"UUID contains invalid hex character"});
        }

        if ((byte_idx & 1) == 0) {
            uuid.data_[byte_idx / 2] = static_cast<std::byte>(nibble << 4);
        } else {
            uuid.data_[byte_idx / 2] |= static_cast<std::byte>(nibble);
        }
        ++byte_idx;
    }

    return Result<Uuid>::success(uuid);
}

Result<Uuid> Uuid::random() {
    Uuid uuid;
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(uuid.data_.data()),
        static_cast<ULONG>(uuid.data_.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (!BCRYPT_SUCCESS(status)) {
        return Result<Uuid>::failure(Error{
            ErrorDomain::internal,
            kRngFailureCode,
            L"Uuid::random",
            L"BCryptGenRandom failed",
            static_cast<std::uint32_t>(status)});
    }

    uuid.data_[6] = static_cast<std::byte>(
        (static_cast<unsigned char>(uuid.data_[6]) & 0x0F) | 0x40);
    uuid.data_[8] = static_cast<std::byte>(
        (static_cast<unsigned char>(uuid.data_[8]) & 0x3F) | 0x80);

    return Result<Uuid>::success(uuid);
}

std::string Uuid::to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(kCanonicalLength);

    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            result += '-';
        }
        auto b = static_cast<unsigned char>(data_[i]);
        result += kHex[b >> 4];
        result += kHex[b & 0x0F];
    }

    return result;
}

std::span<const std::byte, 16> Uuid::bytes() const noexcept {
    return std::span<const std::byte, 16>(data_);
}

} // namespace myrd::product
