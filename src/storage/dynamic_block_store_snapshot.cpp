#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
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

} // namespace myrd
