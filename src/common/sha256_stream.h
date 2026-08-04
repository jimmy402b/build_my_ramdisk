#pragma once

/// @file sha256_stream.h
/// @brief Streaming SHA-256 accumulator that collects data and finalizes via
/// the battle-tested one-shot sha256_hex().

#include "result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace myrd {

class Sha256Stream {
public:
    Sha256Stream() = default;

    /// Append @p data to the accumulator.
    Result<void> update(std::span<const std::byte> data);

    /// Finalize and return uppercase hex string.
    Result<std::string> finalize();

private:
    std::vector<std::byte> buffer_;
    bool finalized_{};
};

}  // namespace myrd
