#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "virtual_memory_arena.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

namespace myrd {
namespace {

Error write_error(OperationErrorCode code,
                  std::wstring operation,
                  std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message),
    };
}

Result<void> invalid_write_range(BlockMathError error) {
    std::wstring message = L"invalid write range: ";
    const auto name = to_string(error);
    message.append(name.begin(), name.end());
    return Result<void>::failure(write_error(
        OperationErrorCode::invalid_range,
        L"DynamicBlockStore::write",
        std::move(message)));
}

Result<void> closed_write() {
    return Result<void>::failure(write_error(
        OperationErrorCode::session_closed,
        L"DynamicBlockStore::write",
        L"dynamic block store is closed"));
}

Result<void> write_invariant(std::wstring message) {
    return Result<void>::failure(Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        L"DynamicBlockStore::write",
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

Result<void> DynamicBlockStore::write_one_segment(
    const OperationContext& context,
    std::uint64_t block_index,
    std::uint64_t block_offset_bytes,
    std::span<const std::byte> source,
    bool explicit_zero_is_noop) {
    for (;;) {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return live;
        }
        if (closing_requested()) {
            return closed_write();
        }

        auto access = directory_->get_or_create(block_index);
        if (!access.ok()) {
            return write_invariant(
                L"failed to materialize sparse block metadata");
        }
        auto* entry = access.value();
        auto& stripe = stripes_->stripe_for(block_index);
        std::unique_lock block_lock(stripe.mutex);

        const auto block_start = block_index * kBlockSizeBytes;
        const auto remaining = geometry_.logical_size_bytes - block_start;
        const auto logical_block_size =
            (std::min)(remaining, kBlockSizeBytes);
        const bool covers_logical_block =
            block_offset_bytes == 0 && source.size() == logical_block_size;

        if (entry->state == BlockState::Loading &&
            !covers_logical_block) {
            block_lock.unlock();
            std::byte ignored{};
            bool mutation_applied = false;
            const auto materialized = read_one_segment(
                context,
                block_index,
                0,
                std::span<std::byte>(&ignored, 1),
                ReadMaterializationPolicy::ApplyMutation,
                block_offset_bytes,
                source,
                &mutation_applied);
            if (!materialized.ok()) {
                return materialized;
            }
            if (mutation_applied) {
                return Result<void>::success();
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
            auto notify_waiter = [&stripe]() noexcept {
                stripe.condition.notify_all();
            };
            std::stop_callback cancellation_callback(
                context.stop_token, notify_waiter);
            while (entry->state == observed_state &&
                   entry->transition_epoch == observed_epoch &&
                   !closing_requested() &&
                   !context.stop_token.stop_requested() &&
                   std::chrono::steady_clock::now() < context.deadline) {
                if (context.deadline ==
                    std::chrono::steady_clock::time_point::max()) {
                    stripe.condition.wait(block_lock);
                } else {
                    stripe.condition.wait_until(block_lock, context.deadline);
                }
            }
            block_lock.unlock();
            if (closing_requested()) {
                return closed_write();
            }
            const auto wait_live = check_operation_context(context);
            if (!wait_live.ok()) {
                return wait_live;
            }
            continue;
        }

        if (entry->state == BlockState::Unresident &&
            entry->completed_load_waiters != 0) {
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            ScopeExit remove_waiter([this]() noexcept {
                stripe_waiter_count_.fetch_sub(1, std::memory_order_release);
            });
            auto notify_waiter = [&stripe]() noexcept {
                stripe.condition.notify_all();
            };
            std::stop_callback cancellation_callback(
                context.stop_token, notify_waiter);
            while (entry->completed_load_waiters != 0 &&
                   !closing_requested() &&
                   !context.stop_token.stop_requested() &&
                   std::chrono::steady_clock::now() < context.deadline) {
                if (context.deadline ==
                    std::chrono::steady_clock::time_point::max()) {
                    stripe.condition.wait(block_lock);
                } else {
                    stripe.condition.wait_until(block_lock, context.deadline);
                }
            }
            block_lock.unlock();
            if (closing_requested()) {
                return closed_write();
            }
            const auto wait_live = check_operation_context(context);
            if (!wait_live.ok()) {
                return wait_live;
            }
            continue;
        }

        if (explicit_zero_is_noop &&
            (entry->state == BlockState::ZeroClean ||
             entry->state == BlockState::ZeroDirty)) {
            return Result<void>::success();
        }

        if (entry->state == BlockState::Unresident &&
            !covers_logical_block) {
            block_lock.unlock();
            std::byte ignored{};
            bool mutation_applied = false;
            const auto materialized = read_one_segment(
                context,
                block_index,
                0,
                std::span<std::byte>(&ignored, 1),
                ReadMaterializationPolicy::ApplyMutation,
                block_offset_bytes,
                source,
                &mutation_applied);
            if (!materialized.ok()) {
                return materialized;
            }
            if (mutation_applied) {
                return Result<void>::success();
            }
            continue;
        }

        if (!is_stable_block_state(entry->state)) {
            return write_invariant(L"unknown stable block state");
        }
        const auto next_version = checked_increment_version(entry->version);
        if (!next_version.ok()) {
            return write_invariant(L"content version wrapped");
        }
        const auto transition_owner = next_epoch(transition_epoch_);
        if (transition_owner == 0) {
            return write_invariant(L"transition epoch wrapped");
        }
        const auto original_state = entry->state;

        if (original_state == BlockState::ResidentClean ||
            original_state == BlockState::ResidentDirty) {
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

            auto rollback = [&](Error error) -> Result<void> {
                {
                    std::scoped_lock state_lock(state_mutex_);
                    increment_state_gauge(stats_, original_state);
                }
                entry->state = original_state;
                entry->prior_state = original_state;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                transition_active = false;
                block_lock.unlock();
                stripe.condition.notify_all();
                return Result<void>::failure(std::move(error));
            };

            if (entry->pin_count != 0) {
                stripe_waiter_count_.fetch_add(1, std::memory_order_release);
                ScopeExit remove_waiter([this]() noexcept {
                    stripe_waiter_count_.fetch_sub(
                        1, std::memory_order_release);
                });
                auto notify_waiter = [&stripe]() noexcept {
                    stripe.condition.notify_all();
                };
                std::stop_callback cancellation_callback(
                    context.stop_token, notify_waiter);
                while (entry->pin_count != 0 &&
                       !closing_requested() &&
                       !context.stop_token.stop_requested() &&
                       std::chrono::steady_clock::now() < context.deadline) {
                    if (context.deadline ==
                        std::chrono::steady_clock::time_point::max()) {
                        stripe.condition.wait(block_lock);
                    } else {
                        stripe.condition.wait_until(
                            block_lock, context.deadline);
                    }
                }
            }
            if (closing_requested()) {
                return rollback(closed_write().error());
            }
            const auto after_pins_live = check_operation_context(context);
            if (!after_pins_live.ok()) {
                return rollback(after_pins_live.error());
            }
            if (entry->pin_count != 0) {
                return rollback(write_error(
                    OperationErrorCode::internal_invariant,
                    L"DynamicBlockStore::write/pins",
                    L"resident pins did not drain"));
            }
            const auto dirty_epoch = next_epoch(dirty_epoch_);
            if (dirty_epoch == 0) {
                return rollback(write_error(
                    OperationErrorCode::internal_invariant,
                    L"DynamicBlockStore::write/dirty-epoch",
                    L"dirty epoch wrapped"));
            }
            const auto address = arena_->block_address(block_index);
            if (!address.ok() || !arena_->is_block_committed(block_index)) {
                return rollback(write_error(
                    OperationErrorCode::internal_invariant,
                    L"DynamicBlockStore::write/resident",
                    L"resident block has no committed address"));
            }

            BackingView captured_view;
            {
                std::scoped_lock state_lock(state_mutex_);
                captured_view = backing_view_;
                ++backing_publications_in_progress_;
            }
            if (covers_logical_block &&
                logical_block_size != kBlockSizeBytes) {
                std::fill_n(address.value(),
                            static_cast<std::size_t>(kBlockSizeBytes),
                            std::byte{0});
            }
            std::memcpy(address.value() + block_offset_bytes,
                        source.data(),
                        source.size());
            {
                std::scoped_lock state_lock(state_mutex_);
                if (backing_view_.generation != captured_view.generation ||
                    backing_view_.store != captured_view.store ||
                    backing_publications_in_progress_ == 0) {
                    std::terminate();
                }
                --backing_publications_in_progress_;
                increment_state_gauge(stats_, BlockState::ResidentDirty);
                entry->state = BlockState::ResidentDirty;
                entry->prior_state = BlockState::ResidentDirty;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                entry->version = next_version.value;
                entry->dirty_epoch = dirty_epoch;
                entry->dirty_base_generation = captured_view.generation;
                entry->dirty_kind = DirtyRecordKind::Data;
                entry->reference_bit = true;
            }
            transition_active = false;
            block_lock.unlock();
            backing_view_condition_.notify_all();
            stripe.condition.notify_all();
            return Result<void>::success();
        }

        if (original_state != BlockState::Unresident &&
            original_state != BlockState::ZeroClean &&
            original_state != BlockState::ZeroDirty) {
            return write_invariant(L"write source state is unsupported");
        }
        if (original_state == BlockState::Unresident &&
            !covers_logical_block) {
            return write_invariant(
                L"partial unresident write was not materialized");
        }
        const auto dirty_epoch = next_epoch(dirty_epoch_);
        if (dirty_epoch == 0) {
            return write_invariant(L"dirty epoch wrapped");
        }

        BackingView captured_view;
        {
            std::scoped_lock state_lock(state_mutex_);
            const bool reservation_invalid =
                commit_reservation_bytes_ > options_.hard_limit_bytes ||
                stats_.committed_bytes >
                    options_.hard_limit_bytes - commit_reservation_bytes_;
            const auto accounted = reservation_invalid
                ? options_.hard_limit_bytes
                : stats_.committed_bytes + commit_reservation_bytes_;
            if (reservation_invalid ||
                options_.hard_limit_bytes - accounted < kBlockSizeBytes) {
                ++stats_.hard_limit_rejections;
                return Result<void>::failure(write_error(
                    OperationErrorCode::memory_hard_limit,
                    L"DynamicBlockStore::write/budget",
                    L"hard limit prevents a new committed block"));
            }
            commit_reservation_bytes_ += kBlockSizeBytes;
            ++backing_publications_in_progress_;
            captured_view = backing_view_;
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
        block_lock.unlock();

        const auto committed = arena_->commit_block(block_index);
        if (!committed.ok()) {
            block_lock.lock();
            {
                std::scoped_lock state_lock(state_mutex_);
                if (commit_reservation_bytes_ < kBlockSizeBytes ||
                    backing_publications_in_progress_ == 0) {
                    std::terminate();
                }
                commit_reservation_bytes_ -= kBlockSizeBytes;
                --backing_publications_in_progress_;
                ++stats_.commit_failures;
                increment_state_gauge(stats_, original_state);
            }
            entry->state = original_state;
            entry->prior_state = original_state;
            entry->transition_owner = 0;
            entry->transition_epoch = 0;
            transition_active = false;
            block_lock.unlock();
            backing_view_condition_.notify_all();
            stripe.condition.notify_all();
            return Result<void>::failure(committed.error());
        }

        if (!covers_logical_block || logical_block_size != kBlockSizeBytes) {
            std::fill_n(committed.value(),
                        static_cast<std::size_t>(kBlockSizeBytes),
                        std::byte{0});
        }
        std::memcpy(committed.value() + block_offset_bytes,
                    source.data(),
                    source.size());

        block_lock.lock();
        if (entry->state != BlockState::Committing ||
            entry->transition_epoch != transition_owner ||
            entry->transition_owner != transition_owner) {
            std::terminate();
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            if (backing_view_.generation != captured_view.generation ||
                backing_view_.store != captured_view.store ||
                commit_reservation_bytes_ < kBlockSizeBytes ||
                backing_publications_in_progress_ == 0) {
                std::terminate();
            }
            commit_reservation_bytes_ -= kBlockSizeBytes;
            --backing_publications_in_progress_;
            stats_.committed_bytes += kBlockSizeBytes;
            increment_state_gauge(stats_, BlockState::ResidentDirty);
            entry->state = BlockState::ResidentDirty;
            entry->prior_state = BlockState::ResidentDirty;
            entry->transition_owner = 0;
            entry->transition_epoch = 0;
            entry->version = next_version.value;
            entry->dirty_epoch = dirty_epoch;
            entry->dirty_base_generation = captured_view.generation;
            entry->dirty_kind = DirtyRecordKind::Data;
            entry->reference_bit = true;
            entry->load_waiter_count = 0;
            entry->completed_load_epoch = 0;
            entry->completed_load_waiters = 0;
            entry->read_through_buffer = nullptr;
            entry->read_through_generation = 0;
            entry->completed_load_error_code = 0;
            entry->completed_load_native_code = 0;
            entry->completed_load_error_domain = 0;
            entry->read_through_ready = false;
        }
        transition_active = false;
        block_lock.unlock();
        backing_view_condition_.notify_all();
        stripe.condition.notify_all();
        return Result<void>::success();
    }
}

Result<void> DynamicBlockStore::write(
    const OperationContext& context,
    std::uint64_t offset,
    std::span<const std::byte> input) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return closed_write();
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    const auto split = validate_and_split_block_range(
        geometry_.logical_size_bytes,
        sector_size_bytes_,
        offset,
        static_cast<std::uint64_t>(input.size()),
        static_cast<std::uint64_t>(input.size()));
    if (!split.ok()) {
        return invalid_write_range(split.error);
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
            return invalid_write_range(segment_result.error);
        }
        const auto segment = segment_result.value;
        const auto source = input.subspan(
            static_cast<std::size_t>(segment.buffer_offset_bytes),
            static_cast<std::size_t>(segment.length_bytes));
        const auto committed_before = stats().committed_bytes;
        const auto result = write_one_segment(
            context,
            segment.block_index,
            segment.block_offset_bytes,
            source,
            false);
        if (!result.ok()) {
            return result;
        }
        const auto committed_after = stats().committed_bytes;
        if (committed_before <= options_.soft_limit_bytes &&
            committed_after > options_.soft_limit_bytes) {
            signal_eviction_worker_if_needed();
        }
    }
    return Result<void>::success();
}

} // namespace myrd
