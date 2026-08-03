#include "eviction_manager.h"

#include "block_directory.h"
#include "dynamic_block_store.h"
#include "dynamic_block_store_internal.h"
#include "virtual_memory_arena.h"
#include "src/common/operation_context.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

namespace myrd {
namespace {

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

void merge_report(EvictionPassResult& aggregate,
                  const EvictionPassResult& pass) noexcept {
    aggregate.scanned_blocks += pass.scanned_blocks;
    aggregate.cleared_reference_bits += pass.cleared_reference_bits;
    aggregate.evicted_blocks += pass.evicted_blocks;
    aggregate.decommit_failures += pass.decommit_failures;
    aggregate.skipped_dirty_blocks += pass.skipped_dirty_blocks;
    aggregate.skipped_zero_blocks += pass.skipped_zero_blocks;
    aggregate.skipped_transient_blocks += pass.skipped_transient_blocks;
    aggregate.skipped_pinned_blocks += pass.skipped_pinned_blocks;
    aggregate.skipped_generation_blocks += pass.skipped_generation_blocks;
    aggregate.invariant_failures += pass.invariant_failures;
    aggregate.target_reached =
        aggregate.target_reached || pass.target_reached;
    aggregate.closing_observed =
        aggregate.closing_observed || pass.closing_observed;
    aggregate.waited_for_scan =
        aggregate.waited_for_scan || pass.waited_for_scan;
    aggregate.request_interrupted =
        aggregate.request_interrupted || pass.request_interrupted;
}

bool contributes_resident_gauge(BlockState state) noexcept {
    return state == BlockState::ResidentClean ||
           state == BlockState::ResidentDirty;
}

void decrement_test_state_gauge(DynamicBlockStoreStats& stats,
                                BlockState state) noexcept {
    if (state == BlockState::ResidentClean) {
        --stats.resident_clean_blocks;
    } else if (state == BlockState::ResidentDirty) {
        --stats.resident_dirty_blocks;
    }
}

void increment_test_state_gauge(DynamicBlockStoreStats& stats,
                                BlockState state) noexcept {
    if (state == BlockState::ResidentClean) {
        ++stats.resident_clean_blocks;
    } else if (state == BlockState::ResidentDirty) {
        ++stats.resident_dirty_blocks;
    }
}

} // namespace

std::unique_ptr<EvictionManager> EvictionManager::create(
    DynamicBlockStore& store) noexcept {
    return std::unique_ptr<EvictionManager>(
        new (std::nothrow) EvictionManager(store));
}

bool EvictionManager::committed_at_or_below(
    std::uint64_t target_bytes) const {
    std::scoped_lock lock(store_.state_mutex_);
    return store_.committed_bytes_ <= target_bytes;
}

EvictionPassResult EvictionManager::sweep_once(
    std::uint64_t target_bytes) {
    std::scoped_lock scan_lock(scan_mutex_);
    return sweep_once_locked(target_bytes, nullptr);
}

EvictionPassResult EvictionManager::reclaim_to(
    std::uint64_t target_bytes,
    const OperationContext* context) {
    std::unique_lock<std::timed_mutex> scan_lock(
        scan_mutex_, std::defer_lock);
    EvictionPassResult aggregate{};

    if (context == nullptr) {
        scan_lock.lock();
    } else if (!scan_lock.try_lock()) {
        aggregate.waited_for_scan = true;
        synchronous_waiters_.fetch_add(1, std::memory_order_release);
        for (;;) {
            if (!check_operation_context(*context).ok()) {
                synchronous_waiters_.fetch_sub(
                    1, std::memory_order_release);
                aggregate.request_interrupted = true;
                return aggregate;
            }
            if (store_.closing_requested()) {
                synchronous_waiters_.fetch_sub(
                    1, std::memory_order_release);
                aggregate.closing_observed = true;
                return aggregate;
            }
            if (scan_lock.try_lock_for(std::chrono::milliseconds(1))) {
                synchronous_waiters_.fetch_sub(
                    1, std::memory_order_release);
                break;
            }
        }
    }

    // Two complete sparse sweeps are sufficient for CLOCK: the first clears
    // every observed reference bit, and the second can reclaim those blocks.
    for (std::uint32_t sweep = 0; sweep < 2; ++sweep) {
        auto pass = sweep_once_locked(target_bytes, context);
        merge_report(aggregate, pass);
        if (pass.target_reached || pass.closing_observed ||
            pass.request_interrupted) {
            break;
        }
        if (pass.cleared_reference_bits == 0) {
            // A complete sparse sweep found no second-chance block that could
            // become eligible on another immediate sweep. Retrying would spin.
            break;
        }
    }
    aggregate.target_reached = committed_at_or_below(target_bytes);
    return aggregate;
}

EvictionPassResult EvictionManager::sweep_once_locked(
    std::uint64_t target_bytes,
    const OperationContext* context) {
    EvictionPassResult report{};
    const auto block_count = store_.directory_->block_count();
    if (block_count == 0) {
        ++report.invariant_failures;
        return report;
    }
    if (committed_at_or_below(target_bytes)) {
        report.target_reached = true;
        return report;
    }

    const auto start = next_block_index_ % block_count;

    auto visit_range = [&](std::uint64_t first,
                           std::uint64_t last) -> bool {
        if (first >= last) {
            return false;
        }
        const auto first_leaf = block_leaf_index(first);
        const auto last_leaf = block_leaf_index(last - 1);
        for (auto leaf_index = first_leaf;
             leaf_index <= last_leaf;
             ++leaf_index) {
            if (context != nullptr &&
                !check_operation_context(*context).ok()) {
                report.request_interrupted = true;
                return true;
            }
            if (store_.closing_requested()) {
                report.closing_observed = true;
                return true;
            }
            const auto* leaf = store_.directory_->allocated_leaf_at(leaf_index);
            if (leaf == nullptr) {
                continue;
            }
            const auto leaf_first = leaf_index * kBlocksPerLeaf;
            const auto entry_first = (std::max)(first, leaf_first);
            const auto entry_last = (std::min)(
                last, (std::min)(block_count, leaf_first + kBlocksPerLeaf));
            for (auto block_index = entry_first;
                 block_index < entry_last;
                 ++block_index) {
                if (context != nullptr &&
                    !check_operation_context(*context).ok()) {
                    report.request_interrupted = true;
                    return true;
                }
                next_block_index_ = block_index + 1 == block_count
                    ? 0
                    : block_index + 1;
                ++report.scanned_blocks;

                auto& stripe = store_.stripes_->stripe_for(block_index);
                std::unique_lock block_lock(stripe.mutex);
                auto* entry = store_.directory_->find(block_index);
                if (entry == nullptr) {
                    ++report.invariant_failures;
                    continue;
                }

                switch (entry->state) {
                case BlockState::ResidentDirty:
                    ++report.skipped_dirty_blocks;
                    continue;
                case BlockState::ZeroClean:
                case BlockState::ZeroDirty:
                    ++report.skipped_zero_blocks;
                    continue;
                case BlockState::Loading:
                case BlockState::Committing:
                case BlockState::Evicting:
                    ++report.skipped_transient_blocks;
                    continue;
                case BlockState::Unresident:
                    continue;
                case BlockState::ResidentClean:
                    break;
                }

                if (entry->pin_count != 0) {
                    ++report.skipped_pinned_blocks;
                    continue;
                }

                bool metadata_accounting_valid = false;
                {
                    std::scoped_lock state_lock(store_.state_mutex_);
                    if (entry->clean_generation >
                        store_.backing_view_.generation) {
                        ++report.skipped_generation_blocks;
                        ++report.invariant_failures;
                        continue;
                    }
                    metadata_accounting_valid =
                        store_.stats_.resident_clean_blocks != 0 &&
                        store_.committed_bytes_ >= kBlockSizeBytes &&
                        store_.stats_.committed_bytes >= kBlockSizeBytes;
                }
                if (!metadata_accounting_valid ||
                    !store_.arena_->is_block_committed(block_index)) {
                    ++report.invariant_failures;
                    continue;
                }
                if (entry->reference_bit) {
                    entry->reference_bit = false;
                    ++report.cleared_reference_bits;
                    continue;
                }

                const auto owner = next_epoch(store_.transition_epoch_);
                if (owner == 0) {
                    ++report.invariant_failures;
                    continue;
                }
                const auto original = *entry;
                {
                    std::scoped_lock state_lock(store_.state_mutex_);
                    if (store_.stats_.resident_clean_blocks == 0) {
                        ++report.invariant_failures;
                        continue;
                    }
                    --store_.stats_.resident_clean_blocks;
                }
                entry->state = BlockState::Evicting;
                entry->prior_state = BlockState::ResidentClean;
                entry->transition_owner = owner;
                entry->transition_epoch = owner;
                block_lock.unlock();

                const auto decommitted =
                    store_.arena_->decommit_block(block_index);

                block_lock.lock();
                if (entry->state != BlockState::Evicting ||
                    entry->transition_owner != owner ||
                    entry->transition_epoch != owner) {
                    std::terminate();
                }
                if (!decommitted.ok()) {
                    {
                        std::scoped_lock state_lock(store_.state_mutex_);
                        ++store_.stats_.resident_clean_blocks;
                        ++store_.stats_.decommit_failures;
                    }
                    *entry = original;
                    ++report.decommit_failures;
                    block_lock.unlock();
                    stripe.condition.notify_all();
                    continue;
                }

                {
                    std::scoped_lock state_lock(store_.state_mutex_);
                    if (store_.committed_bytes_ < kBlockSizeBytes ||
                        store_.stats_.committed_bytes < kBlockSizeBytes) {
                        std::terminate();
                    }
                    store_.committed_bytes_ -= kBlockSizeBytes;
                    store_.stats_.committed_bytes -= kBlockSizeBytes;
                    ++store_.stats_.evicted_blocks_total;
                }
                *entry = original;
                entry->state = BlockState::Unresident;
                entry->prior_state = BlockState::Unresident;
                entry->transition_owner = 0;
                entry->transition_epoch = 0;
                entry->pin_count = 0;
                entry->reference_bit = false;
                ++report.evicted_blocks;
                block_lock.unlock();
                stripe.condition.notify_all();

                if (committed_at_or_below(target_bytes)) {
                    report.target_reached = true;
                    return true;
                }
            }
        }
        return false;
    };

    if (visit_range(start, block_count)) {
        return report;
    }
    if (visit_range(0, start)) {
        return report;
    }
    report.target_reached = committed_at_or_below(target_bytes);
    return report;
}

std::uint64_t EvictionManager::cursor() const {
    std::scoped_lock lock(scan_mutex_);
    return next_block_index_;
}

std::uint64_t EvictionManager::synchronous_waiters() const noexcept {
    return synchronous_waiters_.load(std::memory_order_acquire);
}

EvictionPassResult DynamicBlockStoreTestAccess::run_eviction_sweep(
    DynamicBlockStore& store,
    std::uint64_t target_bytes) {
    return store.eviction_manager_->sweep_once(target_bytes);
}

EvictionPassResult DynamicBlockStoreTestAccess::reclaim_clean_to(
    DynamicBlockStore& store,
    std::uint64_t target_bytes) {
    return store.eviction_manager_->reclaim_to(target_bytes);
}

bool DynamicBlockStoreTestAccess::configure_eviction_entry(
    DynamicBlockStore& store,
    std::uint64_t block_index,
    BlockState state,
    std::uint64_t pin_count,
    bool reference_bit,
    BackingGeneration clean_generation) {
    if (block_index >= store.geometry_.total_blocks) {
        return false;
    }
    if (store.directory_->find(block_index) == nullptr) {
        return false;
    }
    auto& stripe = store.stripes_->stripe_for(block_index);
    std::scoped_lock block_lock(stripe.mutex);
    auto* entry = store.directory_->find(block_index);
    if (entry == nullptr ||
        !store.arena_->is_block_committed(block_index)) {
        return false;
    }
    if (!contributes_resident_gauge(entry->state)) {
        return false;
    }
    const auto old_state = entry->state;
    const auto transition_owner = is_transient_block_state(state)
        ? next_epoch(store.transition_epoch_)
        : 0;
    if (is_transient_block_state(state) && transition_owner == 0) {
        return false;
    }
    {
        std::scoped_lock state_lock(store.state_mutex_);
        decrement_test_state_gauge(store.stats_, entry->state);
        increment_test_state_gauge(store.stats_, state);
    }
    entry->state = state;
    entry->prior_state = is_transient_block_state(state)
        ? (is_stable_block_state(old_state)
            ? old_state
            : BlockState::ResidentClean)
        : state;
    entry->transition_owner = transition_owner;
    entry->transition_epoch = transition_owner;
    entry->pin_count = pin_count;
    entry->reference_bit = reference_bit;
    entry->clean_generation = clean_generation;
    return true;
}

std::uint64_t DynamicBlockStoreTestAccess::eviction_cursor(
    DynamicBlockStore& store) {
    return store.eviction_manager_->cursor();
}

std::uint64_t DynamicBlockStoreTestAccess::synchronous_eviction_waiters(
    DynamicBlockStore& store) noexcept {
    return store.eviction_manager_->synchronous_waiters();
}

bool DynamicBlockStoreTestAccess::signal_eviction_if_needed(
    DynamicBlockStore& store) {
    return store.signal_eviction_worker_if_needed();
}

bool DynamicBlockStoreTestAccess::signal_eviction_many_for_test(
    DynamicBlockStore& store,
    std::uint64_t request_count) {
    if (request_count == 0) {
        return false;
    }
    {
        std::scoped_lock lock(store.lifecycle_mutex_);
        if (store.lifecycle_state_ != DynamicBlockStore::LifecycleState::Open) {
            return false;
        }
        for (std::uint64_t index = 0; index < request_count; ++index) {
            store.eviction_requested_ = true;
        }
    }
    store.lifecycle_condition_.notify_one();
    return true;
}

std::uint64_t DynamicBlockStoreTestAccess::eviction_worker_passes(
    DynamicBlockStore& store) {
    std::scoped_lock lock(store.lifecycle_mutex_);
    return store.eviction_worker_passes_;
}

std::uint64_t DynamicBlockStoreTestAccess::eviction_worker_wakeups(
    DynamicBlockStore& store) {
    std::scoped_lock lock(store.lifecycle_mutex_);
    return store.eviction_worker_wakeups_;
}

bool DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
    DynamicBlockStore& store,
    std::uint64_t minimum_passes,
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(store.lifecycle_mutex_);
    return store.lifecycle_condition_.wait_until(lock, deadline, [&] {
        return store.lifecycle_state_ !=
                   DynamicBlockStore::LifecycleState::Open ||
               (store.eviction_worker_waiting_ &&
                !store.eviction_requested_ &&
                store.eviction_worker_passes_ >= minimum_passes);
    });
}

} // namespace myrd
