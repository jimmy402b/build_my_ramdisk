#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "snapshot_coordinator.h"
#include "virtual_memory_arena.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <string>

namespace myrd {
namespace {

Error range_error(OperationErrorCode code,
                  std::wstring operation,
                  std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message),
    };
}

Result<void> invalid_range(DirtyRecordKind kind, BlockMathError error) {
    std::wstring operation = kind == DirtyRecordKind::Discard
        ? L"DynamicBlockStore::unmap"
        : L"DynamicBlockStore::zero";
    std::wstring message = L"invalid range: ";
    const auto name = to_string(error);
    message.append(name.begin(), name.end());
    return Result<void>::failure(range_error(
        OperationErrorCode::invalid_range,
        std::move(operation),
        std::move(message)));
}

Result<void> closed_range(DirtyRecordKind kind) {
    return Result<void>::failure(range_error(
        OperationErrorCode::session_closed,
        kind == DirtyRecordKind::Discard
            ? L"DynamicBlockStore::unmap"
            : L"DynamicBlockStore::zero",
        L"dynamic block store is closed"));
}

Result<void> range_invariant(DirtyRecordKind kind,
                             std::wstring message) {
    return Result<void>::failure(Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        kind == DirtyRecordKind::Discard
            ? L"DynamicBlockStore::unmap"
            : L"DynamicBlockStore::zero",
        std::move(message),
    });
}

