#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "snapshot_coordinator.h"
#include "virtual_memory_arena.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stop_token>
#include <string>
#include <utility>

namespace myrd {
namespace {

Error snapshot_error(OperationErrorCode code, std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        L"DynamicBlockStore::copy_dirty_batch",
        std::move(message),
    };
}

Error snapshot_invariant(std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        L"DynamicBlockStore::copy_dirty_batch",
        std::move(message),
    };
}

Error consistent_snapshot_error(
    std::uint32_t code,
    std::wstring operation,
    std::wstring message) {
    return Error{
        ErrorDomain::storage,
        code,
        std::move(operation),
        std::move(message),
    };
}

Error consistent_snapshot_invariant(std::wstring message) {
    return consistent_snapshot_error(
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        L"DynamicBlockStore::copy_consistent_snapshot_batch",
        std::move(message));
}

Error consistent_snapshot_error(
    DynamicBlockStoreErrorCode code,
    std::wstring operation,
    std::wstring message) {
    return consistent_snapshot_error(
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message));
}

bool same_snapshot_token(
    const ConsistentSnapshotToken& left,
    const ConsistentSnapshotToken& right) noexcept {
    return left.snapshot_id == right.snapshot_id &&
        left.source_backing_generation == right.source_backing_generation &&
        left.cutoff_dirty_epoch == right.cutoff_dirty_epoch;
}

template <class T>
Result<T> snapshot_failure(Error error) {
    return Result<T>::failure(std::move(error));
}

} // namespace

