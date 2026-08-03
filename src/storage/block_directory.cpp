#include "block_directory.h"

#include <limits>
#include <new>

namespace myrd {

namespace {

bool is_known_block_state(BlockState state) noexcept {
    switch (state) {
    case BlockState::Unresident:
    case BlockState::ResidentClean:
    case BlockState::ResidentDirty:
    case BlockState::ZeroClean:
    case BlockState::ZeroDirty:
    case BlockState::Loading:
    case BlockState::Committing:
    case BlockState::Evicting:
        return true;
    }
    return false;
}

bool is_known_dirty_kind(DirtyRecordKind kind) noexcept {
    switch (kind) {
    case DirtyRecordKind::Data:
    case DirtyRecordKind::Zero:
    case DirtyRecordKind::Discard:
        return true;
    }
    return false;
}

BlockEntry make_validated_initial_block_entry(
    InitialBlockState initial_state,
    BackingGeneration initial_generation) noexcept {
    BlockEntry entry{};
    if (initial_state == InitialBlockState::CleanZero) {
        entry.state = BlockState::ZeroClean;
        entry.prior_state = BlockState::ZeroClean;
    } else {
        // BlockDirectory::create rejects every value except
        // BackingUnresident before this private helper is reached.
        entry.state = BlockState::Unresident;
        entry.prior_state = BlockState::Unresident;
    }
    entry.dirty_base_generation = initial_generation;
    entry.clean_generation = initial_generation;
    return entry;
}

} // namespace

BlockEntryInvariantResult validate_block_entry(
    const BlockEntry& entry,
    BackingGeneration current_backing_generation,
    bool backing_generation_wrapped,
    bool content_version_wrapped) noexcept {
    if (backing_generation_wrapped) {
        return {BlockEntryInvariantError::BackingGenerationWrapped};
    }
    if (content_version_wrapped) {
        return {BlockEntryInvariantError::ContentVersionWrapped};
    }
    if (!is_known_block_state(entry.state)) {
        return {BlockEntryInvariantError::InvalidState};
    }
    if (!is_known_block_state(entry.prior_state)) {
        return {BlockEntryInvariantError::InvalidPriorState};
    }
    if (!is_known_dirty_kind(entry.dirty_kind)) {
        return {BlockEntryInvariantError::InvalidDirtyKind};
    }
    if (is_transient_block_state(entry.state) &&
        !is_stable_block_state(entry.prior_state)) {
        return {BlockEntryInvariantError::TransientPriorNotStable};
    }
    if ((entry.state == BlockState::ResidentClean ||
         entry.state == BlockState::ZeroClean) &&
        entry.clean_generation > current_backing_generation) {
        return {BlockEntryInvariantError::CleanGenerationAhead};
    }
    if ((entry.state == BlockState::ResidentDirty ||
         entry.state == BlockState::ZeroDirty) &&
        entry.dirty_base_generation > current_backing_generation) {
        return {BlockEntryInvariantError::DirtyBaseGenerationAhead};
    }
    return {};
}

LockStripeSet::CreateResult LockStripeSet::create(
    std::size_t stripe_count) noexcept {
    if (stripe_count < 64 ||
        (stripe_count & (stripe_count - 1)) != 0) {
        return CreateResult::failure(LockStripeSetError::InvalidCount);
    }
    if (stripe_count >
        std::numeric_limits<std::size_t>::max() / sizeof(LockStripe)) {
        return CreateResult::failure(LockStripeSetError::AllocationFailed);
    }

    try {
        auto stripes = std::unique_ptr<LockStripe[]>(
            new (std::nothrow) LockStripe[stripe_count]);
        if (!stripes) {
            return CreateResult::failure(LockStripeSetError::AllocationFailed);
        }
        auto result = std::unique_ptr<LockStripeSet>(new (std::nothrow)
            LockStripeSet(std::move(stripes), stripe_count));
        if (!result) {
            return CreateResult::failure(LockStripeSetError::AllocationFailed);
        }
        return CreateResult::success(std::move(result));
    } catch (...) {
        return CreateResult::failure(LockStripeSetError::AllocationFailed);
    }
}

BlockDirectory::BlockDirectory(
    std::uint64_t block_count,
    std::uint64_t top_level_slot_count,
    InitialBlockState initial_state,
    BackingGeneration initial_generation,
    std::unique_ptr<std::atomic<BlockLeaf*>[]> slots,
    std::shared_ptr<IBlockLeafPublicationObserver>
        publication_observer) noexcept
    : block_count_(block_count),
      top_level_slot_count_(top_level_slot_count),
      initial_state_(initial_state),
      initial_generation_(initial_generation),
      initial_entry_(make_validated_initial_block_entry(
          initial_state, initial_generation)),
      slots_(std::move(slots)),
      publication_observer_(std::move(publication_observer)) {}

BlockDirectory::CreateResult BlockDirectory::create(
    std::uint64_t block_count,
    InitialBlockState initial_state,
    BackingGeneration initial_generation,
    std::shared_ptr<IBlockLeafPublicationObserver>
        publication_observer) noexcept {
    if (block_count == 0) {
        return CreateResult::failure(BlockDirectoryError::InvalidBlockCount);
    }
    if (initial_state != InitialBlockState::CleanZero &&
        initial_state != InitialBlockState::BackingUnresident) {
        return CreateResult::failure(BlockDirectoryError::InvalidInitialState);
    }

    const std::uint64_t slot_count =
        (block_count / kBlocksPerLeaf) +
        ((block_count % kBlocksPerLeaf) == 0 ? 0ull : 1ull);
    constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
    if (slot_count > kMaxSize ||
        slot_count > kMaxSize / sizeof(std::atomic<BlockLeaf*>)) {
        return CreateResult::failure(BlockDirectoryError::AllocationFailed);
    }

    try {
        auto slots = std::unique_ptr<std::atomic<BlockLeaf*>[]>(
            new (std::nothrow) std::atomic<BlockLeaf*>[
                static_cast<std::size_t>(slot_count)]);
        if (!slots) {
            return CreateResult::failure(BlockDirectoryError::AllocationFailed);
        }
        for (std::uint64_t index = 0; index < slot_count; ++index) {
            slots[static_cast<std::size_t>(index)].store(
                nullptr, std::memory_order_relaxed);
        }

        auto directory = std::unique_ptr<BlockDirectory>(new (std::nothrow)
            BlockDirectory(block_count,
                           slot_count,
                           initial_state,
                           initial_generation,
                           std::move(slots),
                           std::move(publication_observer)));
        if (!directory) {
            return CreateResult::failure(BlockDirectoryError::AllocationFailed);
        }
        return CreateResult::success(std::move(directory));
    } catch (...) {
        return CreateResult::failure(BlockDirectoryError::AllocationFailed);
    }
}

BlockDirectory::~BlockDirectory() {
    for (std::uint64_t index = 0; index < top_level_slot_count_; ++index) {
        delete slots_[static_cast<std::size_t>(index)].load(
            std::memory_order_relaxed);
    }
}

BlockLeaf* BlockDirectory::load_leaf(std::uint64_t leaf_index) const noexcept {
    return slots_[static_cast<std::size_t>(leaf_index)].load(
        std::memory_order_acquire);
}

BlockEntry* BlockDirectory::find(std::uint64_t block_index) noexcept {
    return const_cast<BlockEntry*>(
        static_cast<const BlockDirectory&>(*this).find(block_index));
}

const BlockEntry* BlockDirectory::find(
    std::uint64_t block_index) const noexcept {
    if (block_index >= block_count_) {
        return nullptr;
    }
    auto* leaf = load_leaf(block_leaf_index(block_index));
    if (leaf == nullptr) {
        return nullptr;
    }
    return &leaf->entries[static_cast<std::size_t>(
        block_entry_index(block_index))];
}

BlockDirectory::AccessResult BlockDirectory::get_or_create(
    std::uint64_t block_index) noexcept {
    if (block_index >= block_count_) {
        return AccessResult::failure(BlockDirectoryError::OutOfRange);
    }

    const auto leaf_index = block_leaf_index(block_index);
    auto* leaf = load_leaf(leaf_index);
    if (leaf == nullptr) {
        auto candidate = std::unique_ptr<BlockLeaf>(
            new (std::nothrow) BlockLeaf(initial_entry_));
        if (!candidate) {
            return AccessResult::failure(BlockDirectoryError::AllocationFailed);
        }

        BlockLeaf* expected = nullptr;
        if (publication_observer_) {
            publication_observer_->before_publish(leaf_index);
        }
        if (slots_[static_cast<std::size_t>(leaf_index)]
                .compare_exchange_strong(expected,
                                         candidate.get(),
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
            leaf = candidate.release();
            allocated_leaf_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            leaf = expected;
        }
        if (publication_observer_) {
            publication_observer_->after_publish(leaf_index);
        }
    }

    return AccessResult::success(&leaf->entries[static_cast<std::size_t>(
        block_entry_index(block_index))]);
}

} // namespace myrd
