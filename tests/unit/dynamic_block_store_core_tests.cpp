#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/zero_backing_store.h"
#include "fake_virtual_memory_api.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;

myrd::DynamicBlockStoreCreateArgs valid_args() {
    return myrd::DynamicBlockStoreCreateArgs{
        kBlock + 4096,
        512,
        myrd::DynamicBlockStoreOptions{
            2 * kBlock,
            4 * kBlock,
            kBlock,
            kBlock,
            8,
            64,
            myrd::kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250},
        },
        myrd::InitialBlockState::CleanZero,
        {},
        0,
    };
}

std::unique_ptr<myrd::DynamicBlockStore> require_store(
    myrd::DynamicBlockStoreCreateArgs args,
    myrd::DynamicBlockStoreTestDependencies dependencies = {}) {
    auto result = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(result.ok());
    return result.move_value();
}

class LifecycleRecorder final
    : public myrd::IDynamicBlockStoreLifecycleObserver {
public:
    void on_event(myrd::DynamicBlockStoreLifecycleEvent event) noexcept override {
        const auto index = count_.fetch_add(1, std::memory_order_acq_rel);
        if (index < events_.size()) {
            events_[index] = event;
            threads_[index] = std::this_thread::get_id();
        }
    }

    std::size_t count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    myrd::DynamicBlockStoreLifecycleEvent at(std::size_t index) const noexcept {
        return events_[index];
    }

    std::thread::id thread_at(std::size_t index) const noexcept {
        return threads_[index];
    }

private:
    std::array<myrd::DynamicBlockStoreLifecycleEvent, 16> events_{};
    std::array<std::thread::id, 16> threads_{};
    std::atomic<std::size_t> count_{0};
};

class ReentrantLifecycleObserver final
    : public myrd::IDynamicBlockStoreLifecycleObserver {
public:
    void bind(myrd::DynamicBlockStore* store) noexcept { store_ = store; }

    void on_event(myrd::DynamicBlockStoreLifecycleEvent event) noexcept override {
        if (event == myrd::DynamicBlockStoreLifecycleEvent::RejectNewRequests &&
            store_ != nullptr && !reentered_.exchange(true)) {
            store_->close();
        }
    }

    bool reentered() const noexcept { return reentered_.load(); }

private:
    myrd::DynamicBlockStore* store_{};
    std::atomic<bool> reentered_{false};
};

class ReentrantDestructionBackingStore final : public myrd::IBackingStore {
public:
    explicit ReentrantDestructionBackingStore(std::atomic<bool>& destroyed)
        : destroyed_(destroyed) {}

    ~ReentrantDestructionBackingStore() override {
        if (store_ != nullptr) {
            (void)store_->stats();
        }
        destroyed_.store(true, std::memory_order_release);
    }

    void bind(myrd::DynamicBlockStore* store) noexcept { store_ = store; }

    myrd::Result<myrd::BlockLoadResult> load_block(
        const myrd::OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        return myrd::Result<myrd::BlockLoadResult>::success(
            {myrd::BlockLoadKind::AllZero});
    }

private:
    std::atomic<bool>& destroyed_;
    myrd::DynamicBlockStore* store_{};
};

class TaggedBackingStore final : public myrd::IBackingStore {
public:
    explicit TaggedBackingStore(std::uint64_t tag) noexcept : tag_(tag) {}

    myrd::Result<myrd::BlockLoadResult> load_block(
        const myrd::OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        load_count_.fetch_add(1, std::memory_order_relaxed);
        return myrd::Result<myrd::BlockLoadResult>::success(
            {myrd::BlockLoadKind::AllZero});
    }

    std::uint64_t tag() const noexcept { return tag_; }
    std::uint64_t load_count() const noexcept {
        return load_count_.load(std::memory_order_relaxed);
    }

private:
    const std::uint64_t tag_;
    mutable std::atomic<std::uint64_t> load_count_{0};
};

