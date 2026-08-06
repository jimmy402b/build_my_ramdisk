#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "snapshot_coordinator.h"
#include "src/common/scope_exit.h"

#include <chrono>
#include <limits>
#include <new>
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

Error version_conflict(std::wstring operation, std::wstring message) {
    return backing_error(
        static_cast<std::uint32_t>(OperationErrorCode::version_changed),
        std::move(operation),
        std::move(message));
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

} // namespace

BackingGeneration DynamicBlockStore::backing_generation() const {
    std::scoped_lock lock(state_mutex_);
    return backing_view_.generation;
}

Result<void> DynamicBlockStore::install_backing_view(
    BackingView next_view) {
    return install_backing_view_impl(std::nullopt, std::move(next_view));
}

BackingView DynamicBlockStore::capture_backing_view() const {
    std::scoped_lock lock(state_mutex_);
    return backing_view_;
}

Result<std::vector<BackingView>>
DynamicBlockStore::capture_live_backing_views(
    const std::size_t max_view_count) const {
    std::scoped_lock lock(state_mutex_);
    try {
        std::erase_if(retired_backing_views_, [](const auto& retired) {
            return retired.store.expired();
        });
        const std::size_t current_count = backing_view_.store ? 1U : 0U;
        if (max_view_count == 0 ||
            retired_backing_views_.size() + current_count > max_view_count) {
            return Result<std::vector<BackingView>>::failure(backing_error(
                static_cast<std::uint32_t>(
                    DynamicBlockStoreErrorCode::SnapshotResourceExhausted),
                L"DynamicBlockStore::capture_live_backing_views",
                L"live backing view count exceeds the caller budget"));
        }
        std::vector<BackingView> views;
        views.reserve(retired_backing_views_.size() + current_count);
        if (backing_view_.store) views.push_back(backing_view_);
        for (const auto& retired : retired_backing_views_) {
            if (auto store = retired.store.lock()) {
                views.push_back({retired.generation, std::move(store)});
            }
        }
        return Result<std::vector<BackingView>>::success(std::move(views));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<BackingView>>::failure(backing_error(
            static_cast<std::uint32_t>(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted),
            L"DynamicBlockStore::capture_live_backing_views",
            L"allocating the live backing view snapshot failed"));
    }
}

Result<void> DynamicBlockStore::install_backing_view_for_snapshot(
    const ConsistentSnapshotToken& token,
    BackingView next_view) {
    return install_backing_view_impl(token, std::move(next_view));
}

