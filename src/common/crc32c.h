#pragma once

/// @file crc32c.h
/// @brief CRC32C (Castagnoli polynomial 0x1EDC6F41) one-shot and streaming API.
///
/// Table-driven, deterministic, no runtime CPU feature detection.

#include <cstddef>
#include <cstdint>
#include <span>

namespace myrd {

/// Compute the CRC32C of @p data in one call.
/// Returns 0 for empty input (consistent with CRC32C convention).
std::uint32_t crc32c(std::span<const std::byte> data) noexcept;

/// Streaming CRC32C accumulator.  update() may be called any number of times;
/// finalize() returns the final CRC32C and resets the state.
class Crc32cStream {
public:
    Crc32cStream() noexcept;
    void update(std::span<const std::byte> data) noexcept;
    std::uint32_t finalize() noexcept;

private:
    std::uint32_t crc_{};
    bool finalized_{};
};

}  // namespace myrd