Result<std::vector<DirtyBlockSnapshot>>
DynamicBlockStore::copy_dirty_batch(
    const OperationContext& context,
    DirtyScanCursor& cursor,
    std::size_t max_bytes) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            snapshot_error(OperationErrorCode::session_closed,
                           L"dynamic block store is closed"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            initial_live.error());
    }
    if (max_bytes < kBlockSizeBytes ||
        max_bytes % kBlockSizeBytes != 0 ||
        cursor.next_block_index > geometry_.total_blocks) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            snapshot_error(OperationErrorCode::invalid_range,
                           L"snapshot limit or cursor is invalid"));
    }

    const auto configured_bytes = static_cast<std::size_t>(
        (std::min)(options_.max_snapshot_batch_bytes,
                   static_cast<std::uint64_t>(
                       (std::numeric_limits<std::size_t>::max)())));
    const auto payload_limit = (std::min)(max_bytes, configured_bytes);
    const auto record_limit = options_.max_snapshot_batch_records;
    const auto original_cursor = cursor.next_block_index;
    auto scan_index = original_cursor;
    std::size_t payload_bytes = 0;
    std::vector<DirtyBlockSnapshot> records;

    try {
        while (scan_index < geometry_.total_blocks &&
               records.size() < record_limit) {
            const auto live = check_operation_context(context);
            if (!live.ok()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    live.error());
            }
            if (closing_requested()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    snapshot_error(OperationErrorCode::session_closed,
                                   L"dynamic block store is closing"));
            }

            const auto leaf_index = block_leaf_index(scan_index);
            if (directory_->allocated_leaf_at(leaf_index) == nullptr) {
                const auto next_leaf =
                    (leaf_index + 1) * kBlocksPerLeaf;
                scan_index = (std::min)(geometry_.total_blocks, next_leaf);
                continue;
            }

            auto* entry = directory_->find(scan_index);
            if (entry == nullptr) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    snapshot_invariant(
                        L"allocated leaf disappeared during snapshot scan"));
            }
            auto& stripe = stripes_->stripe_for(scan_index);
            std::vector<std::byte> payload;

            for (;;) {
                auto notify_waiter = [&stripe]() noexcept {
                    {
                        std::scoped_lock callback_lock(stripe.mutex);
                    }
                    stripe.condition.notify_all();
                };
                std::stop_callback cancellation_callback(
                    context.stop_token, notify_waiter);
                std::unique_lock block_lock(stripe.mutex);
                if (is_transient_block_state(entry->state)) {
                    const auto observed_state = entry->state;
                    const auto observed_epoch = entry->transition_epoch;
                    stripe_waiter_count_.fetch_add(
                        1, std::memory_order_release);
                    ScopeExit remove_waiter([this]() noexcept {
                        stripe_waiter_count_.fetch_sub(
                            1, std::memory_order_release);
                    });
                    while (entry->state == observed_state &&
                           entry->transition_epoch == observed_epoch &&
                           !closing_requested() &&
                           !context.stop_token.stop_requested() &&
                           std::chrono::steady_clock::now() <
                               context.deadline) {
                        if (snapshot_wait_pause_for_test_.load(
                                std::memory_order_acquire)) {
                            snapshot_wait_gap_reached_for_test_.store(
                                true, std::memory_order_release);
                            snapshot_wait_gap_reached_for_test_.notify_all();
                            snapshot_wait_pause_for_test_.wait(
                                true, std::memory_order_acquire);
                        }
                        if (context.deadline ==
                            std::chrono::steady_clock::time_point::max()) {
                            stripe.condition.wait(block_lock);
                        } else {
                            stripe.condition.wait_until(
                                block_lock, context.deadline);
                        }
                    }
                    block_lock.unlock();
                    if (closing_requested()) {
                        return snapshot_failure<
                            std::vector<DirtyBlockSnapshot>>(
                            snapshot_error(
                                OperationErrorCode::session_closed,
                                L"dynamic block store is closing"));
                    }
                    const auto wait_live = check_operation_context(context);
                    if (!wait_live.ok()) {
                        return snapshot_failure<
                            std::vector<DirtyBlockSnapshot>>(
                            wait_live.error());
                    }
                    continue;
                }

                if (!is_stable_block_state(entry->state)) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        snapshot_invariant(L"unknown block state"));
                }
                if (entry->state != BlockState::ResidentDirty &&
                    entry->state != BlockState::ZeroDirty) {
                    ++scan_index;
                    break;
                }

                if (entry->state == BlockState::ZeroDirty) {
                    if (entry->dirty_kind != DirtyRecordKind::Zero &&
                        entry->dirty_kind != DirtyRecordKind::Discard) {
                        return snapshot_failure<
                            std::vector<DirtyBlockSnapshot>>(
                            snapshot_invariant(
                                L"zero dirty block has invalid record kind"));
                    }
                    DirtyBlockSnapshot record{
                        scan_index,
                        entry->version,
                        entry->dirty_kind,
                        entry->dirty_base_generation,
                        {},
                    };
                    block_lock.unlock();
                    records.push_back(std::move(record));
                    ++scan_index;
                    break;
                }

                if (payload_limit - payload_bytes < kBlockSizeBytes) {
                    block_lock.unlock();
                    cursor.next_block_index = scan_index;
                    {
                        std::scoped_lock state_lock(state_mutex_);
                        stats_.snapshot_records_total += records.size();
                    }
                    return Result<std::vector<DirtyBlockSnapshot>>::success(
                        std::move(records));
                }
                if (entry->dirty_kind != DirtyRecordKind::Data) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        snapshot_invariant(
                            L"resident dirty block is not a data record"));
                }
                if (payload.empty()) {
                    block_lock.unlock();
                    if (fail_snapshot_allocation_) {
                        return snapshot_failure<
                            std::vector<DirtyBlockSnapshot>>(
                            snapshot_error(
                                OperationErrorCode::memory_hard_limit,
                                L"injected snapshot allocation failure"));
                    }
                    payload.resize(static_cast<std::size_t>(kBlockSizeBytes));
                    continue;
                }
                if (entry->pin_count ==
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        snapshot_invariant(L"snapshot pin count wrapped"));
                }
                const auto address = arena_->block_address(scan_index);
                if (!address.ok() ||
                    !arena_->is_block_committed(scan_index)) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        snapshot_invariant(
                            L"resident dirty block is not committed"));
                }

                const auto version = entry->version;
                const auto kind = entry->dirty_kind;
                const auto base_generation = entry->dirty_base_generation;
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
                if (snapshot_pin_pause_for_test_.load(
                        std::memory_order_acquire)) {
                    snapshot_pin_gap_reached_for_test_.store(
                        true, std::memory_order_release);
                    snapshot_pin_gap_reached_for_test_.notify_all();
                    snapshot_pin_pause_for_test_.wait(
                        true, std::memory_order_acquire);
                }
                std::memcpy(payload.data(), address.value(), payload.size());
                block_lock.lock();
                const bool writer_waiting_for_pin =
                    entry->state == BlockState::Committing &&
                    entry->prior_state == BlockState::ResidentDirty;
                if (entry->pin_count == 0 ||
                    (entry->state != BlockState::ResidentDirty &&
                     !writer_waiting_for_pin)) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        snapshot_invariant(
                            L"dirty block changed incompatibly while pinned"));
                }
                --entry->pin_count;
                pin_needs_release = false;
                block_lock.unlock();
                stripe.condition.notify_all();

                if (scan_index + 1 == geometry_.total_blocks &&
                    geometry_.last_block_size_bytes < kBlockSizeBytes) {
                    std::fill(
                        payload.begin() + static_cast<std::ptrdiff_t>(
                            geometry_.last_block_size_bytes),
                        payload.end(),
                        std::byte{0});
                }
                records.push_back(DirtyBlockSnapshot{
                    scan_index,
                    version,
                    kind,
                    base_generation,
                    std::move(payload),
                });
                payload_bytes += static_cast<std::size_t>(kBlockSizeBytes);
                ++scan_index;
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            snapshot_error(OperationErrorCode::memory_hard_limit,
                           L"failed to allocate snapshot batch"));
    } catch (...) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            snapshot_error(OperationErrorCode::internal_invariant,
                           L"snapshot batch construction failed"));
    }

    const auto final_live = check_operation_context(context);
    if (!final_live.ok()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            final_live.error());
    }
    if (closing_requested()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            snapshot_error(OperationErrorCode::session_closed,
                           L"dynamic block store is closing"));
    }
    cursor.next_block_index = scan_index;
    {
        std::scoped_lock state_lock(state_mutex_);
        stats_.snapshot_records_total += records.size();
    }
    return Result<std::vector<DirtyBlockSnapshot>>::success(
        std::move(records));
}

