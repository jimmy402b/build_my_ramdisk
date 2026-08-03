#pragma once

#include "block_constants.h"

#include <cstdint>
#include <limits>
#include <string_view>

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
    SegmentIndexOutOfRange = 10,
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
    case BlockMathError::SegmentIndexOutOfRange:
        return "SegmentIndexOutOfRange";
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

struct BlockRangeSegmentResult {
    BlockRangeSegment value{};
    BlockMathError error{BlockMathError::None};

    constexpr bool ok() const noexcept {
        return error == BlockMathError::None;
    }
};

struct BlockRangeSplitResult {
    std::uint64_t request_offset_bytes{};
    std::uint64_t request_end_bytes{};
    std::uint64_t first_block_index{};
    std::uint64_t segment_count{};
    BlockMathError error{BlockMathError::None};

    constexpr bool ok() const noexcept {
        return error == BlockMathError::None;
    }

    constexpr BlockRangeSegmentResult segment_at(
        std::uint64_t segment_index) const noexcept {
        if (!ok() || segment_index >= segment_count) {
            return BlockRangeSegmentResult{
                {}, BlockMathError::SegmentIndexOutOfRange};
        }

        const auto block_index =
            checked_add_u64(first_block_index, segment_index);
        if (!block_index.ok()) {
            return BlockRangeSegmentResult{{}, block_index.error};
        }
        const auto block_start =
            checked_multiply_u64(block_index.value, kBlockSizeBytes);
        if (!block_start.ok()) {
            return BlockRangeSegmentResult{{}, block_start.error};
        }

        const std::uint64_t block_offset =
            segment_index == 0
                ? request_offset_bytes - block_start.value
                : 0ULL;
        const auto absolute_start =
            checked_add_u64(block_start.value, block_offset);
        if (!absolute_start.ok()) {
            return BlockRangeSegmentResult{{}, absolute_start.error};
        }
        if (absolute_start.value < request_offset_bytes ||
            absolute_start.value >= request_end_bytes ||
            block_offset >= kBlockSizeBytes) {
            return BlockRangeSegmentResult{{}, BlockMathError::OutOfRange};
        }

        const std::uint64_t request_remaining =
            request_end_bytes - absolute_start.value;
        const std::uint64_t block_remaining =
            kBlockSizeBytes - block_offset;
        const std::uint64_t segment_length =
            request_remaining < block_remaining
                ? request_remaining
                : block_remaining;

        return BlockRangeSegmentResult{
            BlockRangeSegment{
                block_index.value,
                block_offset,
                segment_length,
                absolute_start.value - request_offset_bytes,
            },
            BlockMathError::None,
        };
    }
};

constexpr BlockRangeSplitResult failed_block_range_split(
    BlockMathError error) noexcept {
    return BlockRangeSplitResult{0, 0, 0, 0, error};
}

constexpr BlockRangeSplitResult validate_and_split_block_range(
    std::uint64_t logical_size_bytes,
    std::uint32_t sector_size_bytes,
    std::uint64_t offset_bytes,
    std::uint64_t length_bytes,
    std::uint64_t buffer_length_bytes) noexcept {
    if (logical_size_bytes == 0) {
        return failed_block_range_split(BlockMathError::InvalidLogicalSize);
    }
    if (sector_size_bytes == 0) {
        return failed_block_range_split(BlockMathError::InvalidSectorSize);
    }
    if (length_bytes == 0) {
        return failed_block_range_split(BlockMathError::ZeroLength);
    }

    const std::uint64_t sector_size = sector_size_bytes;
    if ((offset_bytes % sector_size) != 0 ||
        (length_bytes % sector_size) != 0) {
        return failed_block_range_split(BlockMathError::Misaligned);
    }
    if (buffer_length_bytes != length_bytes) {
        return failed_block_range_split(
            BlockMathError::BufferLengthMismatch);
    }

    const auto end = checked_add_u64(offset_bytes, length_bytes);
    if (!end.ok()) {
        return failed_block_range_split(end.error);
    }
    if (end.value > logical_size_bytes) {
        return failed_block_range_split(BlockMathError::OutOfRange);
    }

    const std::uint64_t first_block_index =
        offset_bytes / kBlockSizeBytes;
    const std::uint64_t last_block_index =
        (end.value - 1) / kBlockSizeBytes;
    const auto segment_count = checked_add_u64(
        last_block_index - first_block_index, 1);
    if (!segment_count.ok()) {
        return failed_block_range_split(segment_count.error);
    }

    return BlockRangeSplitResult{
        offset_bytes,
        end.value,
        first_block_index,
        segment_count.value,
        BlockMathError::None,
    };
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
