#pragma once

#include <cstdint>

namespace myrd {

inline constexpr std::uint64_t kBlockSizeBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kBlocksPerLeaf = 1024ULL;
inline constexpr std::uint64_t kLeafSizeBytes =
    kBlockSizeBytes * kBlocksPerLeaf;

} // namespace myrd