void require_factory_failure(myrd::DynamicBlockStoreCreateArgs args) {
    auto result = myrd::DynamicBlockStore::create(std::move(args));
    REQUIRE(!result.ok());
    REQUIRE(result.has_error());
}

} // namespace

static_assert(std::is_base_of_v<myrd::IBlockDevice, myrd::DynamicBlockStore>);

TEST_CASE(m2_store_validates_every_create_argument_rule) {
    auto args = valid_args();
    args.logical_size_bytes = 0;
    require_factory_failure(args);

    args = valid_args();
    args.sector_size_bytes = 1024;
    require_factory_failure(args);

    args = valid_args();
    args.logical_size_bytes = kBlock + 1;
    require_factory_failure(args);

    args = valid_args();
    args.logical_size_bytes = std::numeric_limits<std::uint64_t>::max() - 511;
    require_factory_failure(args);

    args = valid_args();
    args.options.soft_limit_bytes = args.options.hard_limit_bytes;
    require_factory_failure(args);

    args = valid_args();
    args.options.soft_limit_bytes = kBlock + 1;
    require_factory_failure(args);

    args = valid_args();
    args.options.max_snapshot_batch_bytes = kBlock - 1;
    require_factory_failure(args);

    args = valid_args();
    args.options.max_snapshot_batch_bytes = kBlock + 1;
    require_factory_failure(args);

    args = valid_args();
    args.options.max_snapshot_batch_records = 0;
    require_factory_failure(args);

    args = valid_args();
    args.options.lock_stripe_count = 63;
    require_factory_failure(args);

    args = valid_args();
    args.options.lock_stripe_count = 65;
    require_factory_failure(args);

    args = valid_args();
    args.initial_state = static_cast<myrd::InitialBlockState>(0xff);
    require_factory_failure(args);

    args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    require_factory_failure(args);
}

TEST_CASE(m2_store_preserves_logical_size_and_rounds_reservation) {
    auto store = require_store(valid_args());
    REQUIRE_EQ(store->size_bytes(), kBlock + 4096);
    REQUIRE_EQ(store->sector_size(), 512u);
    REQUIRE_EQ(store->reservation_size_bytes(), 2 * kBlock);
    REQUIRE_EQ(store->total_blocks(), 2ull);
    REQUIRE_EQ(store->last_block_size_bytes(), 4096ull);
}

TEST_CASE(m2_store_clean_zero_null_backing_is_zero_normalized_and_implicit) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.virtual_memory_api = api;
    auto args = valid_args();
    args.initial_backing_generation = 17;
    auto store = require_store(std::move(args), dependencies);

    const auto view = store->backing_view_for_test();
    REQUIRE(view.store != nullptr);
    REQUIRE(dynamic_cast<const myrd::ZeroBackingStore*>(view.store.get()) !=
            nullptr);
    REQUIRE_EQ(view.generation, 17ull);
    REQUIRE_EQ(store->backing_generation(), 17ull);
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(store->stats().zero_clean_blocks, 2ull);
    REQUIRE_EQ(api->allocate_calls().size(), 1ull);
}

TEST_CASE(m2_store_clean_zero_construction_does_not_load_explicit_backing) {
    auto backing = std::make_shared<TaggedBackingStore>(31);
    auto args = valid_args();
    args.backing_store = backing;
    args.initial_backing_generation = 31;
    auto store = require_store(std::move(args));

    REQUIRE_EQ(backing->load_count(), 0ull);
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
}

TEST_CASE(m2_store_backing_unresident_requires_backing_and_is_implicit) {
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = std::make_shared<myrd::ZeroBackingStore>();
    args.initial_backing_generation = 9;
    auto store = require_store(std::move(args));

    REQUIRE_EQ(store->implicit_initial_state_for_test(),
               myrd::InitialBlockState::BackingUnresident);
    REQUIRE_EQ(store->implicit_initial_generation_for_test(), 9ull);
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE_EQ(store->stats().zero_clean_blocks, 0ull);
}

