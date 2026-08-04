#pragma once

#include "block_constants.h"
#include "dynamic_block_store_internal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace myrd {

// Optional test seam around the actual per-slot publication attempt. Production
// construction leaves it null and pays only one predictable branch.
class IBlockLeafPublicationObserver {
public:
    virtual ~IBlockLeafPublicationObserver() = default;
    virtual void before_publish(std::uint64_t leaf_index) noexcept = 0;
    virtual void after_publish(std::uint64_t leaf_index) noexcept = 0;
};

constexpr std::uint64_t block_leaf_index(
    std::uint64_t block_index) noexcept {
    return block_index / kBlocksPerLeaf;
}

constexpr std::uint64_t block_entry_index(
    std::uint64_t block_index) noexcept {
    return block_index % kBlocksPerLeaf;
}

struct BlockLeaf final {
    static constexpr std::uint64_t kEntryCount = kBlocksPerLeaf;
    static constexpr std::uint64_t kCoverageBytes = kLeafSizeBytes;

    explicit BlockLeaf(const BlockEntry& initial_entry) noexcept {
        entries.fill(initial_entry);
    }

    std::array<BlockEntry, kBlocksPerLeaf> entries;
};

struct AllocatedBlockLeaf final {
    std::uint64_t leaf_index{};
    BlockLeaf* leaf{};
};

enum class BlockDirectoryError : std::uint8_t {
    None = 0,
    InvalidBlockCount = 1,
    InvalidInitialState = 2,
    AllocationFailed = 3,
    OutOfRange = 4,
};

class BlockDirectory final {
public:
    using CreateResult = StorageInternalResult<
        std::unique_ptr<BlockDirectory>, BlockDirectoryError>;
    using AccessResult = StorageInternalResult<
        BlockEntry*, BlockDirectoryError>;

    static CreateResult create(
        std::uint64_t block_count,
        InitialBlockState initial_state,
        BackingGeneration initial_generation,
        std::shared_ptr<IBlockLeafPublicationObserver> publication_observer =
            {}) noexcept;

    ~BlockDirectory();

    BlockDirectory(const BlockDirectory&) = delete;
    BlockDirectory& operator=(const BlockDirectory&) = delete;
    BlockDirectory(BlockDirectory&&) = delete;
    BlockDirectory& operator=(BlockDirectory&&) = delete;

    BlockEntry* find(std::uint64_t block_index) noexcept;
    const BlockEntry* find(std::uint64_t block_index) const noexcept;
    AccessResult get_or_create(std::uint64_t block_index) noexcept;

    // Leaves never move or disappear before directory destruction. Scanning
    // the top-level slots therefore visits only materialized metadata instead
    // of walking every logical block in a sparse store.
    const BlockLeaf* allocated_leaf_at(
        std::uint64_t leaf_index) const noexcept;
    // Returns a stable leaf pointer after releasing the index mutex. Leaves
    // never disappear, so callers must not hold this mutex while taking a
    // block stripe.
    std::optional<AllocatedBlockLeaf> next_allocated_leaf_at_or_after(
        std::uint64_t leaf_index) const noexcept;

    std::uint64_t block_count() const noexcept { return block_count_; }
    std::uint64_t top_level_slot_count() const noexcept {
        return top_level_slot_count_;
    }
    std::uint64_t allocated_leaf_count() const noexcept {
        return allocated_leaf_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t initial_metadata_bytes() const noexcept {
        return top_level_slot_count_ *
               (sizeof(std::atomic<BlockLeaf*>) + sizeof(std::uint64_t));
    }
    InitialBlockState initial_state() const noexcept { return initial_state_; }
    BackingGeneration initial_generation() const noexcept {
        return initial_generation_;
    }

private:
    BlockDirectory(
        std::uint64_t block_count,
        std::uint64_t top_level_slot_count,
        InitialBlockState initial_state,
        BackingGeneration initial_generation,
        std::unique_ptr<std::atomic<BlockLeaf*>[]> slots,
        std::vector<std::uint64_t> allocated_leaf_indices,
        std::shared_ptr<IBlockLeafPublicationObserver>
            publication_observer) noexcept;

    BlockLeaf* load_leaf(std::uint64_t leaf_index) const noexcept;

    std::uint64_t block_count_{};
    std::uint64_t top_level_slot_count_{};
    InitialBlockState initial_state_{InitialBlockState::CleanZero};
    BackingGeneration initial_generation_{};
    BlockEntry initial_entry_{};
    std::unique_ptr<std::atomic<BlockLeaf*>[]> slots_;
    mutable std::mutex allocated_leaves_mutex_;
    std::vector<std::uint64_t> allocated_leaf_indices_;
    std::shared_ptr<IBlockLeafPublicationObserver> publication_observer_;
    std::atomic<std::uint64_t> allocated_leaf_count_{0};
};

} // namespace myrd