Result<void> DynamicBlockStore::install_backing_view_impl(
    const std::optional<ConsistentSnapshotToken>& token,
    BackingView next_view) {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(closed_backing(
            L"DynamicBlockStore::install_backing_view"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    MutationToken snapshot_sensitive_operation;
    const auto admitted = enter_mutation(
        OperationContext{
            close_stop_source_.get_token(),
            std::chrono::steady_clock::time_point::max()},
        snapshot_sensitive_operation);
    if (!admitted.ok()) {
        return admitted;
    }
    ScopeExit leave_snapshot_sensitive_operation(
        [this, &snapshot_sensitive_operation]() noexcept {
            leave_mutation(snapshot_sensitive_operation);
        });
    SnapshotPublicationPin publication_pin;
    if (token.has_value()) {
        auto acquired =
            snapshot_coordinator_->acquire_publication_pin(*token);
        if (!acquired.ok()) {
            return Result<void>::failure(acquired.error());
        }
        publication_pin = acquired.move_value();
    } else if (snapshot_coordinator_->snapshot().active_token.has_value()) {
        return Result<void>::failure(backing_error(
            static_cast<std::uint32_t>(
                DynamicBlockStoreErrorCode::SnapshotBusy),
            L"DynamicBlockStore::install_backing_view",
            L"backing publication requires the active snapshot token"));
    }

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

        // Record the old shared object before publication. Every capture and
        // every in-flight read keeps that same shared object alive, so this
        // weak history is complete without requiring callers to register
        // their copies. Expired entries are removed without allocation.
        std::erase_if(retired_backing_views_, [](const auto& retired) {
            return retired.store.expired();
        });
        if (backing_view_.store) {
            try {
                retired_backing_views_.push_back(
                    {backing_view_.generation, backing_view_.store});
            } catch (const std::bad_alloc&) {
                return Result<void>::failure(backing_error(
                    static_cast<std::uint32_t>(
                        DynamicBlockStoreErrorCode::SnapshotResourceExhausted),
                    L"DynamicBlockStore::install_backing_view",
                    L"recording the retired backing lifetime failed"));
            }
        }

        retired_view = std::move(backing_view_);
        backing_view_ = std::move(next_view);
        stats_.backing_generation = backing_view_.generation;
        ++stats_.backing_view_installs_total;
    }
    // The backing pointer/generation swap is the publication side effect.
    // Release its snapshot pin before destroying a user-provided retired
    // backing object, whose destructor may perform reentrant diagnostics.
    publication_pin = SnapshotPublicationPin{};
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
    auto result = mark_persisted_impl(
        std::nullopt, block_index, expected_version, expected_kind,
        std::nullopt, std::nullopt, persisted_generation);
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

Result<void> DynamicBlockStore::mark_persisted_for_snapshot(
    const ConsistentSnapshotToken& token,
    const std::uint64_t block_index,
    const std::uint64_t expected_version,
    const DirtyRecordKind expected_kind,
    const BackingGeneration persisted_generation) {
    auto result = mark_persisted_impl(
        token, block_index, expected_version, expected_kind,
        std::nullopt, std::nullopt, persisted_generation);
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

Result<SnapshotMemberAck> DynamicBlockStore::acknowledge_snapshot_member(
    const ConsistentSnapshotToken& token,
    const std::uint64_t block_index,
    const std::uint64_t expected_version,
    const DirtyRecordKind expected_kind,
    const BackingGeneration expected_base_generation,
    const std::uint64_t expected_dirty_epoch,
    const BackingGeneration persisted_generation) {
    return mark_persisted_impl(
        token, block_index, expected_version, expected_kind,
        expected_base_generation, expected_dirty_epoch,
        persisted_generation);
}

Result<SnapshotMemberAck> DynamicBlockStore::mark_persisted_impl(
    const std::optional<ConsistentSnapshotToken>& token,
    std::uint64_t block_index,
    std::uint64_t expected_version,
    DirtyRecordKind expected_kind,
    std::optional<BackingGeneration> expected_base_generation,
    std::optional<std::uint64_t> expected_dirty_epoch,
    BackingGeneration persisted_generation) {
    const bool exact = expected_base_generation.has_value() &&
                       expected_dirty_epoch.has_value();
    const std::wstring operation = exact
        ? L"DynamicBlockStore::acknowledge_snapshot_member"
        : L"DynamicBlockStore::mark_persisted";
    if (expected_base_generation.has_value() !=
            expected_dirty_epoch.has_value() ||
        (exact && (!token.has_value() || expected_version == 0 ||
                   *expected_dirty_epoch == 0 ||
                   *expected_dirty_epoch > token->cutoff_dirty_epoch ||
                   *expected_base_generation >
                       token->source_backing_generation ||
                   persisted_generation <= *expected_base_generation ||
                   persisted_generation <=
                       token->source_backing_generation))) {
        return Result<SnapshotMemberAck>::failure(backing_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            operation, L"snapshot acknowledgement identity is invalid"));
    }
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<SnapshotMemberAck>::failure(
            closed_backing(operation));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    MutationToken snapshot_sensitive_operation;
    const auto admitted = enter_mutation(
        OperationContext{
            close_stop_source_.get_token(),
            std::chrono::steady_clock::time_point::max()},
        snapshot_sensitive_operation);
    if (!admitted.ok()) {
        return Result<SnapshotMemberAck>::failure(admitted.error());
    }
    ScopeExit leave_snapshot_sensitive_operation(
        [this, &snapshot_sensitive_operation]() noexcept {
            leave_mutation(snapshot_sensitive_operation);
        });
    SnapshotPublicationPin publication_pin;
    if (token.has_value()) {
        auto acquired =
            snapshot_coordinator_->acquire_publication_pin(*token);
        if (!acquired.ok()) {
            return Result<SnapshotMemberAck>::failure(acquired.error());
        }
        publication_pin = acquired.move_value();
    } else if (snapshot_coordinator_->snapshot().active_token.has_value()) {
        return Result<SnapshotMemberAck>::failure(backing_error(
            static_cast<std::uint32_t>(
                DynamicBlockStoreErrorCode::SnapshotBusy),
            operation,
            L"acknowledgement requires the active snapshot token"));
    }

    if (block_index >= geometry_.total_blocks) {
        return Result<SnapshotMemberAck>::failure(backing_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            operation,
            L"block index is outside the logical device"));
    }

    auto* entry = directory_->find(block_index);
    if (entry == nullptr) {
        std::scoped_lock state_lock(state_mutex_);
        ++stats_.snapshot_version_conflicts;
        return Result<SnapshotMemberAck>::failure(version_conflict(
            operation, L"dirty block metadata no longer exists"));
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
        return Result<SnapshotMemberAck>::failure(
            closed_backing(operation));
    }

    const bool resident_dirty = entry->state == BlockState::ResidentDirty;
    const bool zero_dirty = entry->state == BlockState::ZeroDirty;
    const bool dirty = resident_dirty || zero_dirty;
    std::unique_lock<std::mutex> exact_generation_lock;
    if (exact) {
        exact_generation_lock = std::unique_lock<std::mutex>(state_mutex_);
        if (backing_view_.generation != persisted_generation) {
            ++stats_.backing_generation_conflicts;
            return Result<SnapshotMemberAck>::failure(generation_conflict(
                operation,
                L"exact acknowledgement requires the installed generation"));
        }
        if (!dirty) {
            const bool clean = entry->state == BlockState::Unresident ||
                entry->state == BlockState::ResidentClean ||
                entry->state == BlockState::ZeroClean;
            const bool same_identity =
                entry->version == expected_version &&
                entry->dirty_kind == expected_kind &&
                entry->dirty_base_generation ==
                    *expected_base_generation &&
                entry->dirty_epoch == *expected_dirty_epoch;
            if (clean && same_identity &&
                entry->clean_generation == persisted_generation) {
                return Result<SnapshotMemberAck>::success(
                    SnapshotMemberAck::AlreadyPersisted);
            }
            ++stats_.snapshot_version_conflicts;
            return Result<SnapshotMemberAck>::failure(version_conflict(
                operation,
                L"clean snapshot member identity does not match the retry"));
        }
        if (entry->dirty_epoch > token->cutoff_dirty_epoch &&
            entry->dirty_epoch > *expected_dirty_epoch) {
            return Result<SnapshotMemberAck>::success(
                SnapshotMemberAck::NewerDirtyPreserved);
        }
    }
    const bool snapshot_epoch_changed = token.has_value() &&
        entry->dirty_epoch > token->cutoff_dirty_epoch;
    const bool identity_changed = entry->version != expected_version ||
        entry->dirty_kind != expected_kind ||
        (exact &&
         (entry->dirty_base_generation != *expected_base_generation ||
          entry->dirty_epoch != *expected_dirty_epoch));
    if (!dirty || identity_changed || snapshot_epoch_changed) {
        if (exact_generation_lock.owns_lock()) {
            ++stats_.snapshot_version_conflicts;
        } else {
            std::scoped_lock state_lock(state_mutex_);
            ++stats_.snapshot_version_conflicts;
        }
        return Result<SnapshotMemberAck>::failure(version_conflict(
            operation,
            L"dirty state or exact snapshot identity changed"));
    }
    if (exact_generation_lock.owns_lock()) exact_generation_lock.unlock();

    bool test_transition_active = false;
    const auto original_state = entry->state;
    const bool pause_after_validation =
        persisted_ack_pause_for_test_.load(std::memory_order_acquire);
    if (pause_after_validation) {
        {
            std::scoped_lock state_lock(state_mutex_);
            const bool generation_invalid = exact
                ? backing_view_.generation != persisted_generation
                : persisted_generation <= entry->dirty_base_generation ||
                  persisted_generation > backing_view_.generation;
            if (generation_invalid) {
                ++stats_.backing_generation_conflicts;
                return Result<SnapshotMemberAck>::failure(generation_conflict(
                    operation,
                    L"persisted generation is not an installed successor"));
            }
        }
        const auto owner = next_epoch(transition_epoch_);
        if (owner == 0) {
            return Result<SnapshotMemberAck>::failure(backing_error(
                static_cast<std::uint32_t>(
                    DynamicBlockStoreErrorCode::FatalInternalInvariant),
                operation + L"/test-gate",
                L"transition epoch wrapped"));
        }
        {
            std::scoped_lock state_lock(state_mutex_);
            if (resident_dirty) {
                --stats_.resident_dirty_blocks;
            } else {
                --stats_.zero_dirty_blocks;
            }
        }
        entry->state = BlockState::Committing;
        entry->prior_state = original_state;
        entry->transition_owner = owner;
        entry->transition_epoch = owner;
        test_transition_active = true;
        block_lock.unlock();
        persisted_ack_gap_reached_for_test_.store(
            true, std::memory_order_release);
        persisted_ack_gap_reached_for_test_.notify_all();
        persisted_ack_pause_for_test_.wait(
            true, std::memory_order_acquire);
        block_lock.lock();
        if (entry->state != BlockState::Committing ||
            entry->prior_state != original_state ||
            entry->transition_owner != owner ||
            entry->transition_epoch != owner) {
            std::terminate();
        }
        if (closing_requested()) {
            {
                std::scoped_lock state_lock(state_mutex_);
                if (resident_dirty) {
                    ++stats_.resident_dirty_blocks;
                } else {
                    ++stats_.zero_dirty_blocks;
                }
            }
            entry->state = original_state;
            entry->prior_state = original_state;
            entry->transition_owner = 0;
            entry->transition_epoch = 0;
            test_transition_active = false;
            block_lock.unlock();
            stripe.condition.notify_all();
            return Result<SnapshotMemberAck>::failure(
                closed_backing(operation));
        }
    }

    bool generation_conflicted = false;
    {
        std::scoped_lock state_lock(state_mutex_);
        const bool generation_invalid = exact
            ? backing_view_.generation != persisted_generation
            : persisted_generation <= entry->dirty_base_generation ||
              persisted_generation > backing_view_.generation;
        if (generation_invalid) {
            ++stats_.backing_generation_conflicts;
            generation_conflicted = true;
            if (test_transition_active) {
                if (resident_dirty) {
                    ++stats_.resident_dirty_blocks;
                } else {
                    ++stats_.zero_dirty_blocks;
                }
            }
        } else if (resident_dirty) {
            if (!test_transition_active) {
                --stats_.resident_dirty_blocks;
            }
            ++stats_.resident_clean_blocks;
            entry->state = BlockState::ResidentClean;
            entry->prior_state = BlockState::ResidentClean;
        } else {
            if (!test_transition_active) {
                --stats_.zero_dirty_blocks;
            }
            ++stats_.zero_clean_blocks;
            entry->state = BlockState::ZeroClean;
            entry->prior_state = BlockState::ZeroClean;
        }
        if (!generation_conflicted) {
            entry->clean_generation = persisted_generation;
        }
    }
    if (generation_conflicted) {
        if (test_transition_active) {
            entry->state = original_state;
            entry->prior_state = original_state;
            entry->transition_owner = 0;
            entry->transition_epoch = 0;
            block_lock.unlock();
            stripe.condition.notify_all();
        }
        return Result<SnapshotMemberAck>::failure(generation_conflict(
            operation,
            L"persisted generation is not an installed successor"));
    }
    entry->transition_owner = 0;
    entry->transition_epoch = 0;
    test_transition_active = false;
    block_lock.unlock();
    stripe.condition.notify_all();
    if (resident_dirty) {
        // Pressure can previously have had no victim because every committed
        // block was dirty. A successful persisted acknowledgement creates a
        // newly eligible clean victim without committing another block.
        signal_eviction_worker_if_needed();
    }
    return Result<SnapshotMemberAck>::success(
        SnapshotMemberAck::Acknowledged);
}

bool DynamicBlockStoreTestAccess::persisted_ack_gap_reached(
    const DynamicBlockStore& store) noexcept {
    return store.persisted_ack_gap_reached_for_test_.load(
        std::memory_order_acquire);
}

bool DynamicBlockStoreTestAccess::release_persisted_ack_gap(
    DynamicBlockStore& store) noexcept {
    const bool paused = store.persisted_ack_pause_for_test_.exchange(
        false, std::memory_order_acq_rel);
    store.persisted_ack_pause_for_test_.notify_all();
    return paused;
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

bool DynamicBlockStoreTestAccess::track_retired_backing_view_for_test(
    DynamicBlockStore& store,
    const BackingGeneration generation,
    const std::shared_ptr<const IBackingStore>& backing_store) {
    if (!backing_store) return false;
    std::scoped_lock lock(store.state_mutex_);
    try {
        store.retired_backing_views_.push_back(
            {generation, backing_store});
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace myrd
