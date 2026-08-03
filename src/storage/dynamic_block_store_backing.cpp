#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "src/common/scope_exit.h"

#include <limits>
#include <string>
#include <utility>

namespace myrd {
namespace {

Error backing_error(std::uint32_t code,
                    std::wstring operation,
                    std::wstring message) {
    return Error{
        ErrorDomain::storage,
        code,
        std::move(operation),
        std::move(message),
    };
}

Error closed_backing(std::wstring operation) {
    return backing_error(
        static_cast<std::uint32_t>(OperationErrorCode::session_closed),
        std::move(operation),
        L"dynamic block store is closed");
}

Error generation_conflict(std::wstring operation, std::wstring message) {
    return backing_error(
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::BackingGenerationConflict),
        std::move(operation),
        std::move(message));
}

Error version_conflict(std::wstring message) {
    return backing_error(
        static_cast<std::uint32_t>(OperationErrorCode::version_changed),
        L"DynamicBlockStore::mark_persisted",
        std::move(message));
}

} // namespace

BackingGeneration DynamicBlockStore::backing_generation() const {
    std::scoped_lock lock(state_mutex_);
    return backing_view_.generation;
}

Result<void> DynamicBlockStore::install_backing_view(
    BackingView next_view) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(closed_backing(
            L"DynamicBlockStore::install_backing_view"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    BackingView retired_view;
    {
        std::unique_lock lock(state_mutex_);
        backing_install_waiter_count_.fetch_add(
            1, std::memory_order_release);
        ScopeExit remove_waiter([this]() noexcept {
            backing_install_waiter_count_.fetch_sub(
                1, std::memory_order_release);
        });
        backing_view_condition_.wait(lock, [this] {
            return backing_publications_in_progress_ == 0 ||
                   closing_requested();
        });
        if (closing_requested()) {
            return Result<void>::failure(closed_backing(
                L"DynamicBlockStore::install_backing_view"));
        }
        if (!next_view.store ||
            next_view.generation <= backing_view_.generation) {
            ++stats_.backing_generation_conflicts;
            return Result<void>::failure(generation_conflict(
                L"DynamicBlockStore::install_backing_view",
                L"backing view must be non-null and strictly newer"));
        }

        retired_view = std::move(backing_view_);
        backing_view_ = std::move(next_view);
        stats_.backing_generation = backing_view_.generation;
        ++stats_.backing_view_installs_total;
    }
    // shared_ptr destruction may execute a user-provided backing destructor.
    // Keep it outside state_mutex_ so reentrant diagnostics cannot deadlock.
    retired_view.store.reset();
    return Result<void>::success();
}

Result<void> DynamicBlockStore::mark_persisted(
    std::uint64_t block_index,
    std::uint64_t expected_version,
    DirtyRecordKind expected_kind,
    BackingGeneration persisted_generation) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(closed_backing(
            L"DynamicBlockStore::mark_persisted"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    if (block_index >= geometry_.total_blocks) {
        return Result<void>::failure(backing_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"DynamicBlockStore::mark_persisted",
            L"block index is outside the logical device"));
    }

    auto* entry = directory_->find(block_index);
    if (entry == nullptr) {
        std::scoped_lock state_lock(state_mutex_);
        ++stats_.snapshot_version_conflicts;
        return Result<void>::failure(version_conflict(
            L"dirty block metadata no longer exists"));
    }

    auto& stripe = stripes_->stripe_for(block_index);
    std::unique_lock block_lock(stripe.mutex);
    while ((is_transient_block_state(entry->state) ||
            entry->pin_count != 0) &&
           !closing_requested()) {
        stripe_waiter_count_.fetch_add(1, std::memory_order_release);
        ScopeExit remove_waiter([this]() noexcept {
            stripe_waiter_count_.fetch_sub(1, std::memory_order_release);
        });
        const auto observed_state = entry->state;
        const auto observed_epoch = entry->transition_epoch;
        const auto observed_pin_count = entry->pin_count;
        stripe.condition.wait(block_lock, [&] {
            return entry->state != observed_state ||
                   entry->transition_epoch != observed_epoch ||
                   entry->pin_count != observed_pin_count ||
                   closing_requested();
        });
    }
    if (closing_requested()) {
        return Result<void>::failure(closed_backing(
            L"DynamicBlockStore::mark_persisted"));
    }

    const bool resident_dirty = entry->state == BlockState::ResidentDirty;
    const bool zero_dirty = entry->state == BlockState::ZeroDirty;
    if ((!resident_dirty && !zero_dirty) ||
        entry->version != expected_version ||
        entry->dirty_kind != expected_kind) {
        std::scoped_lock state_lock(state_mutex_);
        ++stats_.snapshot_version_conflicts;
        return Result<void>::failure(version_conflict(
            L"dirty state, version, or record kind changed"));
    }

    {
        std::scoped_lock state_lock(state_mutex_);
        if (persisted_generation <= entry->dirty_base_generation ||
            persisted_generation > backing_view_.generation) {
            ++stats_.backing_generation_conflicts;
            return Result<void>::failure(generation_conflict(
                L"DynamicBlockStore::mark_persisted",
                L"persisted generation is not an installed successor"));
        }

        if (resident_dirty) {
            --stats_.resident_dirty_blocks;
            ++stats_.resident_clean_blocks;
            entry->state = BlockState::ResidentClean;
            entry->prior_state = BlockState::ResidentClean;
        } else {
            --stats_.zero_dirty_blocks;
            ++stats_.zero_clean_blocks;
            entry->state = BlockState::ZeroClean;
            entry->prior_state = BlockState::ZeroClean;
        }
        entry->clean_generation = persisted_generation;
    }
    block_lock.unlock();
    stripe.condition.notify_all();
    if (resident_dirty) {
        // Pressure can previously have had no victim because every committed
        // block was dirty. A successful persisted acknowledgement creates a
        // newly eligible clean victim without committing another block.
        signal_eviction_worker_if_needed();
    }
    return Result<void>::success();
}

bool DynamicBlockStoreTestAccess::begin_backing_publication(
    DynamicBlockStore& store) {
    std::scoped_lock state_lock(store.state_mutex_);
    if (store.backing_publications_in_progress_ ==
        (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    ++store.backing_publications_in_progress_;
    return true;
}

bool DynamicBlockStoreTestAccess::end_backing_publication(
    DynamicBlockStore& store) {
    {
        std::scoped_lock state_lock(store.state_mutex_);
        if (store.backing_publications_in_progress_ == 0) {
            return false;
        }
        --store.backing_publications_in_progress_;
    }
    store.backing_view_condition_.notify_all();
    return true;
}

} // namespace myrd