Result<std::vector<DirtyBlockSnapshot>>
DynamicBlockStore::copy_consistent_snapshot_batch(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    DirtyScanCursor& cursor,
    const std::size_t max_bytes) {
    constexpr auto operation =
        L"DynamicBlockStore::copy_consistent_snapshot_batch";
    RequestToken request;
    if (!try_enter_request(request)) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::session_closed),
                operation,
                L"dynamic block store is closed"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            initial_live.error());
    }
    if (max_bytes < kBlockSizeBytes ||
        max_bytes % kBlockSizeBytes != 0 ||
        cursor.next_block_index > geometry_.total_blocks) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::invalid_range),
                operation,
                L"snapshot byte limit or cursor is invalid"));
    }

    const auto coordinator = snapshot_coordinator_->snapshot();
    if (!coordinator.active_token.has_value() ||
        !same_snapshot_token(*coordinator.active_token, token)) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                operation,
                L"snapshot token does not own this store"));
    }
    if (cursor.next_block_index != coordinator.next_scan_block) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::invalid_range),
                operation,
                L"snapshot cursor does not equal the token frontier"));
    }
    if (coordinator.active_state == ConsistentSnapshotState::Aborted) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                operation,
                L"snapshot was aborted"));
    }
    const auto expected_frontier = coordinator.next_scan_block;

    const auto configured_bytes = static_cast<std::size_t>(
        (std::min)(options_.max_snapshot_batch_bytes,
                   static_cast<std::uint64_t>(
                       (std::numeric_limits<std::size_t>::max)())));
    const auto payload_limit = (std::min)(max_bytes, configured_bytes);
    const auto record_limit = options_.max_snapshot_batch_records;
    auto scan_index = cursor.next_block_index;
    std::size_t payload_bytes = 0;
    std::vector<DirtyBlockSnapshot> records;
    std::uint64_t cached_leaf_index =
        (std::numeric_limits<std::uint64_t>::max)();
    BlockLeaf* cached_leaf = nullptr;

    const auto pause_after_staging = [this]() noexcept {
        if (!consistent_snapshot_record_pause_for_test_.load(
                std::memory_order_acquire)) {
            return;
        }
        consistent_snapshot_records_staged_for_test_.fetch_add(
            1, std::memory_order_release);
        consistent_snapshot_records_staged_for_test_.notify_all();
        consistent_snapshot_record_pause_for_test_.wait(
            true, std::memory_order_acquire);
    };

    try {
        bool stop_after_safe_progress = false;
        while (scan_index < geometry_.total_blocks &&
               records.size() < record_limit &&
               !stop_after_safe_progress) {
            const auto live = check_operation_context(context);
            if (!live.ok()) {
                if (records.empty()) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(live.error());
                }
                break;
            }
            if (closing_requested()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_error(
                        static_cast<std::uint32_t>(
                            OperationErrorCode::session_closed),
                        operation,
                        L"dynamic block store is closing"));
            }

            const auto leaf_index = block_leaf_index(scan_index);
            const bool count_scan_visits =
                consistent_snapshot_scan_counting_for_test_.load(
                    std::memory_order_relaxed);
            if (cached_leaf == nullptr ||
                cached_leaf_index != leaf_index) {
                if (count_scan_visits) {
                    consistent_snapshot_leaf_slot_visits_for_test_.fetch_add(
                        1, std::memory_order_relaxed);
                }
                const auto next_leaf =
                    directory_->next_allocated_leaf_at_or_after(leaf_index);
                if (!next_leaf.has_value()) {
                    scan_index = geometry_.total_blocks;
                    continue;
                }
                cached_leaf_index = next_leaf->leaf_index;
                cached_leaf = next_leaf->leaf;
                if (cached_leaf_index != leaf_index) {
                    scan_index = (std::min)(
                        geometry_.total_blocks,
                        cached_leaf_index * kBlocksPerLeaf);
                    continue;
                }
            }

            auto* entry = &cached_leaf->entries[static_cast<std::size_t>(
                block_entry_index(scan_index))];
            if (count_scan_visits) {
                consistent_snapshot_entry_visits_for_test_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            auto& stripe = stripes_->stripe_for(scan_index);
            auto notify_waiter = [&stripe]() noexcept {
                {
                    std::scoped_lock callback_lock(stripe.mutex);
                }
                stripe.condition.notify_all();
            };
            std::stop_callback cancellation_callback(
                context.stop_token, notify_waiter);
            std::unique_lock block_lock(stripe.mutex);

            while (is_transient_block_state(entry->state) &&
                   !closing_requested()) {
                const auto observed_state = entry->state;
                const auto observed_epoch = entry->transition_epoch;
                stripe_waiter_count_.fetch_add(
                    1, std::memory_order_release);
                ScopeExit remove_waiter([this]() noexcept {
                    stripe_waiter_count_.fetch_sub(
                        1, std::memory_order_release);
                });
                while (entry->state == observed_state &&
                       entry->transition_epoch == observed_epoch &&
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
                if (closing_requested()) {
                    break;
                }
                const auto wait_live = check_operation_context(context);
                if (!wait_live.ok()) {
                    if (records.empty()) {
                        return snapshot_failure<
                            std::vector<DirtyBlockSnapshot>>(
                            wait_live.error());
                    }
                    stop_after_safe_progress = true;
                    break;
                }
            }
            if (closing_requested()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_error(
                        static_cast<std::uint32_t>(
                            OperationErrorCode::session_closed),
                        operation,
                        L"dynamic block store is closing"));
            }
            if (stop_after_safe_progress) {
                break;
            }
            if (!is_stable_block_state(entry->state)) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_invariant(L"unknown block state"));
            }

            const auto preimage_metadata =
                snapshot_coordinator_->peek_preimage_metadata(scan_index);
            if (preimage_metadata.has_value() &&
                preimage_metadata->kind == DirtyRecordKind::Data &&
                payload_limit - payload_bytes < kBlockSizeBytes) {
                break;
            }
            auto preimage = preimage_metadata.has_value()
                ? snapshot_coordinator_->peek_preimage(scan_index)
                : std::nullopt;
            if (preimage_metadata.has_value() && !preimage.has_value()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_error(
                        DynamicBlockStoreErrorCode::SnapshotBusy,
                        operation,
                        L"snapshot frontier changed while a pre-image was staged"));
            }
            if (preimage.has_value()) {
                if (!preimage->data.empty() &&
                    consistent_snapshot_payload_copy_counting_for_test_.load(
                        std::memory_order_relaxed)) {
                    consistent_snapshot_payload_copy_bytes_for_test_.fetch_add(
                        static_cast<std::uint64_t>(preimage->data.size()),
                        std::memory_order_relaxed);
                }
                const bool valid_kind =
                    preimage->kind == DirtyRecordKind::Data ||
                    preimage->kind == DirtyRecordKind::Zero ||
                    preimage->kind == DirtyRecordKind::Discard;
                if (preimage->block_index != scan_index ||
                    preimage->version == 0 ||
                    preimage->dirty_epoch == 0 ||
                    preimage->dirty_epoch > token.cutoff_dirty_epoch ||
                    preimage->base_generation >
                        token.source_backing_generation ||
                    !valid_kind ||
                    (preimage->kind == DirtyRecordKind::Data &&
                     preimage->data.size() != kBlockSizeBytes) ||
                    (preimage->kind != DirtyRecordKind::Data &&
                     !preimage->data.empty())) {
                    return snapshot_failure<
                        std::vector<DirtyBlockSnapshot>>(
                        consistent_snapshot_invariant(
                            L"snapshot pre-image has an invalid payload"));
                }
                DirtyBlockSnapshot record{
                    preimage->block_index,
                    preimage->version,
                    preimage->kind,
                    preimage->base_generation,
                    std::move(preimage->data),
                };
                if (record.kind == DirtyRecordKind::Data) {
                    payload_bytes += static_cast<std::size_t>(kBlockSizeBytes);
                }
                block_lock.unlock();
                records.push_back(std::move(record));
                ++scan_index;
                pause_after_staging();
                continue;
            }

            const bool resident_dirty =
                entry->state == BlockState::ResidentDirty;
            const bool zero_dirty = entry->state == BlockState::ZeroDirty;
            if (!resident_dirty && !zero_dirty) {
                block_lock.unlock();
                ++scan_index;
                continue;
            }
            if (entry->dirty_epoch == 0 ||
                entry->version == 0 ||
                entry->dirty_base_generation >
                    token.source_backing_generation ||
                (resident_dirty &&
                 entry->dirty_kind != DirtyRecordKind::Data) ||
                (zero_dirty &&
                 entry->dirty_kind != DirtyRecordKind::Zero &&
                 entry->dirty_kind != DirtyRecordKind::Discard)) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_invariant(
                        L"dirty block has an invalid snapshot identity"));
            }
            if (entry->dirty_epoch > token.cutoff_dirty_epoch) {
                block_lock.unlock();
                ++scan_index;
                continue;
            }
            if (zero_dirty) {
                DirtyBlockSnapshot record{
                    scan_index,
                    entry->version,
                    entry->dirty_kind,
                    entry->dirty_base_generation,
                    {},
                };
                block_lock.unlock();
                records.push_back(std::move(record));
                ++scan_index;
                pause_after_staging();
                continue;
            }

            if (payload_limit - payload_bytes < kBlockSizeBytes) {
                break;
            }
            if (entry->pin_count ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_invariant(
                        L"snapshot pin count wrapped"));
            }
            const auto address = arena_->block_address(scan_index);
            if (!address.ok() || !arena_->is_block_committed(scan_index)) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_invariant(
                        L"resident dirty block is not committed"));
            }
            std::vector<std::byte> payload(
                static_cast<std::size_t>(kBlockSizeBytes));
            const auto version = entry->version;
            const auto kind = entry->dirty_kind;
            const auto base_generation = entry->dirty_base_generation;
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
            std::memcpy(payload.data(), address.value(), payload.size());
            block_lock.lock();
            const bool writer_waiting_for_pin =
                entry->state == BlockState::Committing &&
                entry->prior_state == BlockState::ResidentDirty;
            if (entry->pin_count == 0 ||
                (entry->state != BlockState::ResidentDirty &&
                 !writer_waiting_for_pin)) {
                return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                    consistent_snapshot_invariant(
                        L"dirty block changed incompatibly while pinned"));
            }
            --entry->pin_count;
            pin_needs_release = false;
            block_lock.unlock();
            stripe.condition.notify_all();

            if (scan_index + 1 == geometry_.total_blocks &&
                geometry_.last_block_size_bytes < kBlockSizeBytes) {
                std::fill(
                    payload.begin() + static_cast<std::ptrdiff_t>(
                        geometry_.last_block_size_bytes),
                    payload.end(),
                    std::byte{0});
            }
            records.push_back(DirtyBlockSnapshot{
                scan_index,
                version,
                kind,
                base_generation,
                std::move(payload),
            });
            payload_bytes += static_cast<std::size_t>(kBlockSizeBytes);
            ++scan_index;
            pause_after_staging();
        }
    } catch (const std::bad_alloc&) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::memory_hard_limit),
                operation,
                L"failed to allocate a consistent snapshot batch"));
    } catch (...) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::internal_invariant),
                operation,
                L"consistent snapshot batch construction failed"));
    }

    if (records.empty()) {
        const auto final_live = check_operation_context(context);
        if (!final_live.ok()) {
            return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
                final_live.error());
        }
    }
    if (closing_requested()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::session_closed),
                operation,
                L"dynamic block store is closing"));
    }
    const auto advanced = snapshot_coordinator_->advance_scan_frontier(
        token, expected_frontier, scan_index, geometry_.total_blocks);
    if (!advanced.ok()) {
        return snapshot_failure<std::vector<DirtyBlockSnapshot>>(
            advanced.error());
    }
    cursor.next_block_index = scan_index;
    {
        std::scoped_lock state_lock(state_mutex_);
        ++stats_.snapshot_consistent_batches_total;
        stats_.snapshot_consistent_records_total += records.size();
    }
    return Result<std::vector<DirtyBlockSnapshot>>::success(
        std::move(records));
}

