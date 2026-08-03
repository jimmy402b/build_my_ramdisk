#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "virtual_memory_arena.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <string>

namespace myrd {
namespace {

Error read_error(OperationErrorCode code,
                 std::wstring operation,
                 std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message),
    };
}

Result<void> invalid_read_range(BlockMathError error) {
    std::wstring message = L"invalid read range: ";
    const auto name = to_string(error);
    message.append(name.begin(), name.end());
    return Result<void>::failure(read_error(
        OperationErrorCode::invalid_range,
        L"DynamicBlockStore::read",
        std::move(message)));
}

Result<void> unavailable_read_state(BlockState state) {
    std::wstring message = L"read state is handled by a later M2 task: ";
    const auto name = to_string(state);
    message.append(name.begin(), name.end());
    return Result<void>::failure(read_error(
        OperationErrorCode::internal_invariant,
        L"DynamicBlockStore::read",
        std::move(message)));
}

void decrement_state_gauge(DynamicBlockStoreStats& stats,
                           BlockState state) noexcept {
    switch (state) {
    case BlockState::ResidentClean:
        --stats.resident_clean_blocks;
        break;
    case BlockState::ResidentDirty:
        --stats.resident_dirty_blocks;
        break;
    case BlockState::ZeroClean:
        --stats.zero_clean_blocks;
        break;
    case BlockState::ZeroDirty:
        --stats.zero_dirty_blocks;
        break;
    case BlockState::Loading:
        --stats.loading_blocks;
        break;
    case BlockState::Unresident:
    case BlockState::Committing:
    case BlockState::Evicting:
        break;
    }
}

void increment_state_gauge(DynamicBlockStoreStats& stats,
                           BlockState state) noexcept {
    switch (state) {
    case BlockState::ResidentClean:
        ++stats.resident_clean_blocks;
        break;
    case BlockState::ResidentDirty:
        ++stats.resident_dirty_blocks;
        break;
    case BlockState::ZeroClean:
        ++stats.zero_clean_blocks;
        break;
    case BlockState::ZeroDirty:
        ++stats.zero_dirty_blocks;
        break;
    case BlockState::Loading:
        ++stats.loading_blocks;
        break;
    case BlockState::Unresident:
    case BlockState::Committing:
    case BlockState::Evicting:
        break;
    }
}

} // namespace

