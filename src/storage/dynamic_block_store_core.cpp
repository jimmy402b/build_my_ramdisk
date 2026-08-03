#include "dynamic_block_store.h"

#include "block_directory.h"
#include "dynamic_block_store_internal.h"
#include "eviction_manager.h"
#include "virtual_memory_arena.h"
#include "zero_backing_store.h"
#include "src/common/scope_exit.h"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace myrd {

namespace {

Error make_store_error(std::uint32_t code,
                       std::wstring operation,
                       std::wstring message) {
    return Error{
        ErrorDomain::storage,
        code,
        std::move(operation),
        std::move(message),
    };
}

Error make_session_closed_error(std::wstring operation) {
    return make_store_error(
        static_cast<std::uint32_t>(OperationErrorCode::session_closed),
        std::move(operation),
        L"dynamic block store is closed");
}

Error make_factory_error(std::wstring operation, std::wstring message) {
    return make_store_error(
        static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
        std::move(operation),
        std::move(message));
}

Error make_validation_error(
    const DynamicBlockStoreCreateValidation& validation) {
    std::wstring message = L"invalid dynamic block store create arguments: ";
    const auto name = to_string(validation.error);
    message.append(name.begin(), name.end());
    if (validation.error == DynamicBlockStoreCreateError::InvalidOptions) {
        message.append(L"/");
        const auto option_name = to_string(validation.options_error);
        message.append(option_name.begin(), option_name.end());
    }
    return make_store_error(
        static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
        L"DynamicBlockStore::create",
        std::move(message));
}

} // namespace

thread_local DynamicBlockStore::RequestToken*
    DynamicBlockStore::current_request_token_ = nullptr;

Result<std::unique_ptr<DynamicBlockStore>> DynamicBlockStore::create(
    DynamicBlockStoreCreateArgs args) {
    return create_impl(std::move(args), {});
}

Result<std::unique_ptr<DynamicBlockStore>>
DynamicBlockStore::create_for_testing(
    DynamicBlockStoreCreateArgs args,
    DynamicBlockStoreTestDependencies dependencies) {
    return create_impl(std::move(args), std::move(dependencies));
}

Result<std::unique_ptr<DynamicBlockStore>> DynamicBlockStore::create_impl(
    DynamicBlockStoreCreateArgs args,
    DynamicBlockStoreTestDependencies dependencies) {
    const auto validation = validate_dynamic_block_store_create_args(args);
    if (!validation.ok()) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_validation_error(validation));
    }

    try {
        if (validation.requires_zero_backing_store()) {
            args.backing_store = std::make_shared<ZeroBackingStore>();
        }
    } catch (...) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/backing",
                               L"failed to allocate normalized backing store"));
    }

    BackingView initial_view{
        args.initial_backing_generation,
        args.backing_store,
    };
    if (!initial_view.store) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/backing",
                               L"normalized backing store is null"));
    }

    const auto geometry_result = compute_block_geometry(args.logical_size_bytes);
    if (!geometry_result.ok()) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/geometry",
                               L"failed to compute block geometry"));
    }
    const auto geometry = geometry_result.value;

    auto arena_result = dependencies.virtual_memory_api
                            ? VirtualMemoryArena::create_with_api(
                                  geometry.reservation_size_bytes,
                                  dependencies.virtual_memory_api,
                                  dependencies.cleanup_tracker)
                            : VirtualMemoryArena::create(
                                  geometry.reservation_size_bytes,
                                  dependencies.cleanup_tracker);
    if (!arena_result.ok()) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            arena_result.error());
    }
    auto arena = arena_result.move_value();

    if (dependencies.fail_stage ==
        DynamicBlockStoreFactoryStage::CreateStripes) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/stripes",
                               L"injected stripe construction failure"));
    }
    auto stripes_result = LockStripeSet::create(args.options.lock_stripe_count);
    if (!stripes_result.ok()) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/stripes",
                               L"failed to allocate lock stripes"));
    }
    auto stripes = stripes_result.move_value();

    if (dependencies.fail_stage ==
        DynamicBlockStoreFactoryStage::AllocateDirectory) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/directory",
                               L"injected directory construction failure"));
    }
    auto directory_result = BlockDirectory::create(
        geometry.total_blocks,
        args.initial_state,
        args.initial_backing_generation);
    if (!directory_result.ok()) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/directory",
                               L"failed to allocate sparse block directory"));
    }
    auto directory = directory_result.move_value();

    auto store = std::unique_ptr<DynamicBlockStore>(new (std::nothrow)
        DynamicBlockStore(std::move(args),
                          geometry,
                          std::move(initial_view),
                          std::move(arena),
                          std::move(stripes),
                          std::move(directory),
                          std::move(dependencies.lifecycle_observer),
                          std::move(dependencies.read_observer),
                          dependencies.fail_snapshot_allocation,
                          dependencies.pause_before_snapshot_wait,
                          dependencies.pause_after_snapshot_pin));
    if (!store) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/object",
                               L"failed to allocate store object"));
    }

    store->eviction_manager_ = EvictionManager::create(*store);
    if (!store->eviction_manager_) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/eviction",
                               L"failed to allocate eviction manager"));
    }

    if (dependencies.fail_stage ==
        DynamicBlockStoreFactoryStage::StartWorker) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/worker",
                               L"injected worker start failure"));
    }
    bool worker_started = false;
    try {
        worker_started = store->start_worker();
    } catch (...) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/worker",
                               L"worker synchronization failed"));
    }
    if (!worker_started) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/worker",
                               L"failed to start eviction worker"));
    }
    if (dependencies.fail_stage ==
        DynamicBlockStoreFactoryStage::AfterWorkerStarted) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            make_factory_error(L"DynamicBlockStore::create/publish",
                               L"injected post-worker publication failure"));
    }

    return Result<std::unique_ptr<DynamicBlockStore>>::success(
        std::move(store));
}