std::uint64_t next_epoch(std::atomic<std::uint64_t>& sequence) noexcept {
    auto current = sequence.load(std::memory_order_relaxed);
    for (;;) {
        if (current == (std::numeric_limits<std::uint64_t>::max)()) {
            return 0;
        }
        if (sequence.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current + 1;
        }
    }
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

Result<void> DynamicBlockStore::publish_full_zero_segment(
    const OperationContext& context,
    std::uint64_t block_index,
    DirtyRecordKind kind) {
    std::uint64_t capacity_grant_ticket = 0;
    ScopeExit release_capacity_grant([this, &capacity_grant_ticket]() noexcept {
        snapshot_coordinator_->release_capacity_grant(
            capacity_grant_ticket);
    });
    for (;;) {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return live;
        }
        if (closing_requested()) {
            return closed_range(kind);
        }

        auto access = directory_->get_or_create(block_index);
        if (!access.ok()) {
            return range_invariant(
                kind, L"failed to materialize sparse block metadata");
        }
        auto* entry = access.value();
        auto& stripe = stripes_->stripe_for(block_index);
        std::unique_lock block_lock(stripe.mutex);

        if (entry->snapshot_preparing_id != 0 ||
            entry->snapshot_reservation_id != 0) {
            if (entry->snapshot_preparing_id == 0 ||
                entry->snapshot_reservation_id == 0) {
                return range_invariant(
                    kind, L"snapshot Preparing marker identity diverged");
            }
            const auto waited = wait_for_snapshot_preparing(
                context,
                block_lock,
                entry->snapshot_preparing_id,
                entry->snapshot_reservation_id,
                block_index);
            block_lock.unlock();
            if (!waited.ok()) {
                return closing_requested() ? closed_range(kind) : waited;
            }
            continue;
        }

        if (is_transient_block_state(entry->state)) {
            const auto observed_state = entry->state;
            const auto observed_epoch = entry->transition_epoch;
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            ScopeExit remove_waiter([this]() noexcept {
                stripe_waiter_count_.fetch_sub(1, std::memory_order_release);
            });
            wait_for_interruptible_stripe_predicate(
                stripe.condition,
                block_lock,
                context.stop_token,
                context.deadline,
                [&]() noexcept {
                    return closing_requested() ||
                           entry->state != observed_state ||
                           entry->transition_epoch != observed_epoch;
                });
            block_lock.unlock();
            if (closing_requested()) {
                return closed_range(kind);
            }
            const auto wait_live = check_operation_context(context);
            if (!wait_live.ok()) {
                return wait_live;
            }
            continue;
        }

        if (!is_stable_block_state(entry->state)) {
            return range_invariant(kind, L"unknown stable block state");
        }
        const auto next_version = checked_increment_version(entry->version);
        if (!next_version.ok()) {
            return range_invariant(kind, L"content version wrapped");
        }
        const auto dirty_epoch = next_epoch(dirty_epoch_);
        if (dirty_epoch == 0) {
            return range_invariant(kind, L"dirty epoch wrapped");
        }
        const auto original_entry = *entry;
        const auto original_state = entry->state;

        if (original_state != BlockState::ResidentClean &&
            original_state != BlockState::ResidentDirty) {
            if (original_state == BlockState::ZeroDirty) {
                const auto preserved =
                    preserve_snapshot_preimage_before_mutation(
                        context,
                        block_index,
                        entry->version,
                        entry->dirty_kind,
                        entry->dirty_base_generation,
                        entry->dirty_epoch,
                        {},
                        capacity_grant_ticket,
                        block_lock,
                        stripe.condition);
                entry = directory_->find(block_index);
                if (entry == nullptr) {
                    std::terminate();
                }
                if (!preserved.ok()) {
                    return Result<void>::failure(preserved.error());
                }
                if (preserved.value() ==
                    SnapshotPreimagePreserveOutcome::CapacityUnavailable) {
                    const auto waited = wait_for_snapshot_capacity(
                        context,
                        block_lock,
                        block_index,
                        false,
                        capacity_grant_ticket);
                    block_lock.unlock();
                    stripe.condition.notify_all();
                    if (!waited.ok()) {
                        return waited;
                    }
                    continue;
                }
            }
            BackingGeneration generation{};
            {
                std::scoped_lock state_lock(state_mutex_);
                generation = backing_view_.generation;
                decrement_state_gauge(stats_, original_state);
                increment_state_gauge(stats_, BlockState::ZeroDirty);
                entry->state = BlockState::ZeroDirty;
                entry->prior_state = BlockState::ZeroDirty;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                entry->version = next_version.value;
                entry->dirty_epoch = dirty_epoch;
                entry->dirty_base_generation = generation;
                entry->dirty_kind = kind;
                entry->reference_bit = false;
            }
            block_lock.unlock();
            stripe.condition.notify_all();
            return Result<void>::success();
        }

        if (!arena_->is_block_committed(block_index)) {
            return range_invariant(
                kind, L"resident block has no committed memory");
        }
        if (original_state == BlockState::ResidentDirty &&
            entry->dirty_kind != DirtyRecordKind::Data) {
            return range_invariant(
                kind, L"resident dirty block has a non-DATA record kind");
        }
        const auto transition_owner = next_epoch(transition_epoch_);
        if (transition_owner == 0) {
            return range_invariant(kind, L"transition epoch wrapped");
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            decrement_state_gauge(stats_, original_state);
        }
        entry->state = BlockState::Committing;
        entry->prior_state = original_state;
        entry->transition_owner = transition_owner;
        entry->transition_epoch = transition_owner;
        bool transition_active = true;
        ScopeExit protect_transition([&]() noexcept {
            if (transition_active) {
                std::terminate();
            }
        });

        auto restore_before_decommit = [&](Error error) -> Result<void> {
            {
                std::scoped_lock state_lock(state_mutex_);
                increment_state_gauge(stats_, original_state);
            }
            *entry = original_entry;
            transition_active = false;
            block_lock.unlock();
            stripe.condition.notify_all();
            return Result<void>::failure(std::move(error));
        };

        if (entry->pin_count != 0) {
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            ScopeExit remove_waiter([this]() noexcept {
                stripe_waiter_count_.fetch_sub(1, std::memory_order_release);
            });
            wait_for_interruptible_stripe_predicate(
                stripe.condition,
                block_lock,
                context.stop_token,
                context.deadline,
                [&]() noexcept {
                    return closing_requested() || entry->pin_count == 0;
                });
        }
        if (closing_requested()) {
            return restore_before_decommit(closed_range(kind).error());
        }
        const auto after_pins_live = check_operation_context(context);
        if (!after_pins_live.ok()) {
            return restore_before_decommit(after_pins_live.error());
        }
        if (entry->pin_count != 0) {
            return restore_before_decommit(range_error(
                OperationErrorCode::internal_invariant,
                kind == DirtyRecordKind::Discard
                    ? L"DynamicBlockStore::unmap/pins"
                    : L"DynamicBlockStore::zero/pins",
                L"resident pins did not drain"));
        }

        if (original_state == BlockState::ResidentDirty) {
            const auto address = arena_->block_address(block_index);
            if (!address.ok() ||
                !arena_->is_block_committed(block_index)) {
                return restore_before_decommit(range_invariant(
                    kind, L"resident block has no committed address").error());
            }
            const auto preserved =
                preserve_snapshot_preimage_before_mutation(
                    context,
                    block_index,
                    original_entry.version,
                    original_entry.dirty_kind,
                    original_entry.dirty_base_generation,
                    original_entry.dirty_epoch,
                    std::span<const std::byte>(
                        address.value(),
                        static_cast<std::size_t>(kBlockSizeBytes)),
                    capacity_grant_ticket,
                    block_lock,
                    stripe.condition);
            entry = directory_->find(block_index);
            if (entry == nullptr) {
                std::terminate();
            }
            if (!preserved.ok()) {
                return restore_before_decommit(preserved.error());
            }
            if (preserved.value() ==
                SnapshotPreimagePreserveOutcome::CapacityUnavailable) {
                {
                    std::scoped_lock state_lock(state_mutex_);
                    increment_state_gauge(stats_, original_state);
                }
                *entry = original_entry;
                transition_active = false;
                stripe.condition.notify_all();
                const auto waited = wait_for_snapshot_capacity(
                    context,
                    block_lock,
                    block_index,
                    true,
                    capacity_grant_ticket);
                block_lock.unlock();
                stripe.condition.notify_all();
                if (!waited.ok()) {
                    return waited;
                }
                continue;
            }
        }

        BackingView captured_view;
        {
            std::scoped_lock state_lock(state_mutex_);
            captured_view = backing_view_;
            ++backing_publications_in_progress_;
        }
        block_lock.unlock();

        const auto decommitted = arena_->decommit_block(block_index);
        if (!decommitted.ok()) {
            block_lock.lock();
            if (entry->state != BlockState::Committing ||
                entry->transition_owner != transition_owner ||
                entry->transition_epoch != transition_owner) {
                std::terminate();
            }
            {
                std::scoped_lock state_lock(state_mutex_);
                if (backing_publications_in_progress_ == 0) {
                    std::terminate();
                }
                --backing_publications_in_progress_;
                ++stats_.decommit_failures;
                increment_state_gauge(stats_, original_state);
            }
            *entry = original_entry;
            transition_active = false;
            block_lock.unlock();
            backing_view_condition_.notify_all();
            stripe.condition.notify_all();
            return Result<void>::failure(decommitted.error());
        }

        block_lock.lock();
        if (entry->state != BlockState::Committing ||
            entry->transition_owner != transition_owner ||
            entry->transition_epoch != transition_owner ||
            entry->pin_count != 0) {
            std::terminate();
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            if (backing_view_.generation != captured_view.generation ||
                backing_view_.store != captured_view.store ||
                backing_publications_in_progress_ == 0 ||
                committed_bytes_ < kBlockSizeBytes ||
                stats_.committed_bytes < kBlockSizeBytes) {
                std::terminate();
            }
            --backing_publications_in_progress_;
            committed_bytes_ -= kBlockSizeBytes;
            stats_.committed_bytes -= kBlockSizeBytes;
            increment_state_gauge(stats_, BlockState::ZeroDirty);
            entry->state = BlockState::ZeroDirty;
            entry->prior_state = BlockState::ZeroDirty;
            entry->transition_owner = 0;
            entry->transition_epoch = 0;
            entry->version = next_version.value;
            entry->dirty_epoch = dirty_epoch;
            entry->dirty_base_generation = captured_view.generation;
            entry->dirty_kind = kind;
            entry->reference_bit = false;
        }
        transition_active = false;
        block_lock.unlock();
        backing_view_condition_.notify_all();
        stripe.condition.notify_all();
        return Result<void>::success();
    }
}

