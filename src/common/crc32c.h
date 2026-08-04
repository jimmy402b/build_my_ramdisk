#pragma once

/// @file crc32c.h
/// @brief CRC32C (Castagnoli polynomial 0x1EDC6F41) one-shot and streaming API.
///
/// Table-driven, deterministic, no runtime CPU feature detection.

#include <cstddef>
#include <cstdint>
#include <span>

#include "result.h"

namespace myrd {

/// Compute the CRC32C of @p data in one call.
/// Returns 0 for empty input (consistent with CRC32C convention).
std::uint32_t crc32c(std::span<const std::byte> data) noexcept;

enum class Crc32cErrorCode : std::uint32_t {
    already_finalized = 0x2101U,
};

/// Streaming CRC32C accumulator. finalize() succeeds exactly once.
class Crc32cStream {
public:
    Crc32cStream() noexcept;
    Result<void> update(std::span<const std::byte> data);
    Result<std::uint32_t> finalize();

private:
    std::uint32_t crc_{};
    bool finalized_{};
};

}  // namespace myrd
