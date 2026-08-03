#pragma once

#include "block_constants.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace myrd {

enum class BlockMathError : std::uint8_t {
    None = 0,
    InvalidAlignment = 1,
    Overflow = 2,
    InvalidLogicalSize = 3,
    InvalidSectorSize = 4,
    ZeroLength = 5,
    Misaligned = 6,
    BufferLengthMismatch = 7,
    OutOfRange = 8,
    DivergentBlocksExceedTotal = 9,
};

constexpr std::string_view to_string(BlockMathError error) noexcept {
    switch (error) {
    case BlockMathError::None:
        return "None";
    case BlockMathError::InvalidAlignment:
        return "InvalidAlignment";
    case BlockMathError::Overflow:
        return "Overflow";
    case BlockMathError::InvalidLogicalSize:
        return "InvalidLogicalSize";
    case BlockMathError::InvalidSectorSize:
        return "InvalidSectorSize";
    case BlockMathError::ZeroLength:
        return "ZeroLength";
    case BlockMathError::Misaligned:
        return "Misaligned";
    case BlockMathError::BufferLengthMismatch:
        return "BufferLengthMismatch";
    case BlockMathError::OutOfRange:
        return "OutOfRange";
    case BlockMathError::DivergentBlocksExceedTotal:
        return "DivergentBlocksExceedTotal";
    }
    return "UnknownBlockMathError";
}

struct CheckedU64Result {
    std::uint64_t value{};
    BlockMathError error{BlockMathError::None};

    constexpr bool ok() const noexcept {
        return error == BlockMathError::None;
    }
};

constexpr CheckedU64Result checked_add_u64(std::uint64_t left,
                                           std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return CheckedU64Result{0, BlockMathError::Overflow};
    }
    return CheckedU64Result{left + right, BlockMathError::None};
}

constexpr CheckedU64Result checked_multiply_u64(
    std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return CheckedU64Result{0, BlockMathError::Overflow};
    }
    return CheckedU64Result{left * right, BlockMathError::None};
}

constexpr CheckedU64Result checked_round_up_u64(
    std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment == 0) {
        return CheckedU64Result{0, BlockMathError::InvalidAlignment};
    }
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0) {
        return CheckedU64Result{value, BlockMathError::None};
    }
    return checked_add_u64(value, alignment - remainder);
}

struct BlockGeometry {
    std::uint64_t logical_size_bytes{};
    std::uint64_t reservation_size_bytes{};
    std::uint64_t total_blocks{};
    std::uint64_t total_leaves{};
    std::uint64_t last_block_size_bytes{};
};

struct BlockGeometryResult {
    BlockGeometry value{};
    BlockMathError error{BlockMathError::None};

    constexpr bool ok() const noexcept {
        return error == BlockMathError::None;
    }
};

constexpr BlockGeometryResult compute_block_geometry(
    std::uint64_t logical_size_bytes) noexcept {
    if (logical_size_bytes == 0) {
        return BlockGeometryResult{{}, BlockMathError::InvalidLogicalSize};
    }

    const auto reservation =
        checked_round_up_u64(logical_size_bytes, kBlockSizeBytes);
    if (!reservation.ok()) {
        return BlockGeometryResult{{}, reservation.error};
    }

    const std::uint64_t total_blocks =
        reservation.value / kBlockSizeBytes;
    const std::uint64_t total_leaves =
        (total_blocks / kBlocksPerLeaf) +
        ((total_blocks % kBlocksPerLeaf) == 0 ? 0ULL : 1ULL);
    const std::uint64_t final_remainder =
        logical_size_bytes % kBlockSizeBytes;

    return BlockGeometryResult{
        BlockGeometry{
            logical_size_bytes,
            reservation.value,
            total_blocks,
            total_leaves,
            final_remainder == 0 ? kBlockSizeBytes : final_remainder,
        },
        BlockMathError::None,
    };
}

struct BlockRangeSegment {
    std::uint64_t block_index{};
    std::uint64_t block_offset_bytes{};
    std::uint64_t length_bytes{};
    std::uint64_t buffer_offset_bytes{};
};

struct BlockRangeSplitResult {
    std::vector<BlockRangeSegment> segments;
    BlockMathError error{BlockMathError::None};

    bool ok() const noexcept {
        return error == BlockMathError::None;
    }
};

inline BlockRangeSplitResult validate_and_split_block_range(
    std::uint64_t logical_size_bytes,
    std::uint32_t sector_size_bytes,
    std::uint64_t offset_bytes,
    std::uint64_t length_bytes,
    std::uint64_t buffer_length_bytes) {
    if (logical_size_bytes == 0) {
        return BlockRangeSplitResult{{}, BlockMathError::InvalidLogicalSize};
    }
    if (sector_size_bytes == 0) {
        return BlockRangeSplitResult{{}, BlockMathError::InvalidSectorSize};
    }
    if (length_bytes == 0) {
        return BlockRangeSplitResult{{}, BlockMathError::ZeroLength};
    }

    const std::uint64_t sector_size = sector_size_bytes;
    if ((offset_bytes % sector_size) != 0 ||
        (length_bytes % sector_size) != 0) {
        return BlockRangeSplitResult{{}, BlockMathError::Misaligned};
    }
    if (buffer_length_bytes != length_bytes) {
        return BlockRangeSplitResult{
            {}, BlockMathError::BufferLengthMismatch};
    }

    const auto end = checked_add_u64(offset_bytes, length_bytes);
    if (!end.ok()) {
        return BlockRangeSplitResult{{}, end.error};
    }
    if (end.value > logical_size_bytes) {
        return BlockRangeSplitResult{{}, BlockMathError::OutOfRange};
    }

    BlockRangeSplitResult result;
    std::uint64_t current = offset_bytes;
    std::uint64_t buffer_offset = 0;
    while (current < end.value) {
        const std::uint64_t block_offset = current % kBlockSizeBytes;
        const std::uint64_t block_remaining =
            kBlockSizeBytes - block_offset;
        const std::uint64_t request_remaining = end.value - current;
        const std::uint64_t segment_length =
            std::min(block_remaining, request_remaining);
        result.segments.push_back(BlockRangeSegment{
            current / kBlockSizeBytes,
            block_offset,
            segment_length,
            buffer_offset,
        });
        current += segment_length;
        buffer_offset += segment_length;
    }
    return result;
}

constexpr CheckedU64Result implicit_clean_zero_block_count(
    std::uint64_t total_blocks,
    std::uint64_t divergent_blocks) noexcept {
    if (divergent_blocks > total_blocks) {
        return CheckedU64Result{
            0, BlockMathError::DivergentBlocksExceedTotal};
    }
    return CheckedU64Result{
        total_blocks - divergent_blocks, BlockMathError::None};
}

} // namespace myrd