Result<void> DynamicBlockStore::finish_consistent_snapshot(
    const ConsistentSnapshotToken& token) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(consistent_snapshot_error(
            static_cast<std::uint32_t>(OperationErrorCode::session_closed),
            L"DynamicBlockStore::finish_consistent_snapshot",
            L"dynamic block store is closed"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });
    return snapshot_coordinator_->finish(token, geometry_.total_blocks);
}

Result<void> DynamicBlockStore::abort_consistent_snapshot(
    const ConsistentSnapshotToken& token) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(consistent_snapshot_error(
            static_cast<std::uint32_t>(OperationErrorCode::session_closed),
            L"DynamicBlockStore::abort_consistent_snapshot",
            L"dynamic block store is closed"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });
    return snapshot_coordinator_->abort(token);
}

Result<ConsistentSnapshotStatus>
DynamicBlockStore::consistent_snapshot_status(
    const ConsistentSnapshotToken& token) const {
    if (closing_requested()) {
        return Result<ConsistentSnapshotStatus>::failure(
            consistent_snapshot_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::session_closed),
                L"DynamicBlockStore::consistent_snapshot_status",
                L"dynamic block store is closed"));
    }
    return snapshot_coordinator_->status(token);
}

bool DynamicBlockStoreTestAccess::snapshot_wait_gap_reached(
    const DynamicBlockStore& store) noexcept {
    return store.snapshot_wait_gap_reached_for_test_.load(
        std::memory_order_acquire);
}