TEST_CASE(m2_store_initial_geometry_options_stats_and_features_are_exact) {
    auto args = valid_args();
    args.initial_backing_generation = 23;
    const auto expected_options = args.options;
    auto store = require_store(std::move(args));

    const auto stats = store->stats();
    REQUIRE_EQ(stats.logical_size_bytes, kBlock + 4096);
    REQUIRE_EQ(stats.reserved_bytes, 2 * kBlock);
    REQUIRE_EQ(stats.committed_bytes, 0ull);
    REQUIRE_EQ(stats.resident_clean_blocks, 0ull);
    REQUIRE_EQ(stats.resident_dirty_blocks, 0ull);
    REQUIRE_EQ(stats.zero_clean_blocks, 2ull);
    REQUIRE_EQ(stats.zero_dirty_blocks, 0ull);
    REQUIRE_EQ(stats.loading_blocks, 0ull);
    REQUIRE_EQ(stats.backing_generation, 23ull);
    REQUIRE_EQ(stats.backing_view_installs_total, 0ull);
    REQUIRE_EQ(store->options().soft_limit_bytes,
               expected_options.soft_limit_bytes);
    REQUIRE_EQ(store->options().lock_stripe_count,
               expected_options.lock_stripe_count);
    REQUIRE(!store->features().read_only);
    REQUIRE(store->features().supports_unmap);
    REQUIRE(store->features().supports_zero);
}

TEST_CASE(m2_store_initial_generation_and_view_are_coherent) {
    auto first = std::make_shared<myrd::ZeroBackingStore>();
    auto second = std::make_shared<myrd::ZeroBackingStore>();
    auto args = valid_args();
    args.backing_store = first;
    args.initial_backing_generation = 7;
    auto store = require_store(std::move(args));

    auto initial = store->backing_view_for_test();
    REQUIRE_EQ(initial.generation, 7ull);
    REQUIRE(initial.store == first);

    REQUIRE(store->install_backing_view({8, second}).ok());
    auto installed = store->backing_view_for_test();
    REQUIRE_EQ(installed.generation, 8ull);
    REQUIRE(installed.store == second);

    const auto same = store->install_backing_view({8, first});
    REQUIRE(!same.ok());
    const auto lower = store->install_backing_view({6, first});
    REQUIRE(!lower.ok());
    const auto null_store = store->install_backing_view({9, {}});
    REQUIRE(!null_store.ok());
    auto unchanged = store->backing_view_for_test();
    REQUIRE_EQ(unchanged.generation, 8ull);
    REQUIRE(unchanged.store == second);
}

TEST_CASE(m2_store_rejects_generation_wrap_without_changing_view) {
    auto args = valid_args();
    args.initial_backing_generation =
        std::numeric_limits<std::uint64_t>::max();
    auto store = require_store(std::move(args));
    auto before = store->backing_view_for_test();
    auto result = store->install_backing_view(
        {0, std::make_shared<myrd::ZeroBackingStore>()});
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::BackingGenerationConflict));
    auto after = store->backing_view_for_test();
    REQUIRE_EQ(after.generation, before.generation);
    REQUIRE(after.store == before.store);
}