Result<void> DynamicBlockStore::apply_zero_ranges(
    const OperationContext& context,
    std::span<const DataRange> ranges,
    DirtyRecordKind kind) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return closed_range(kind);
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    const auto live = check_operation_context(context);
    if (!live.ok()) {
        return live;
    }

    for (const auto& range : ranges) {
        const auto split = validate_and_split_block_range(
            geometry_.logical_size_bytes,
            sector_size_bytes_,
            range.offset,
            range.length,
            range.length);
        if (!split.ok()) {
            return invalid_range(kind, split.error);
        }
    }

    MutationToken mutation;
    const auto admitted = enter_mutation(context, mutation);
    if (!admitted.ok()) {
        return admitted;
    }
    ScopeExit leave_mutation_scope(
        [this, &mutation]() noexcept { leave_mutation(mutation); });

    static constexpr std::array<std::byte, kBlockSizeBytes> zeros{};
    for (const auto& range : ranges) {
        const auto split = validate_and_split_block_range(
            geometry_.logical_size_bytes,
            sector_size_bytes_,
            range.offset,
            range.length,
            range.length);
        for (std::uint64_t segment_index = 0;
             segment_index < split.segment_count;
             ++segment_index) {
            const auto segment_result = split.segment_at(segment_index);
            if (!segment_result.ok()) {
                return invalid_range(kind, segment_result.error);
            }
            const auto segment = segment_result.value;
            const auto block_start = segment.block_index * kBlockSizeBytes;
            const auto logical_block_size = (std::min)(
                geometry_.logical_size_bytes - block_start,
                kBlockSizeBytes);
            const bool covers_logical_block =
                segment.block_offset_bytes == 0 &&
                segment.length_bytes == logical_block_size;
            Result<void> result = covers_logical_block
                ? publish_full_zero_segment(
                    context, segment.block_index, kind)
                : write_one_segment(
                    context,
                    segment.block_index,
                    segment.block_offset_bytes,
                    std::span<const std::byte>(zeros).first(
                        static_cast<std::size_t>(segment.length_bytes)),
                    true);
            if (!result.ok()) {
                return result;
            }
            {
                std::scoped_lock state_lock(state_mutex_);
                if (kind == DirtyRecordKind::Discard) {
                    stats_.unmap_bytes_total += segment.length_bytes;
                } else {
                    stats_.zero_bytes_total += segment.length_bytes;
                }
            }
        }
    }
    return Result<void>::success();
}

Result<void> DynamicBlockStore::unmap(
    const OperationContext& context,
    std::span<const DataRange> ranges) {
    return apply_zero_ranges(context, ranges, DirtyRecordKind::Discard);
}

Result<void> DynamicBlockStore::zero(
    const OperationContext& context,
    std::span<const DataRange> ranges) {
    return apply_zero_ranges(context, ranges, DirtyRecordKind::Zero);
}

} // namespace myrd
