#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "virtual_memory_arena.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <malloc.h>
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

Result<void> closed_read() {
    return Result<void>::failure(read_error(
        OperationErrorCode::session_closed,
        L"DynamicBlockStore::read",
        L"dynamic block store is closed"));
}

Result<void> read_invariant(std::wstring message) {
    return Result<void>::failure(read_error(
        OperationErrorCode::internal_invariant,
        L"DynamicBlockStore::read",
        std::move(message)));
}

Error mutation_invariant_error(std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        L"DynamicBlockStore::write/materialize",
        std::move(message),
    };
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

struct AlignedBlockDeleter final {
    void operator()(std::byte* address) const noexcept {
        _aligned_free(address);
    }
};

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

Result<void> DynamicBlockStore::read_one_segment(
    const OperationContext& context,
    std::uint64_t block_index,
    std::uint64_t block_offset_bytes,
    std::span<std::byte> destination,
    ReadMaterializationPolicy materialization_policy,
    std::uint64_t mutation_offset_bytes,
    std::span<const std::byte> mutation_source,
    bool* mutation_applied) {
    constexpr std::uint32_t kMaxGenerationRetries = 16;
    const bool mutation_requested =
        materialization_policy == ReadMaterializationPolicy::ApplyMutation;
    if (mutation_applied != nullptr) {
        *mutation_applied = false;
    }
    if (mutation_requested) {
        if (mutation_applied == nullptr || mutation_source.empty() ||
            mutation_offset_bytes > kBlockSizeBytes ||
            mutation_source.size() >
                kBlockSizeBytes - mutation_offset_bytes) {
            return Result<void>::failure(mutation_invariant_error(
                L"invalid private-buffer mutation request"));
        }
    } else if (mutation_applied != nullptr || !mutation_source.empty() ||
               mutation_offset_bytes != 0) {
        return read_invariant(L"unexpected mutation arguments on read");
    }

    for (;;) {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return live;
        }
        if (closing_requested()) {
            return closed_read();
        }

        auto* entry = directory_->find(block_index);
        if (entry == nullptr) {
            if (directory_->initial_state() != InitialBlockState::CleanZero) {
                auto created = directory_->get_or_create(block_index);
                if (!created.ok()) {
                    return read_invariant(
                        L"failed to materialize sparse block metadata");
                }
                entry = created.value();
            } else {
                if (mutation_requested) {
                    return Result<void>::success();
                }
                std::fill(destination.begin(), destination.end(), std::byte{0});
                return Result<void>::success();
            }
        }

        auto& stripe = stripes_->stripe_for(block_index);
        std::unique_lock block_lock(stripe.mutex);
        if (entry->state == BlockState::ZeroClean ||
            entry->state == BlockState::ZeroDirty) {
            block_lock.unlock();
            if (mutation_requested) {
                return Result<void>::success();
            }
            std::fill(destination.begin(), destination.end(), std::byte{0});
            return Result<void>::success();
        }

        if (entry->state == BlockState::ResidentClean ||
            entry->state == BlockState::ResidentDirty) {
            if (mutation_requested) {
                return Result<void>::success();
            }
            if (entry->pin_count ==
                    (std::numeric_limits<std::uint64_t>::max)() ||
                !arena_->is_block_committed(block_index)) {
                return read_invariant(
                    L"resident block pin or commit invariant failed");
            }
            const auto block_address = arena_->block_address(block_index);
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
                    std::terminate();
                }
            });
            block_lock.unlock();
            if (read_observer_) {
                read_observer_->after_resident_pin(block_index);
            }
            std::memcpy(destination.data(),
                        block_address.value() + block_offset_bytes,
                        destination.size());

            block_lock.lock();
            const bool resident_write_waiting_for_pins =
                entry->state == BlockState::Committing &&
                (entry->prior_state == BlockState::ResidentClean ||
                 entry->prior_state == BlockState::ResidentDirty);
            const bool pinned_state_is_valid =
                entry->pin_count != 0 &&
                (entry->state == BlockState::ResidentClean ||
                 entry->state == BlockState::ResidentDirty ||
                 resident_write_waiting_for_pins);
            if (!pinned_state_is_valid) {
                return read_invariant(L"resident block changed while pinned");
            }
            --entry->pin_count;
            pin_needs_release = false;
            const auto epoch = next_epoch(access_epoch_);
            if (epoch == 0) {
                stripe.condition.notify_all();
                return read_invariant(L"access epoch wrapped");
            }
            entry->last_access_epoch = epoch;
            entry->reference_bit = true;
            block_lock.unlock();
            stripe.condition.notify_all();
            return Result<void>::success();
        }

        if (entry->state == BlockState::Loading) {
            const auto observed_epoch = entry->transition_epoch;
            if (entry->load_waiter_count ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return read_invariant(L"load waiter count wrapped");
            }
            ++entry->load_waiter_count;
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            bool waiter_registered = true;
            ScopeExit unregister_waiter([&]() noexcept {
                try {
                    if (!waiter_registered) {
                        return;
                    }
                    if (!block_lock.owns_lock()) {
                        block_lock.lock();
                    }
                    if (entry->state == BlockState::Loading &&
                        entry->transition_epoch == observed_epoch &&
                        entry->load_waiter_count != 0) {
                        --entry->load_waiter_count;
                    } else if (
                        entry->completed_load_epoch == observed_epoch &&
                        entry->completed_load_waiters != 0) {
                        --entry->completed_load_waiters;
                    }
                    waiter_registered = false;
                    stripe_waiter_count_.fetch_sub(
                        1, std::memory_order_release);
                    stripe.condition.notify_all();
                } catch (...) {
                    std::terminate();
                }
            });
            auto notify_waiter = [&stripe]() noexcept {
                stripe.condition.notify_all();
            };
            std::stop_callback cancellation_callback(
                context.stop_token, notify_waiter);

            while (entry->state == BlockState::Loading &&
                   entry->transition_epoch == observed_epoch &&
                   !entry->read_through_ready &&
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

            if (closing_requested()) {
                return closed_read();
            }
            const auto waiter_live = check_operation_context(context);
            if (!waiter_live.ok()) {
                return waiter_live;
            }
            if (entry->completed_load_epoch == observed_epoch &&
                entry->completed_load_error_code != 0) {
                const Error error{
                    static_cast<ErrorDomain>(
                        entry->completed_load_error_domain),
                    entry->completed_load_error_code,
                    L"DynamicBlockStore::read/wait",
                    L"single-flight load owner failed",
                    entry->completed_load_native_code,
                };
                return Result<void>::failure(error);
            }
            if (entry->state == BlockState::Loading &&
                entry->transition_epoch == observed_epoch &&
                entry->read_through_ready) {
                if (mutation_requested) {
                    return Result<void>::failure(read_error(
                        OperationErrorCode::memory_hard_limit,
                        L"DynamicBlockStore::write/materialize",
                        L"hard limit prevents mutation materialization"));
                }
                const auto* read_through_buffer =
                    entry->read_through_buffer;
                if (read_through_buffer == nullptr) {
                    return read_invariant(
                        L"read-through result published without a buffer");
                }
                block_lock.unlock();
                std::memcpy(destination.data(),
                            read_through_buffer + block_offset_bytes,
                            destination.size());
                block_lock.lock();
                if (entry->state != BlockState::Loading ||
                    entry->transition_epoch != observed_epoch ||
                    !entry->read_through_ready ||
                    entry->read_through_buffer != read_through_buffer) {
                    return read_invariant(
                        L"read-through buffer lifetime ended while in use");
                }
                {
                    std::scoped_lock state_lock(state_mutex_);
                    ++stats_.uncached_reads_total;
                }
                return Result<void>::success();
            }
            block_lock.unlock();
            continue;
        }

        if (entry->state == BlockState::Committing ||
            entry->state == BlockState::Evicting) {
            const auto observed_state = entry->state;
            const auto observed_epoch = entry->transition_epoch;
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            ScopeExit remove_waiter_count([this]() noexcept {
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
                return closed_read();
            }
            const auto wait_live = check_operation_context(context);
            if (!wait_live.ok()) {
                return wait_live;
            }
            continue;
        }

        if (entry->state != BlockState::Unresident) {
            return unavailable_read_state(entry->state);
        }

        if (entry->completed_load_waiters != 0) {
            stripe_waiter_count_.fetch_add(1, std::memory_order_release);
            ScopeExit remove_waiter_count([this]() noexcept {
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
                return closed_read();
            }
            const auto wait_live = check_operation_context(context);
            if (!wait_live.ok()) {
                return wait_live;
            }
            continue;
        }

        std::uint64_t mutation_next_version = 0;
        if (mutation_requested) {
            const auto next_version = checked_increment_version(entry->version);
            if (!next_version.ok()) {
                return Result<void>::failure(mutation_invariant_error(
                    L"content version wrapped"));
            }
            mutation_next_version = next_version.value;
        }
        const auto owner_epoch = next_epoch(transition_epoch_);
        if (owner_epoch == 0) {
            return read_invariant(L"transition epoch wrapped");
        }
        entry->prior_state = BlockState::Unresident;
        entry->state = BlockState::Loading;
        entry->transition_owner = owner_epoch;
        entry->transition_epoch = owner_epoch;
        entry->load_epoch = owner_epoch;
        entry->load_waiter_count = 0;
        entry->completed_load_epoch = 0;
        entry->completed_load_waiters = 0;
        entry->read_through_buffer = nullptr;
        entry->read_through_generation = 0;
        entry->completed_load_error_code = 0;
        entry->completed_load_native_code = 0;
        entry->completed_load_error_domain = 0;
        entry->read_through_ready = false;
        {
            std::scoped_lock state_lock(state_mutex_);
            increment_state_gauge(stats_, BlockState::Loading);
        }
        block_lock.unlock();

        const auto finish_failure =
            [this, entry, &stripe, owner_epoch](
                Error error,
                bool release_budget,
                bool release_publication,
                bool commit_failure) -> Result<void> {
            const bool owner_scoped_failure =
                error.code == static_cast<std::uint32_t>(
                    OperationErrorCode::cancelled) ||
                error.code == static_cast<std::uint32_t>(
                    OperationErrorCode::timed_out);
            bool notify_view = false;
            {
                std::unique_lock failure_lock(stripe.mutex);
                if (entry->state != BlockState::Loading ||
                    entry->transition_epoch != owner_epoch ||
                    entry->transition_owner != owner_epoch) {
                    return read_invariant(
                        L"load owner changed before failure rollback");
                }
                std::scoped_lock state_lock(state_mutex_);
                if (release_budget) {
                    if (commit_reservation_bytes_ < kBlockSizeBytes) {
                        std::terminate();
                    }
                    commit_reservation_bytes_ -= kBlockSizeBytes;
                }
                if (release_publication) {
                    if (backing_publications_in_progress_ == 0) {
                        std::terminate();
                    }
                    --backing_publications_in_progress_;
                    notify_view = true;
                }
                if (commit_failure) {
                    ++stats_.commit_failures;
                }
                decrement_state_gauge(stats_, BlockState::Loading);
                entry->state = entry->prior_state;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                entry->completed_load_epoch = owner_epoch;
                entry->completed_load_waiters = entry->load_waiter_count;
                entry->load_waiter_count = 0;
                entry->read_through_buffer = nullptr;
                entry->read_through_generation = 0;
                entry->read_through_ready = false;
                entry->completed_load_error_code = owner_scoped_failure
                    ? 0
                    : error.code;
                entry->completed_load_native_code = owner_scoped_failure
                    ? 0
                    : error.native_code;
                entry->completed_load_error_domain = owner_scoped_failure
                    ? 0
                    : static_cast<std::uint8_t>(error.domain);
            }
            if (notify_view) {
                backing_view_condition_.notify_all();
            }
            stripe.condition.notify_all();
            return Result<void>::failure(std::move(error));
        };

        auto scratch = std::unique_ptr<std::byte, AlignedBlockDeleter>(
            static_cast<std::byte*>(_aligned_malloc(
                static_cast<std::size_t>(kBlockSizeBytes),
                static_cast<std::size_t>(kBlockSizeBytes))));
        if (!scratch) {
            return finish_failure(
                read_error(OperationErrorCode::commit_failed,
                           L"DynamicBlockStore::read/load-buffer",
                           L"failed to allocate private load buffer"),
                false,
                false,
                true);
        }
        const auto scratch_span = std::span<std::byte>(
            scratch.get(), static_cast<std::size_t>(kBlockSizeBytes));

        bool resident_published = false;
        for (std::uint32_t generation_attempt = 0;
             generation_attempt < kMaxGenerationRetries;
             ++generation_attempt) {
            if (closing_requested()) {
                return finish_failure(closed_read().error(), false, false, false);
            }
            const auto owner_live = check_operation_context(context);
            if (!owner_live.ok()) {
                return finish_failure(
                    owner_live.error(), false, false, false);
            }

            BackingView captured_view;
            {
                std::scoped_lock state_lock(state_mutex_);
                captured_view = backing_view_;
                ++stats_.backing_reads_total;
                stats_.backing_read_bytes_total += kBlockSizeBytes;
            }

            std::stop_source load_stop;
            auto request_load_stop = [&load_stop]() noexcept {
                load_stop.request_stop();
            };
            std::stop_callback request_callback(
                context.stop_token, request_load_stop);
            std::stop_callback close_callback(
                close_stop_source_.get_token(), request_load_stop);
            const OperationContext load_context{
                load_stop.get_token(), context.deadline};
            auto loaded = [&]() {
                try {
                    return captured_view.store->load_block(
                        load_context, block_index, scratch_span);
                } catch (...) {
                    return Result<BlockLoadResult>::failure(read_error(
                        OperationErrorCode::backing_read_failed,
                        L"DynamicBlockStore::read/backing",
                        L"backing load threw an exception"));
                }
            }();

            if (closing_requested()) {
                return finish_failure(closed_read().error(), false, false, false);
            }
            const auto post_load_live = check_operation_context(context);
            if (!post_load_live.ok()) {
                return finish_failure(
                    post_load_live.error(), false, false, false);
            }
            if (!loaded.ok()) {
                return finish_failure(
                    loaded.error(), false, false, false);
            }

            if (loaded.value().kind == BlockLoadKind::AllZero &&
                mutation_requested) {
                std::fill(scratch_span.begin(), scratch_span.end(), std::byte{0});
            }
            if (loaded.value().kind == BlockLoadKind::AllZero &&
                !mutation_requested) {
                {
                    std::unique_lock reserve_publish_lock(stripe.mutex);
                    if (entry->state != BlockState::Loading ||
                        entry->transition_epoch != owner_epoch ||
                        entry->transition_owner != owner_epoch) {
                        return read_invariant(
                            L"load owner changed before zero reservation");
                    }
                    std::scoped_lock state_lock(state_mutex_);
                    if (backing_view_.generation != captured_view.generation ||
                        backing_view_.store != captured_view.store) {
                        ++stats_.stale_backing_load_retries;
                        continue;
                    }
                    ++backing_publications_in_progress_;
                }
                std::fill(
                    destination.begin(), destination.end(), std::byte{0});
                {
                    std::unique_lock publish_lock(stripe.mutex);
                    if (entry->state != BlockState::Loading ||
                        entry->transition_epoch != owner_epoch ||
                        entry->transition_owner != owner_epoch) {
                        std::terminate();
                    }
                    std::scoped_lock state_lock(state_mutex_);
                    if (backing_view_.generation != captured_view.generation ||
                        backing_view_.store != captured_view.store ||
                        backing_publications_in_progress_ == 0) {
                        std::terminate();
                    }
                    --backing_publications_in_progress_;
                    decrement_state_gauge(stats_, BlockState::Loading);
                    increment_state_gauge(stats_, BlockState::ZeroClean);
                    entry->state = BlockState::ZeroClean;
                    entry->prior_state = BlockState::ZeroClean;
                    entry->transition_owner = 0;
                    entry->transition_epoch = 0;
                    entry->clean_generation = captured_view.generation;
                    entry->dirty_kind = DirtyRecordKind::Data;
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
                backing_view_condition_.notify_all();
                stripe.condition.notify_all();
                return Result<void>::success();
            }
            if (loaded.value().kind != BlockLoadKind::Data &&
                !(mutation_requested &&
                  loaded.value().kind == BlockLoadKind::AllZero)) {
                return finish_failure(
                    read_error(OperationErrorCode::internal_invariant,
                               L"DynamicBlockStore::read/backing",
                               L"backing returned an unknown load kind"),
                    false,
                    false,
                    false);
            }

            bool use_read_through = false;
            bool resident_required_but_unavailable = false;
            {
                std::unique_lock reserve_lock(stripe.mutex);
                if (entry->state != BlockState::Loading ||
                    entry->transition_epoch != owner_epoch ||
                    entry->transition_owner != owner_epoch) {
                    return read_invariant(
                        L"load owner changed before commit reservation");
                }
                std::scoped_lock state_lock(state_mutex_);
                if (backing_view_.generation != captured_view.generation ||
                    backing_view_.store != captured_view.store) {
                    ++stats_.stale_backing_load_retries;
                    continue;
                }
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
                    use_read_through = true;
                    ++backing_publications_in_progress_;
                    if (mutation_requested) {
                        resident_required_but_unavailable = true;
                    }
                } else {
                    commit_reservation_bytes_ += kBlockSizeBytes;
                    ++backing_publications_in_progress_;
                }
            }

            if (use_read_through) {
                if (!resident_required_but_unavailable) {
                    std::memcpy(destination.data(),
                                scratch.get() + block_offset_bytes,
                                destination.size());
                }
                bool read_through_pointer_published = false;
                ScopeExit protect_published_pointer([&]() noexcept {
                    if (read_through_pointer_published) {
                        std::terminate();
                    }
                });
                {
                    std::unique_lock publish_lock(stripe.mutex);
                    if (entry->state != BlockState::Loading ||
                        entry->transition_epoch != owner_epoch ||
                        entry->transition_owner != owner_epoch) {
                        std::terminate();
                    }
                    {
                        std::scoped_lock state_lock(state_mutex_);
                        if (backing_view_.generation !=
                                captured_view.generation ||
                            backing_view_.store != captured_view.store ||
                            backing_publications_in_progress_ == 0) {
                            std::terminate();
                        }
                        if (!resident_required_but_unavailable) {
                            ++stats_.uncached_reads_total;
                        }
                        entry->read_through_buffer = scratch.get();
                        entry->read_through_generation =
                            captured_view.generation;
                        entry->read_through_ready = true;
                        read_through_pointer_published = true;
                    }
                    stripe.condition.notify_all();
                    stripe.condition.wait(publish_lock, [&] {
                        return entry->load_waiter_count == 0;
                    });
                    std::scoped_lock final_state_lock(state_mutex_);
                    if (entry->state != BlockState::Loading ||
                        entry->transition_epoch != owner_epoch ||
                        entry->transition_owner != owner_epoch ||
                        !entry->read_through_ready ||
                        entry->read_through_buffer != scratch.get() ||
                        entry->read_through_generation !=
                            captured_view.generation ||
                        backing_view_.generation != captured_view.generation ||
                        backing_view_.store != captured_view.store ||
                        backing_publications_in_progress_ == 0) {
                        std::terminate();
                    }
                    --backing_publications_in_progress_;
                    decrement_state_gauge(stats_, BlockState::Loading);
                    entry->state = BlockState::Unresident;
                    entry->prior_state = BlockState::Unresident;
                    entry->transition_owner = 0;
                    entry->transition_epoch = 0;
                    entry->load_waiter_count = 0;
                    entry->completed_load_epoch = 0;
                    entry->completed_load_waiters = 0;
                    entry->read_through_buffer = nullptr;
                    entry->read_through_generation = 0;
                    entry->completed_load_error_code = 0;
                    entry->completed_load_native_code = 0;
                    entry->completed_load_error_domain = 0;
                    entry->read_through_ready = false;
                    read_through_pointer_published = false;
                }
                backing_view_condition_.notify_all();
                stripe.condition.notify_all();
                if (resident_required_but_unavailable) {
                    return Result<void>::failure(read_error(
                        OperationErrorCode::memory_hard_limit,
                        L"DynamicBlockStore::write/materialize",
                        L"hard limit prevents mutation materialization"));
                }
                return Result<void>::success();
            }

            std::uint64_t mutation_dirty_epoch = 0;
            if (mutation_requested) {
                mutation_dirty_epoch = next_epoch(dirty_epoch_);
                if (mutation_dirty_epoch == 0) {
                    return finish_failure(
                        mutation_invariant_error(L"dirty epoch wrapped"),
                        true,
                        true,
                        false);
                }
                if (block_index + 1 == geometry_.total_blocks &&
                    geometry_.last_block_size_bytes != kBlockSizeBytes) {
                    std::fill(
                        scratch_span.begin() + static_cast<std::size_t>(
                            geometry_.last_block_size_bytes),
                        scratch_span.end(),
                        std::byte{0});
                }
                std::memcpy(
                    scratch.get() + mutation_offset_bytes,
                    mutation_source.data(),
                    mutation_source.size());
            }

            const auto committed = arena_->commit_block(block_index);
            if (!committed.ok()) {
                return finish_failure(
                    committed.error(), true, true, true);
            }
            std::memcpy(committed.value(), scratch.get(),
                        static_cast<std::size_t>(kBlockSizeBytes));

            {
                std::unique_lock publish_lock(stripe.mutex);
                if (entry->state != BlockState::Loading ||
                    entry->transition_epoch != owner_epoch ||
                    entry->transition_owner != owner_epoch) {
                    std::terminate();
                }
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
                decrement_state_gauge(stats_, BlockState::Loading);
                const auto published_state = mutation_requested
                    ? BlockState::ResidentDirty
                    : BlockState::ResidentClean;
                increment_state_gauge(stats_, published_state);
                entry->state = published_state;
                entry->prior_state = published_state;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                entry->dirty_kind = DirtyRecordKind::Data;
                if (mutation_requested) {
                    entry->version = mutation_next_version;
                    entry->dirty_epoch = mutation_dirty_epoch;
                    entry->dirty_base_generation = captured_view.generation;
                    entry->reference_bit = true;
                } else {
                    entry->clean_generation = captured_view.generation;
                }
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
            backing_view_condition_.notify_all();
            stripe.condition.notify_all();
            if (mutation_requested) {
                *mutation_applied = true;
                return Result<void>::success();
            }
            resident_published = true;
            break;
        }

        if (resident_published) {
            continue;
        }
        return finish_failure(
            read_error(OperationErrorCode::internal_invariant,
                       L"DynamicBlockStore::read/backing",
                       L"backing generation changed too many times"),
            false,
            false,
            false);
    }
}

Result<void> DynamicBlockStore::read(
    const OperationContext& context,
    std::uint64_t offset,
    std::span<std::byte> output) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return closed_read();
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
        const auto result = read_one_segment(
            context,
            segment.block_index,
            segment.block_offset_bytes,
            destination,
            ReadMaterializationPolicy::AllowReadThrough,
            0,
            {},
            nullptr);
        if (!result.ok()) {
            return result;
        }
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

bool DynamicBlockStoreTestAccess::set_version(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    std::uint64_t version) {
    auto* entry = store.directory_->find(block_index);
    if (entry == nullptr) {
        return false;
    }
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock lock(stripe.mutex);
    if (!is_stable_block_state(entry->state) || entry->pin_count != 0) {
        return false;
    }
    entry->version = version;
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

std::uint64_t DynamicBlockStoreTestAccess::commit_reservation_bytes(
    DynamicBlockStore& store) {
    std::scoped_lock state_lock(store.state_mutex_);
    return store.commit_reservation_bytes_;
}

} // namespace myrd