TEST_CASE(m2_store_concurrent_view_capture_never_tears_generation_and_pointer) {
    constexpr std::uint64_t kInstalls = 1000;
    auto args = valid_args();
    args.backing_store = std::make_shared<TaggedBackingStore>(0);
    auto store = require_store(std::move(args));
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::atomic<bool> valid{true};
    std::vector<std::thread> readers;

    for (std::size_t reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!done.load(std::memory_order_acquire)) {
                const auto view = store->backing_view_for_test();
                const auto* tagged =
                    dynamic_cast<const TaggedBackingStore*>(view.store.get());
                if (tagged == nullptr || tagged->tag() != view.generation) {
                    valid.store(false, std::memory_order_release);
                    return;
                }
                const auto snapshot = store->stats();
                if (snapshot.logical_size_bytes != kBlock + 4096 ||
                    snapshot.reserved_bytes != 2 * kBlock) {
                    valid.store(false, std::memory_order_release);
                    return;
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (std::uint64_t generation = 1; generation <= kInstalls; ++generation) {
        auto installed = store->install_backing_view(
            {generation, std::make_shared<TaggedBackingStore>(generation)});
        if (!installed.ok()) {
            valid.store(false, std::memory_order_release);
            break;
        }
    }
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(valid.load(std::memory_order_acquire));
    REQUIRE_EQ(store->backing_generation(), kInstalls);
    REQUIRE_EQ(store->stats().backing_generation, kInstalls);
    REQUIRE_EQ(store->stats().backing_view_installs_total, kInstalls);
}

TEST_CASE(m2_store_factory_failure_stages_release_reserved_arena) {
    const auto make_dependencies = [] {
        myrd::DynamicBlockStoreTestDependencies dependencies;
        dependencies.virtual_memory_api =
            std::make_shared<myrd::test::FakeVirtualMemoryApi>();
        return dependencies;
    };

    {
        auto dependencies = make_dependencies();
        auto api = std::static_pointer_cast<
            myrd::test::FakeVirtualMemoryApi>(
                dependencies.virtual_memory_api);
        dependencies.fail_stage =
            myrd::DynamicBlockStoreFactoryStage::CreateStripes;
        auto result = myrd::DynamicBlockStore::create_for_testing(
            valid_args(), dependencies);
        REQUIRE(!result.ok());
        REQUIRE_EQ(api->free_calls().size(), 1ull);
    }
    {
        auto dependencies = make_dependencies();
        auto api = std::static_pointer_cast<
            myrd::test::FakeVirtualMemoryApi>(
                dependencies.virtual_memory_api);
        dependencies.fail_stage =
            myrd::DynamicBlockStoreFactoryStage::AllocateDirectory;
        auto result = myrd::DynamicBlockStore::create_for_testing(
            valid_args(), dependencies);
        REQUIRE(!result.ok());
        REQUIRE_EQ(api->free_calls().size(), 1ull);
    }
    {
        auto dependencies = make_dependencies();
        auto api = std::static_pointer_cast<
            myrd::test::FakeVirtualMemoryApi>(
                dependencies.virtual_memory_api);
        dependencies.fail_stage =
            myrd::DynamicBlockStoreFactoryStage::StartWorker;
        auto result = myrd::DynamicBlockStore::create_for_testing(
            valid_args(), dependencies);
        REQUIRE(!result.ok());
        REQUIRE_EQ(api->free_calls().size(), 1ull);
    }
    {
        auto dependencies = make_dependencies();
        auto api = std::static_pointer_cast<
            myrd::test::FakeVirtualMemoryApi>(
                dependencies.virtual_memory_api);
        api->fail(myrd::test::FakeVirtualMemoryOperation::Reserve, 1234);
        auto result = myrd::DynamicBlockStore::create_for_testing(
            valid_args(), dependencies);
        REQUIRE(!result.ok());
        REQUIRE_EQ(api->free_calls().size(), 0ull);
    }

    {
        auto dependencies = make_dependencies();
        auto api = std::static_pointer_cast<
            myrd::test::FakeVirtualMemoryApi>(
                dependencies.virtual_memory_api);
        auto observer = std::make_shared<LifecycleRecorder>();
        dependencies.lifecycle_observer = observer;
        dependencies.fail_stage =
            myrd::DynamicBlockStoreFactoryStage::AfterWorkerStarted;
        auto result = myrd::DynamicBlockStore::create_for_testing(
            valid_args(), dependencies);
        REQUIRE(!result.ok());
        REQUIRE_EQ(api->free_calls().size(), 1ull);
        REQUIRE_EQ(observer->at(0),
                   myrd::DynamicBlockStoreLifecycleEvent::WorkerStarted);
        REQUIRE_EQ(observer->at(3),
                   myrd::DynamicBlockStoreLifecycleEvent::WorkerStopped);
    }
}

TEST_CASE(m2_store_close_rejects_new_requests_and_destroys_in_order) {
    auto observer = std::make_shared<LifecycleRecorder>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.lifecycle_observer = observer;
    auto store = require_store(valid_args(), dependencies);
    REQUIRE(store->worker_running_for_test());
    REQUIRE_EQ(observer->thread_at(0), std::this_thread::get_id());

    store->close();
    REQUIRE(store->is_closed());
    std::array<std::byte, 1> byte{};
    const myrd::OperationContext context;
    REQUIRE_EQ(store->read(context, 0, byte).error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(store->install_backing_view(
                   {1, std::make_shared<myrd::ZeroBackingStore>()})
                   .error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));

    store.reset();
    REQUIRE_EQ(observer->count(), 8ull);
    REQUIRE_EQ(observer->at(0),
               myrd::DynamicBlockStoreLifecycleEvent::WorkerStarted);
    REQUIRE_EQ(observer->at(1),
               myrd::DynamicBlockStoreLifecycleEvent::RejectNewRequests);
    REQUIRE_EQ(observer->at(2),
               myrd::DynamicBlockStoreLifecycleEvent::WaitersNotified);
    REQUIRE_EQ(observer->at(3),
               myrd::DynamicBlockStoreLifecycleEvent::WorkerStopped);
    REQUIRE_EQ(observer->at(4),
               myrd::DynamicBlockStoreLifecycleEvent::Quiesced);
    REQUIRE_EQ(observer->at(5),
               myrd::DynamicBlockStoreLifecycleEvent::DirectoryReleased);
    REQUIRE_EQ(observer->at(6),
               myrd::DynamicBlockStoreLifecycleEvent::StripesReleased);
    REQUIRE_EQ(observer->at(7),
               myrd::DynamicBlockStoreLifecycleEvent::ArenaReleased);
}

TEST_CASE(m2_store_close_wakes_a_real_stripe_waiter) {
    auto store = require_store(valid_args());
    std::atomic<bool> waiter_returned{false};
    std::thread waiter([&] {
        const auto result = store->wait_for_close_for_test(0);
        waiter_returned.store(result.ok(), std::memory_order_release);
    });

    while (store->stripe_waiter_count_for_test() == 0) {
        std::this_thread::yield();
    }
    store->close();
    waiter.join();
    REQUIRE(waiter_returned.load(std::memory_order_acquire));
    REQUIRE_EQ(store->stripe_waiter_count_for_test(), 0ull);
}

TEST_CASE(m2_store_lifecycle_observer_may_reenter_close_without_deadlock) {
    auto observer = std::make_shared<ReentrantLifecycleObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.lifecycle_observer = observer;
    auto store = require_store(valid_args(), dependencies);
    observer->bind(store.get());

    store->close();
    REQUIRE(observer->reentered());
    REQUIRE(store->is_closed());
}

TEST_CASE(m2_store_releases_retired_backing_outside_state_lock) {
    std::atomic<bool> destroyed{false};
    auto old_backing =
        std::make_shared<ReentrantDestructionBackingStore>(destroyed);
    auto args = valid_args();
    args.backing_store = old_backing;
    auto store = require_store(std::move(args));
    old_backing->bind(store.get());
    old_backing.reset();

    REQUIRE(store->install_backing_view(
                {1, std::make_shared<myrd::ZeroBackingStore>()})
                .ok());
    REQUIRE(destroyed.load(std::memory_order_acquire));
}

TEST_CASE(m2_store_exposes_m1_interface_without_proxy_downcast) {
    auto store = require_store(valid_args());
    myrd::IBlockDevice& device = *store;
    REQUIRE_EQ(device.size_bytes(), kBlock + 4096);
    REQUIRE(!device.features().read_only);
}