DynamicBlockStore::DynamicBlockStore(
    DynamicBlockStoreCreateArgs args,
    BlockGeometry geometry,
    BackingView backing_view,
    std::unique_ptr<VirtualMemoryArena> arena,
    std::unique_ptr<LockStripeSet> stripes,
    std::unique_ptr<BlockDirectory> directory,
    std::shared_ptr<IDynamicBlockStoreLifecycleObserver>
        lifecycle_observer,
    std::shared_ptr<IDynamicBlockStoreReadObserver>
        read_observer,
    bool fail_snapshot_allocation,
    bool pause_before_snapshot_wait,
    bool pause_after_snapshot_pin) noexcept
    : geometry_(geometry),
      options_(args.options),
      sector_size_bytes_(args.sector_size_bytes),
      backing_view_(std::move(backing_view)),
      arena_(std::move(arena)),
      stripes_(std::move(stripes)),
      directory_(std::move(directory)),
      lifecycle_observer_(std::move(lifecycle_observer)),
      read_observer_(std::move(read_observer)),
      fail_snapshot_allocation_(fail_snapshot_allocation),
      snapshot_wait_pause_for_test_(pause_before_snapshot_wait),
      snapshot_pin_pause_for_test_(pause_after_snapshot_pin) {
    stats_.logical_size_bytes = geometry_.logical_size_bytes;
    stats_.reserved_bytes = geometry_.reservation_size_bytes;
    stats_.zero_clean_blocks =
        args.initial_state == InitialBlockState::CleanZero
            ? geometry_.total_blocks
            : 0;
    stats_.backing_generation = backing_view_.generation;
}

DynamicBlockStore::~DynamicBlockStore() noexcept {
    try {
        close();
    } catch (...) {
        // Once destruction starts, releasing the arena while a worker or a
        // request may still reference it would be memory-unsafe. A platform
        // synchronization failure is therefore an explicit fail-fast event.
        std::terminate();
    }

    eviction_manager_.reset();
    directory_.reset();
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::DirectoryReleased));
    stripes_.reset();
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::StripesReleased));
    arena_.reset();
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::ArenaReleased));
}

bool DynamicBlockStore::start_worker() {
    try {
        worker_ = std::thread(&DynamicBlockStore::worker_main, this);
    } catch (...) {
        return false;
    }

    {
        std::unique_lock lock(lifecycle_mutex_);
        lifecycle_condition_.wait(lock, [this] { return worker_running_; });
    }
    // Observers are lifecycle-owner callbacks. Never invoke one from the
    // worker itself: a reentrant close would otherwise attempt to join self.
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::WorkerStarted));
    return true;
}