bool DynamicBlockStoreTestAccess::release_snapshot_wait_gap(
    DynamicBlockStore& store) noexcept {
    const bool paused = store.snapshot_wait_pause_for_test_.exchange(
        false, std::memory_order_acq_rel);
    store.snapshot_wait_pause_for_test_.notify_all();
    return paused;
}

bool DynamicBlockStoreTestAccess::snapshot_pin_gap_reached(
    const DynamicBlockStore& store) noexcept {
    return store.snapshot_pin_gap_reached_for_test_.load(
        std::memory_order_acquire);
}

bool DynamicBlockStoreTestAccess::release_snapshot_pin_gap(
    DynamicBlockStore& store) noexcept {
    const bool paused = store.snapshot_pin_pause_for_test_.exchange(
        false, std::memory_order_acq_rel);
    store.snapshot_pin_pause_for_test_.notify_all();
    return paused;
}

bool DynamicBlockStoreTestAccess::
pause_consistent_snapshot_after_first_record(
    DynamicBlockStore& store) noexcept {
    store.consistent_snapshot_records_staged_for_test_.store(
        0, std::memory_order_release);
    return !store.consistent_snapshot_record_pause_for_test_.exchange(
        true, std::memory_order_acq_rel);
}

bool DynamicBlockStoreTestAccess::consistent_snapshot_record_staged(
    const DynamicBlockStore& store) noexcept {
    return consistent_snapshot_records_staged(store) != 0;
}

