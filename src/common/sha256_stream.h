#pragma once

/// @file sha256_stream.h
/// @brief Bounded streaming SHA-256 accumulator backed by BCrypt.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "result.h"
#include "sha256_stream_state.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd {

enum class Sha256StreamErrorCode : std::uint32_t {
    already_finalized = detail::kSha256AlreadyFinalizedCode,
    initialization_failed = 0x2202U,
    hash_update_failed = 0x2203U,
    hash_finalize_failed = 0x2204U,
    invalid_hash_length = 0x2205U,
    allocation_failed = 0x2206U,
};

class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();

    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    /// Append @p data to the accumulator.
    Result<void> update(std::span<const std::byte> data);

    /// Finalize and return uppercase hex string.
    Result<std::string> finalize();

private:
    Result<void> initialize();

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> hash_object_;
    std::vector<UCHAR> digest_;
    detail::Sha256StreamState state_;
    // Constructed before any input is hashed so a BCrypt failure can move a
    // complete diagnostic into the sticky state without allocating.
    Error hash_update_error_;
    Error hash_finalize_error_;
};

}  // namespace myrd