void DynamicBlockStore::worker_main() noexcept {
    try {
        std::unique_lock lock(lifecycle_mutex_);
        worker_running_ = true;
        eviction_worker_waiting_ = true;
        lifecycle_condition_.notify_all();
        for (;;) {
            lifecycle_condition_.wait(lock, [this] {
                return lifecycle_state_ != LifecycleState::Open ||
                       eviction_requested_;
            });
            if (lifecycle_state_ != LifecycleState::Open) {
                break;
            }

            eviction_requested_ = false;
            eviction_worker_waiting_ = false;
            ++eviction_worker_wakeups_;
            lock.unlock();
            eviction_manager_->reclaim_to(
                options_.eviction_low_watermark_bytes);
            lock.lock();
            ++eviction_worker_passes_;
            eviction_worker_waiting_ = true;
            lifecycle_condition_.notify_all();
        }
        eviction_worker_waiting_ = false;
        worker_running_ = false;
        lifecycle_condition_.notify_all();
    } catch (...) {
        // Continuing after an internal synchronization primitive fails could
        // publish a store whose worker lifetime is unknowable. Fail fast
        // instead of risking use-after-free during later destruction.
        std::terminate();
    }
}

bool DynamicBlockStore::signal_eviction_worker_if_needed() {
    bool pressure = false;
    {
        std::scoped_lock state_lock(state_mutex_);
        pressure = stats_.committed_bytes > options_.soft_limit_bytes;
    }
    if (!pressure) {
        return false;
    }

    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (lifecycle_state_ != LifecycleState::Open) {
            return false;
        }
        eviction_requested_ = true;
    }
    lifecycle_condition_.notify_one();
    return true;
}

void DynamicBlockStore::close() {
    const bool called_from_request = current_thread_has_active_request();
    {
        std::unique_lock lock(lifecycle_mutex_);
        if (lifecycle_state_ == LifecycleState::Closed) {
            return;
        }
        if (lifecycle_state_ == LifecycleState::Closing) {
            if (close_owner_ == std::this_thread::get_id() ||
                called_from_request) {
                // A synchronous observer or an in-flight request must not
                // wait for itself to leave the active-request set.
                return;
            }
            lifecycle_condition_.wait(lock, [this] {
                return lifecycle_state_ == LifecycleState::Closed;
            });
            return;
        }
        lifecycle_state_ = LifecycleState::Closing;
        close_owner_ = std::this_thread::get_id();
        closing_requested_.store(true, std::memory_order_release);
    }

    close_stop_source_.request_stop();
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::RejectNewRequests));
    notify_all_waiters();
    lifecycle_condition_.notify_all();
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::WaitersNotified));

    if (worker_.joinable()) {
        worker_.join();
    }
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::WorkerStopped));

    if (called_from_request) {
        // The request that initiated close still owns store resources. Its
        // final leave_request() publishes Quiesced/Closed after every active
        // request has drained.
        std::scoped_lock lock(lifecycle_mutex_);
        deferred_close_ready_ = true;
        return;
    }

    {
        std::unique_lock lock(lifecycle_mutex_);
        lifecycle_condition_.wait(lock, [this] {
            return active_requests_ == 0;
        });
    }
    finish_close();
}

void DynamicBlockStore::finish_close() {
    notify_lifecycle_event(static_cast<std::uint8_t>(
        DynamicBlockStoreLifecycleEvent::Quiesced));

    {
        std::scoped_lock lock(lifecycle_mutex_);
        lifecycle_state_ = LifecycleState::Closed;
        deferred_close_ready_ = false;
        close_owner_ = {};
    }
    lifecycle_condition_.notify_all();
}

bool DynamicBlockStore::try_enter_request(RequestToken& token) {
    std::scoped_lock lock(lifecycle_mutex_);
    if (lifecycle_state_ != LifecycleState::Open) {
        return false;
    }
    token.store = this;
    token.previous = current_request_token_;
    current_request_token_ = &token;
    ++active_requests_;
    return true;
}

void DynamicBlockStore::leave_request(RequestToken& token) noexcept {
    try {
        if (current_request_token_ != &token || token.store != this) {
            std::terminate();
        }
        current_request_token_ = token.previous;
        token = {};

        bool finish_deferred_close = false;
        {
            std::scoped_lock lock(lifecycle_mutex_);
            if (active_requests_ == 0) {
                std::terminate();
            }
            --active_requests_;
            if (active_requests_ == 0) {
                lifecycle_condition_.notify_all();
                if (lifecycle_state_ == LifecycleState::Closing &&
                    deferred_close_ready_) {
                    deferred_close_ready_ = false;
                    close_owner_ = std::this_thread::get_id();
                    finish_deferred_close = true;
                }
            }
        }
        if (finish_deferred_close) {
            finish_close();
        }
    } catch (...) {
        // A failed synchronization operation leaves request lifetime and
        // shutdown ownership unknowable; continuing could free live state.
        std::terminate();
    }
}