std::uint64_t DynamicBlockStoreTestAccess::
consistent_snapshot_records_staged(
    const DynamicBlockStore& store) noexcept {
    return store.consistent_snapshot_records_staged_for_test_.load(
        std::memory_order_acquire);
}

bool DynamicBlockStoreTestAccess::
release_consistent_snapshot_record_pause(
    DynamicBlockStore& store) noexcept {
    const bool paused =
        store.consistent_snapshot_record_pause_for_test_.exchange(
            false, std::memory_order_acq_rel);
    store.consistent_snapshot_record_pause_for_test_.notify_all();
    return paused;
}

void DynamicBlockStoreTestAccess::reset_consistent_snapshot_scan_visits(
    DynamicBlockStore& store) noexcept {
    store.consistent_snapshot_entry_visits_for_test_.store(
        0, std::memory_order_relaxed);
    store.consistent_snapshot_leaf_slot_visits_for_test_.store(
        0, std::memory_order_relaxed);
    store.consistent_snapshot_scan_counting_for_test_.store(
        true, std::memory_order_relaxed);
}

std::uint64_t DynamicBlockStoreTestAccess::
consistent_snapshot_entry_visits(
    const DynamicBlockStore& store) noexcept {
    return store.consistent_snapshot_entry_visits_for_test_.load(
        std::memory_order_relaxed);
}