Result<void> DynamicBlockStore::read(
    const OperationContext& context,
    std::uint64_t offset,
    std::span<std::byte> output) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(read_error(
            OperationErrorCode::session_closed,
            L"DynamicBlockStore::read",
            L"dynamic block store is closed"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    const auto split = validate_and_split_block_range(
        geometry_.logical_size_bytes,
        sector_size_bytes_,
        offset,
        static_cast<std::uint64_t>(output.size()),
        static_cast<std::uint64_t>(output.size()));
    if (!split.ok()) {
        return invalid_read_range(split.error);
    }

    for (std::uint64_t segment_index = 0;
         segment_index < split.segment_count;
         ++segment_index) {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return live;
        }

        const auto segment_result = split.segment_at(segment_index);
        if (!segment_result.ok()) {
            return invalid_read_range(segment_result.error);
        }
        const auto segment = segment_result.value;
        auto destination = output.subspan(
            static_cast<std::size_t>(segment.buffer_offset_bytes),
            static_cast<std::size_t>(segment.length_bytes));

        auto* entry = directory_->find(segment.block_index);
        if (entry == nullptr) {
            if (directory_->initial_state() != InitialBlockState::CleanZero) {
                return unavailable_read_state(BlockState::Unresident);
            }
            std::fill(destination.begin(), destination.end(), std::byte{0});
            if (read_observer_) {
                read_observer_->after_segment(segment.block_index);
            }
            continue;
        }

        auto& stripe = stripes_->stripe_for(segment.block_index);
        std::unique_lock block_lock(stripe.mutex);
        if (entry->state == BlockState::ZeroClean ||
            entry->state == BlockState::ZeroDirty) {
            block_lock.unlock();
            std::fill(destination.begin(), destination.end(), std::byte{0});
            if (read_observer_) {
                read_observer_->after_segment(segment.block_index);
            }
            continue;
        }

        if (entry->state != BlockState::ResidentClean &&
            entry->state != BlockState::ResidentDirty) {
            return unavailable_read_state(entry->state);
        }
        if (entry->pin_count ==
            (std::numeric_limits<std::uint64_t>::max)() ||
            !arena_->is_block_committed(segment.block_index)) {
            return Result<void>::failure(read_error(
                OperationErrorCode::internal_invariant,
                L"DynamicBlockStore::read",
                L"resident block pin or commit invariant failed"));
        }
        const auto block_address = arena_->block_address(segment.block_index);
        if (!block_address.ok()) {
            return Result<void>::failure(block_address.error());
        }

        ++entry->pin_count;
        bool pin_needs_release = true;
        ScopeExit release_pin([&]() noexcept {
            if (!pin_needs_release) {
                return;
            }
            try {
                if (!block_lock.owns_lock()) {
                    block_lock.lock();
                }
                if (entry->pin_count != 0) {
                    --entry->pin_count;
                }
                block_lock.unlock();
                stripe.condition.notify_all();
            } catch (...) {
                // A pin must never leak. If mutex recovery itself fails, the
                // store can no longer prove address lifetime, so fail fast.
                std::terminate();
            }
        });
        block_lock.unlock();
        if (read_observer_) {
            read_observer_->after_resident_pin(segment.block_index);
        }
        std::memcpy(destination.data(),
                    block_address.value() + segment.block_offset_bytes,
                    static_cast<std::size_t>(segment.length_bytes));

        block_lock.lock();
        const bool pinned_state_is_valid =
            entry->pin_count != 0 &&
            (entry->state == BlockState::ResidentClean ||
             entry->state == BlockState::ResidentDirty);
        if (!pinned_state_is_valid) {
            return Result<void>::failure(read_error(
                OperationErrorCode::internal_invariant,
                L"DynamicBlockStore::read",
                L"resident block changed while pinned"));
        }
        --entry->pin_count;
        pin_needs_release = false;
        auto epoch = access_epoch_.load(std::memory_order_relaxed);
        for (;;) {
            if (epoch == (std::numeric_limits<std::uint64_t>::max)()) {
                stripe.condition.notify_all();
                return Result<void>::failure(read_error(
                    OperationErrorCode::internal_invariant,
                    L"DynamicBlockStore::read",
                    L"access epoch wrapped"));
            }
            if (access_epoch_.compare_exchange_weak(
                    epoch,
                    epoch + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        entry->last_access_epoch = epoch + 1;
        entry->reference_bit = true;
        block_lock.unlock();
        stripe.condition.notify_all();

        if (read_observer_) {
            read_observer_->after_segment(segment.block_index);
        }
    }
    return Result<void>::success();
}

bool DynamicBlockStoreTestAccess::seed_zero(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    BlockState state,
    BackingGeneration generation) {
    if (block_index >= store.geometry_.total_blocks ||
        (state != BlockState::ZeroClean &&
         state != BlockState::ZeroDirty)) {
        return false;
    }
    auto access = store.directory_->get_or_create(block_index);
    if (!access.ok()) {
        return false;
    }
    auto* entry = access.value();
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock block_lock(stripe.mutex);
    if (entry->pin_count != 0 ||
        store.arena_->is_block_committed(block_index)) {
        return false;
    }
    {
        std::scoped_lock state_lock(store.state_mutex_);
        decrement_state_gauge(store.stats_, entry->state);
        increment_state_gauge(store.stats_, state);
    }
    BlockEntry replacement{};
    replacement.state = state;
    replacement.prior_state = state;
    replacement.dirty_kind = DirtyRecordKind::Zero;
    replacement.dirty_base_generation = generation;
    replacement.clean_generation = generation;
    *entry = replacement;
    return true;
}

bool DynamicBlockStoreTestAccess::seed_resident(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    BlockState state,
    std::span<const std::byte> data,
    BackingGeneration generation) {
    if (block_index >= store.geometry_.total_blocks ||
        data.size() != kBlockSizeBytes ||
        (state != BlockState::ResidentClean &&
         state != BlockState::ResidentDirty)) {
        return false;
    }
    auto access = store.directory_->get_or_create(block_index);
    if (!access.ok()) {
        return false;
    }
    auto* entry = access.value();
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock block_lock(stripe.mutex);
    if (entry->pin_count != 0 ||
        store.arena_->is_block_committed(block_index)) {
        return false;
    }
    const auto committed = store.arena_->commit_block(block_index);
    if (!committed.ok()) {
        return false;
    }
    std::memcpy(committed.value(), data.data(), data.size());
    {
        std::scoped_lock state_lock(store.state_mutex_);
        decrement_state_gauge(store.stats_, entry->state);
        increment_state_gauge(store.stats_, state);
        store.stats_.committed_bytes += kBlockSizeBytes;
    }
    BlockEntry replacement{};
    replacement.state = state;
    replacement.prior_state = state;
    replacement.dirty_kind = DirtyRecordKind::Data;
    replacement.dirty_base_generation = generation;
    replacement.clean_generation = generation;
    *entry = replacement;
    return true;
}

bool DynamicBlockStoreTestAccess::seed_uncommitted_resident(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    BlockState state,
    BackingGeneration generation) {
    if (block_index >= store.geometry_.total_blocks ||
        (state != BlockState::ResidentClean &&
         state != BlockState::ResidentDirty)) {
        return false;
    }
    auto access = store.directory_->get_or_create(block_index);
    if (!access.ok()) {
        return false;
    }
    auto* entry = access.value();
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock block_lock(stripe.mutex);
    if (entry->pin_count != 0 ||
        store.arena_->is_block_committed(block_index)) {
        return false;
    }
    {
        std::scoped_lock state_lock(store.state_mutex_);
        decrement_state_gauge(store.stats_, entry->state);
        increment_state_gauge(store.stats_, state);
    }
    BlockEntry replacement{};
    replacement.state = state;
    replacement.prior_state = state;
    replacement.dirty_kind = DirtyRecordKind::Data;
    replacement.dirty_base_generation = generation;
    replacement.clean_generation = generation;
    *entry = replacement;
    return true;
}

bool DynamicBlockStoreTestAccess::snapshot_entry(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    BlockEntry& output) {
    auto* entry = store.directory_->find(block_index);
    if (entry == nullptr) {
        return false;
    }
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock lock(stripe.mutex);
    output = *entry;
    return true;
}

bool DynamicBlockStoreTestAccess::try_decommit_resident_clean(
    DynamicBlockStore& store,
    std::uint64_t block_index) {
    auto* entry = store.directory_->find(block_index);
    if (entry == nullptr) {
        return false;
    }
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::unique_lock block_lock(stripe.mutex);
    if (entry->state != BlockState::ResidentClean || entry->pin_count != 0) {
        return false;
    }
    const auto original = *entry;
    entry->prior_state = BlockState::ResidentClean;
    entry->state = BlockState::Evicting;
    block_lock.unlock();

    const auto decommitted = store.arena_->decommit_block(block_index);
    block_lock.lock();
    if (!decommitted.ok()) {
        *entry = original;
        block_lock.unlock();
        stripe.condition.notify_all();
        return false;
    }
    {
        std::scoped_lock state_lock(store.state_mutex_);
        --store.stats_.resident_clean_blocks;
        store.stats_.committed_bytes -= kBlockSizeBytes;
    }
    BlockEntry replacement{};
    replacement.state = BlockState::Unresident;
    replacement.prior_state = BlockState::Unresident;
    replacement.clean_generation = original.clean_generation;
    *entry = replacement;
    block_lock.unlock();
    stripe.condition.notify_all();
    return true;
}

} // namespace myrd