bool DynamicBlockStore::current_thread_has_active_request() const noexcept {
    for (auto* token = current_request_token_;
         token != nullptr;
         token = token->previous) {
        if (token->store == this) {
            return true;
        }
    }
    return false;
}

bool DynamicBlockStore::closing_requested() const noexcept {
    return closing_requested_.load(std::memory_order_acquire);
}

void DynamicBlockStore::notify_lifecycle_event(std::uint8_t event) noexcept {
    if (lifecycle_observer_) {
        lifecycle_observer_->on_event(
            static_cast<DynamicBlockStoreLifecycleEvent>(event));
    }
}

void DynamicBlockStore::notify_all_waiters() {
    snapshot_wait_pause_for_test_.store(false, std::memory_order_release);
    snapshot_wait_pause_for_test_.notify_all();
    snapshot_pin_pause_for_test_.store(false, std::memory_order_release);
    snapshot_pin_pause_for_test_.notify_all();
    {
        // Synchronize with install_backing_view's predicate check before
        // notifying it. Without taking state_mutex_, close could publish the
        // closing flag between that check and condition_variable::wait's
        // atomic unlock-and-sleep sequence.
        std::scoped_lock state_lock(state_mutex_);
    }
    backing_view_condition_.notify_all();
    if (!stripes_) {
        return;
    }
    for (std::size_t index = 0; index < stripes_->count(); ++index) {
        auto& stripe = stripes_->stripe_for(index);
        {
            // Synchronize with the waiter's predicate check. The close flag is
            // already visible, so a waiter cannot check false and then miss
            // this notification while atomically releasing the stripe lock.
            std::scoped_lock lock(stripe.mutex);
        }
        stripe.condition.notify_all();
    }
}

std::uint64_t DynamicBlockStore::size_bytes() const noexcept {
    return geometry_.logical_size_bytes;
}

std::uint32_t DynamicBlockStore::sector_size() const noexcept {
    return sector_size_bytes_;
}

DeviceFeatures DynamicBlockStore::features() const noexcept {
    return DeviceFeatures{false, true, true};
}

const BlockGeometry& DynamicBlockStore::geometry() const noexcept {
    return geometry_;
}

const DynamicBlockStoreOptions& DynamicBlockStore::options() const noexcept {
    return options_;
}

DynamicBlockStoreStats DynamicBlockStore::stats() const {
    std::scoped_lock lock(state_mutex_);
    return stats_;
}

std::uint64_t DynamicBlockStore::reservation_size_bytes() const noexcept {
    return geometry_.reservation_size_bytes;
}

std::uint64_t DynamicBlockStore::total_blocks() const noexcept {
    return geometry_.total_blocks;
}

std::uint64_t DynamicBlockStore::last_block_size_bytes() const noexcept {
    return geometry_.last_block_size_bytes;
}

bool DynamicBlockStore::is_closed() const noexcept {
    return closing_requested();
}