std::uint64_t DynamicBlockStoreTestAccess::
consistent_snapshot_leaf_slot_visits(
    const DynamicBlockStore& store) noexcept {
    return store.consistent_snapshot_leaf_slot_visits_for_test_.load(
        std::memory_order_relaxed);
}

void DynamicBlockStoreTestAccess::
reset_consistent_snapshot_payload_copy_bytes(
    DynamicBlockStore& store) noexcept {
    store.consistent_snapshot_payload_copy_bytes_for_test_.store(
        0, std::memory_order_relaxed);
    store.consistent_snapshot_payload_copy_counting_for_test_.store(
        true, std::memory_order_relaxed);
}

std::uint64_t DynamicBlockStoreTestAccess::
consistent_snapshot_payload_copy_bytes(
    const DynamicBlockStore& store) noexcept {
    return store.consistent_snapshot_payload_copy_bytes_for_test_.load(
        std::memory_order_relaxed);
}

std::uint64_t DynamicBlockStoreTestAccess::allocated_leaf_count(
    const DynamicBlockStore& store) noexcept {
    return store.directory_->allocated_leaf_count();
}

bool DynamicBlockStoreTestAccess::snapshot_preimage(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    SnapshotPreimage& output) {
    const auto preimage =
        store.snapshot_coordinator_->peek_preimage(block_index);
    if (!preimage.has_value()) {
        return false;
    }
    output = *preimage;
    return true;
}

bool DynamicBlockStoreTestAccess::snapshot_preimage_lookup_entered(
    const DynamicBlockStore& store) noexcept {
    return store.snapshot_coordinator_->preimage_lookup_entered_for_test();
}

bool DynamicBlockStoreTestAccess::advance_snapshot_frontier(
    DynamicBlockStore& store,
    const ConsistentSnapshotToken& token,
    std::uint64_t next_scan_block) {
    const auto expected =
        store.snapshot_coordinator_->snapshot().next_scan_block;
    return store.snapshot_coordinator_->advance_scan_frontier(
        token, expected, next_scan_block, store.geometry_.total_blocks).ok();
}

} // namespace myrd
