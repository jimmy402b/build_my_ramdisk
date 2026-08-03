#pragma once

#include <cstdint>

namespace myrd {

inline constexpr std::uint64_t kBlockSizeBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kBlocksPerLeaf = 1024ULL;
inline constexpr std::uint64_t kLeafSizeBytes =
    kBlockSizeBytes * kBlocksPerLeaf;

constexpr bool is_supported_sector_size(
    std::uint32_t sector_size_bytes) noexcept {
    return sector_size_bytes == 512 || sector_size_bytes == 4'096;
}

} // namespace myrd