Result<void> DynamicBlockStore::validate_invariants_for_test() {
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(make_session_closed_error(
            L"DynamicBlockStore::validate_invariants_for_test"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    constexpr std::uint32_t kMaxSnapshotAttempts = 8;
    for (std::uint32_t attempt = 0;
         attempt < kMaxSnapshotAttempts;
         ++attempt) {
        BackingView view_before;
        DynamicBlockStoreStats stats_before;
        {
            std::scoped_lock lock(state_mutex_);
            view_before = backing_view_;
            stats_before = stats_;
        }
        const auto committed_before = arena_->committed_bytes();
        const auto leaves_before = directory_->allocated_leaf_count();

        bool block_invariant_failed = false;
        for (std::uint64_t leaf_index = 0;
             leaf_index < directory_->top_level_slot_count();
             ++leaf_index) {
            const auto* leaf = directory_->allocated_leaf_at(leaf_index);
            if (leaf == nullptr) {
                continue;
            }
            const auto first_block = leaf_index * kBlocksPerLeaf;
            const auto remaining = geometry_.total_blocks - first_block;
            const auto entry_count =
                remaining < kBlocksPerLeaf ? remaining : kBlocksPerLeaf;
            for (std::uint64_t entry_index = 0;
                 entry_index < entry_count;
                 ++entry_index) {
                const auto block = first_block + entry_index;
                std::scoped_lock lock(stripes_->stripe_for(block).mutex);
                const auto invariant = validate_block_entry(
                    leaf->entries[static_cast<std::size_t>(entry_index)],
                    view_before.generation,
                    false,
                    false);
                block_invariant_failed |= !invariant.ok();
            }
        }

        BackingView view_after;
        DynamicBlockStoreStats stats_after;
        {
            std::scoped_lock lock(state_mutex_);
            view_after = backing_view_;
            stats_after = stats_;
        }
        const auto committed_after = arena_->committed_bytes();
        const auto leaves_after = directory_->allocated_leaf_count();

        const bool stable =
            view_before.generation == view_after.generation &&
            view_before.store == view_after.store &&
            stats_before == stats_after &&
            committed_before == committed_after &&
            leaves_before == leaves_after;
        if (!stable) {
            continue;
        }
        if (!view_before.store ||
            stats_before.backing_generation != view_before.generation ||
            stats_before.logical_size_bytes != geometry_.logical_size_bytes ||
            stats_before.reserved_bytes != geometry_.reservation_size_bytes ||
            arena_->reserved_bytes() != geometry_.reservation_size_bytes ||
            committed_before != stats_before.committed_bytes ||
            block_invariant_failed) {
            return Result<void>::failure(make_store_error(
                static_cast<std::uint32_t>(
                    DynamicBlockStoreErrorCode::FatalInternalInvariant),
                L"DynamicBlockStore::validate_invariants_for_test",
                L"stable store invariant snapshot failed"));
        }
        return Result<void>::success();
    }
    return Result<void>::failure(make_store_error(
        static_cast<std::uint32_t>(
            DynamicBlockStoreErrorCode::FatalInternalInvariant),
        L"DynamicBlockStore::validate_invariants_for_test",
        L"could not capture a stable invariant snapshot"));
}

BackingView DynamicBlockStore::backing_view_for_test() const {
    std::scoped_lock lock(state_mutex_);
    return backing_view_;
}

std::uint64_t DynamicBlockStore::allocated_leaf_count_for_test() const noexcept {
    return directory_ ? directory_->allocated_leaf_count() : 0;
}

InitialBlockState
DynamicBlockStore::implicit_initial_state_for_test() const noexcept {
    return directory_->initial_state();
}

BackingGeneration
DynamicBlockStore::implicit_initial_generation_for_test() const noexcept {
    return directory_->initial_generation();
}

bool DynamicBlockStore::worker_running_for_test() const {
    std::scoped_lock lock(lifecycle_mutex_);
    return worker_running_;
}

Result<void> DynamicBlockStore::wait_for_close_for_test(
    std::uint64_t block_index) {
    if (block_index >= geometry_.total_blocks) {
        return Result<void>::failure(make_store_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"DynamicBlockStore::wait_for_close_for_test",
            L"block index is outside the store"));
    }
    RequestToken request;
    if (!try_enter_request(request)) {
        return Result<void>::failure(make_session_closed_error(
            L"DynamicBlockStore::wait_for_close_for_test"));
    }
    ScopeExit leave([this, &request]() { leave_request(request); });

    auto& stripe = stripes_->stripe_for(block_index);
    std::unique_lock lock(stripe.mutex);
    stripe_waiter_count_.fetch_add(1, std::memory_order_release);
    ScopeExit remove_waiter([this]() noexcept {
        stripe_waiter_count_.fetch_sub(1, std::memory_order_release);
    });
    stripe.condition.wait(lock, [this] { return closing_requested(); });
    return Result<void>::success();
}

std::uint64_t DynamicBlockStore::stripe_waiter_count_for_test() const noexcept {
    return stripe_waiter_count_.load(std::memory_order_acquire);
}

std::uint64_t
DynamicBlockStore::backing_install_waiter_count_for_test() const noexcept {
    return backing_install_waiter_count_.load(std::memory_order_acquire);
}

} // namespace myrd
