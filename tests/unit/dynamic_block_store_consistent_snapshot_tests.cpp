#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/common/scope_exit.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"
#include "src/storage/snapshot_coordinator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

myrd::DynamicBlockStoreOptions valid_options() {
    return myrd::DynamicBlockStoreOptions{
        2 * myrd::kBlockSizeBytes,
        3 * myrd::kBlockSizeBytes,
        myrd::kBlockSizeBytes,
        2 * myrd::kBlockSizeBytes,
        8,
        64,
        myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
}

constexpr auto kWaitBound = std::chrono::seconds(2);

myrd::test::FakeBackingStore::BlockData block_data(std::uint8_t seed) {
    myrd::test::FakeBackingStore::BlockData data{};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = std::byte{
            static_cast<std::uint8_t>(seed + (index % 251))};
    }
    return data;
}

myrd::DynamicBlockStoreCreateArgs backing_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t logical_blocks = 4,
    myrd::BackingGeneration generation = 7) {
    auto options = valid_options();
    // Keep admission tests independent of eviction pressure: their four-block
    // fixtures can make every logical block resident at once.
    options.soft_limit_bytes = 3 * myrd::kBlockSizeBytes;
    options.hard_limit_bytes = 4 * myrd::kBlockSizeBytes;
    return myrd::DynamicBlockStoreCreateArgs{
        logical_blocks * myrd::kBlockSizeBytes,
        512,
        options,
        myrd::InitialBlockState::BackingUnresident,
        std::move(backing),
        generation,
    };
}

std::unique_ptr<myrd::DynamicBlockStore> require_store(
    myrd::DynamicBlockStoreCreateArgs args) {
    auto created = myrd::DynamicBlockStore::create(std::move(args));
    REQUIRE(created.ok());
    return created.move_value();
}

myrd::OperationContext bounded_context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

template <class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

std::uint32_t error_code(const myrd::Error& error) {
    return error.code;
}

std::uint64_t payload_hash(std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto value : payload) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::byte> as_vector(
    const myrd::test::FakeBackingStore::BlockData& data) {
    return {data.begin(), data.end()};
}

constexpr auto kNoResult =
    (std::numeric_limits<std::uint32_t>::max)();

class SnapshotCoordinatorBarrier final
    : public myrd::ISnapshotCoordinatorNonReentrantBarrier {
public:
    explicit SnapshotCoordinatorBarrier(
        myrd::SnapshotCoordinatorBarrierEvent target) noexcept
        : target_(target) {}

    void pause_while_coordinator_locked(
        myrd::SnapshotCoordinatorBarrierEvent event) noexcept override {
        if (event != target_) {
            return;
        }
        reached_.store(true, std::memory_order_release);
        reached_.notify_all();
        release_.wait(false, std::memory_order_acquire);
    }

    bool reached() const noexcept {
        return reached_.load(std::memory_order_acquire);
    }

    void release() noexcept {
        release_.store(true, std::memory_order_release);
        release_.notify_all();
    }

private:
    myrd::SnapshotCoordinatorBarrierEvent target_;
    std::atomic<bool> reached_{};
    std::atomic<bool> release_{};
};

class Task07ManualClock final : public myrd::ISnapshotPressureClock {
public:
    explicit Task07ManualClock(
        std::chrono::steady_clock::time_point initial) noexcept
        : ticks_(initial.time_since_epoch().count()) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now()
        const noexcept override {
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{
                ticks_.load(std::memory_order_acquire)}};
    }

    void advance(std::chrono::milliseconds delta) noexcept {
        ticks_.fetch_add(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                delta)
                .count(),
            std::memory_order_acq_rel);
    }

private:
    std::atomic<std::chrono::steady_clock::duration::rep> ticks_;
};

class Task07OneRecordCowFixture final {
public:
    Task07OneRecordCowFixture() : options_(valid_options()) {
        options_.snapshot_cow_max_bytes =
            myrd::kMinimumSnapshotCowBytes;
    }

    [[nodiscard]] const myrd::DynamicBlockStoreOptions& options()
        const noexcept {
        return options_;
    }

    [[nodiscard]] std::unique_ptr<myrd::DynamicBlockStore> create_store(
        std::shared_ptr<const myrd::IBackingStore> backing,
        std::uint64_t logical_blocks = 2,
        myrd::BackingGeneration generation = 7) const {
        auto args = backing_args(
            std::move(backing), logical_blocks, generation);
        args.options.snapshot_cow_max_bytes =
            options_.snapshot_cow_max_bytes;
        args.options.snapshot_writer_stall_limit =
            options_.snapshot_writer_stall_limit;
        return require_store(std::move(args));
    }

private:
    myrd::DynamicBlockStoreOptions options_;
};

class Task07EventBarrier final {
public:
    void arrive_and_wait() {
        std::unique_lock lock(mutex_);
        arrived_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool wait_until_arrived(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(
            lock, deadline, [this] { return arrived_; });
    }

    void release() {
        std::scoped_lock lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool arrived_{};
    bool released_{};
};

class Task07CloseObserver final
    : public myrd::IDynamicBlockStoreLifecycleObserver {
public:
    void on_event(myrd::DynamicBlockStoreLifecycleEvent event) noexcept
        override {
        std::scoped_lock lock(mutex_);
        last_event_ = event;
        condition_.notify_all();
    }

    [[nodiscard]] bool wait_for(
        myrd::DynamicBlockStoreLifecycleEvent event,
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this, event] {
            return static_cast<std::uint8_t>(last_event_) >=
                   static_cast<std::uint8_t>(event);
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    myrd::DynamicBlockStoreLifecycleEvent last_event_{
        myrd::DynamicBlockStoreLifecycleEvent::WorkerStarted};
};

class Task07CompletionEvent final {
public:
    void signal() noexcept {
        std::scoped_lock lock(mutex_);
        signaled_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool wait(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return signaled_;
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{};
};

class Task07MultiPressureObserver final
    : public myrd::ISnapshotPressureTestObserver {
public:
    void on_snapshot_pressure_event(
        myrd::SnapshotPressureTestEvent event,
        std::uint64_t block_index) noexcept override {
        if (block_index >= states_.size()) {
            std::terminate();
        }
        std::unique_lock lock(mutex_);
        auto& state = states_[static_cast<std::size_t>(block_index)];
        if (event == myrd::SnapshotPressureTestEvent::CapacityWaitReady) {
            state.ready = true;
            condition_.notify_all();
            condition_.wait(lock, [&state] { return state.released; });
        } else if (
            event == myrd::SnapshotPressureTestEvent::CapacityWaitSleeping) {
            state.sleeping = true;
            condition_.notify_all();
        }
    }

    [[nodiscard]] bool wait_ready(
        std::uint64_t block_index,
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this, block_index] {
            return states_[static_cast<std::size_t>(block_index)].ready;
        });
    }

    [[nodiscard]] bool wait_sleeping(
        std::uint64_t block_index,
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this, block_index] {
            return states_[static_cast<std::size_t>(block_index)].sleeping;
        });
    }

    void release(std::uint64_t block_index) noexcept {
        std::scoped_lock lock(mutex_);
        states_[static_cast<std::size_t>(block_index)].released = true;
        condition_.notify_all();
    }

private:
    struct State final {
        bool ready{};
        bool sleeping{};
        bool released{};
    };

    std::mutex mutex_;
    std::condition_variable condition_;
    std::array<State, 4> states_{};
};

class Task07PreparingObserver final
    : public myrd::ISnapshotPressureTestObserver {
public:
    void on_snapshot_pressure_event(
        myrd::SnapshotPressureTestEvent event,
        std::uint64_t block_index) noexcept override {
        std::unique_lock lock(mutex_);
        block_index_ = block_index;
        if (event == myrd::SnapshotPressureTestEvent::PreparingPublished) {
            preparing_published_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return release_owner_; });
        } else if (
            event == myrd::SnapshotPressureTestEvent::PreparingWaitReady) {
            preparing_wait_ready_ = true;
            condition_.notify_all();
        } else if (
            event ==
            myrd::SnapshotPressureTestEvent::PreparingMarkerWaitReady) {
            preparing_marker_wait_ready_ = true;
            condition_.notify_all();
        }
    }

    [[nodiscard]] bool wait_published(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return preparing_published_;
        });
    }

    [[nodiscard]] bool wait_reader_ready(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return preparing_wait_ready_;
        });
    }

    [[nodiscard]] bool wait_marker_wait_ready(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return preparing_marker_wait_ready_;
        });
    }

    void release_owner() noexcept {
        std::scoped_lock lock(mutex_);
        release_owner_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::uint64_t block_index() const noexcept {
        std::scoped_lock lock(mutex_);
        return block_index_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t block_index_{};
    bool preparing_published_{};
    bool preparing_wait_ready_{};
    bool preparing_marker_wait_ready_{};
    bool release_owner_{};
};

class Task07ActiveFillObserver final
    : public myrd::ISnapshotPressureTestObserver {
public:
    void on_snapshot_pressure_event(
        myrd::SnapshotPressureTestEvent event,
        std::uint64_t block_index) noexcept override {
        if (event !=
            myrd::SnapshotPressureTestEvent::PreparingFillActive) {
            return;
        }
        std::unique_lock lock(mutex_);
        block_index_ = block_index;
        active_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool wait_active(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return active_;
        });
    }

    void release() noexcept {
        std::scoped_lock lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::uint64_t block_index() const noexcept {
        std::scoped_lock lock(mutex_);
        return block_index_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t block_index_{};
    bool active_{};
    bool released_{};
};

class Task07PressureObserver final
    : public myrd::ISnapshotPressureTestObserver {
public:
    enum class RaceResult : std::uint8_t {
        ReadyToSleep,
        WriterReturned,
        TimedOut,
    };

    void on_snapshot_pressure_event(
        myrd::SnapshotPressureTestEvent event,
        std::uint64_t block_index) noexcept override {
        std::unique_lock lock(mutex_);
        block_index_ = block_index;
        if (event == myrd::SnapshotPressureTestEvent::CapacityUnavailable) {
            capacity_unavailable_ = true;
        } else if (
            event == myrd::SnapshotPressureTestEvent::CapacityWaitReady) {
            ready_to_sleep_ = true;
        } else if (
            event == myrd::SnapshotPressureTestEvent::CapacityWaitSleeping) {
            sleeping_ = true;
        }
        condition_.notify_all();
        if (event == myrd::SnapshotPressureTestEvent::CapacityWaitReady) {
            condition_.wait(lock, [this] { return release_ready_event_; });
        }
    }

    void record_writer_returned() noexcept {
        std::scoped_lock lock(mutex_);
        writer_returned_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] RaceResult wait_for_ready_or_writer_returned(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_until(lock, deadline, [this] {
                return ready_to_sleep_ || writer_returned_;
            })) {
            return RaceResult::TimedOut;
        }
        return ready_to_sleep_ ? RaceResult::ReadyToSleep
                               : RaceResult::WriterReturned;
    }

    [[nodiscard]] bool capacity_unavailable() const noexcept {
        std::scoped_lock lock(mutex_);
        return capacity_unavailable_;
    }

    [[nodiscard]] bool wait_until_sleeping(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] {
            return sleeping_;
        });
    }

    [[nodiscard]] std::uint64_t block_index() const noexcept {
        std::scoped_lock lock(mutex_);
        return block_index_;
    }

    void release_ready_event() noexcept {
        std::scoped_lock lock(mutex_);
        release_ready_event_ = true;
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t block_index_{};
    bool capacity_unavailable_{};
    bool ready_to_sleep_{};
    bool sleeping_{};
    bool writer_returned_{};
    bool release_ready_event_{};
};

myrd::SnapshotPreimage metadata_preimage(
    std::uint64_t block_index,
    std::uint64_t version,
    myrd::DirtyRecordKind kind,
    myrd::BackingGeneration base_generation,
    std::uint64_t dirty_epoch) {
    return myrd::SnapshotPreimage{
        block_index,
        version,
        kind,
        base_generation,
        dirty_epoch,
        {},
    };
}

enum class LogicalDirtyKind : std::uint8_t {
    Data,
    Zero,
    Discard,
};

std::array<myrd::DataRange, 1> full_block_range(
    std::uint64_t block_index = 0) {
    return {{{block_index * myrd::kBlockSizeBytes,
              myrd::kBlockSizeBytes}}};
}

void publish_logical_kind(
    myrd::DynamicBlockStore& store,
    LogicalDirtyKind kind,
    std::uint64_t block_index,
    std::span<const std::byte> data) {
    const auto offset = block_index * myrd::kBlockSizeBytes;
    const auto range = full_block_range(block_index);
    switch (kind) {
    case LogicalDirtyKind::Data:
        REQUIRE_EQ(data.size(),
                   static_cast<std::size_t>(myrd::kBlockSizeBytes));
        REQUIRE(store.write(bounded_context(), offset, data).ok());
        return;
    case LogicalDirtyKind::Zero:
        REQUIRE(store.zero(bounded_context(), range).ok());
        return;
    case LogicalDirtyKind::Discard:
        REQUIRE(store.unmap(bounded_context(), range).ok());
        return;
    }
    REQUIRE(false);
}

myrd::DirtyRecordKind record_kind(LogicalDirtyKind kind) {
    switch (kind) {
    case LogicalDirtyKind::Data:
        return myrd::DirtyRecordKind::Data;
    case LogicalDirtyKind::Zero:
        return myrd::DirtyRecordKind::Zero;
    case LogicalDirtyKind::Discard:
        return myrd::DirtyRecordKind::Discard;
    }
    return static_cast<myrd::DirtyRecordKind>(0xFF);
}

} // namespace

TEST_CASE(m2_1_task07_pressure_fixture_freezes_exact_one_record_contract) {
    const auto initial_time = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds{1'000}};
    Task07ManualClock clock(initial_time);
    Task07OneRecordCowFixture fixture;
    Task07EventBarrier barrier;

    REQUIRE_EQ(fixture.options().snapshot_cow_max_bytes,
               myrd::kMinimumSnapshotCowBytes);
    REQUIRE_EQ(fixture.options().snapshot_writer_stall_limit,
               std::chrono::milliseconds{250});

    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = fixture.create_store(backing);
    std::vector<std::byte> cut_value(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x41});
    std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x52});
    REQUIRE(store->write(bounded_context(), 0, cut_value).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    REQUIRE(store->write(bounded_context(), 0, replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);

    clock.advance(std::chrono::milliseconds{37});
    REQUIRE_EQ(clock.now(), initial_time + std::chrono::milliseconds{37});

    std::thread waiter([&] { barrier.arrive_and_wait(); });
    REQUIRE(barrier.wait_until_arrived(
        std::chrono::steady_clock::now() + kWaitBound));
    barrier.release();
    waiter.join();
}

TEST_CASE(m2_1_task07_full_cow_budget_blocks_second_writer_before_publish) {
    Task07OneRecordCowFixture fixture;
    auto observer = std::make_shared<Task07PressureObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto first_cut = block_data(0x31);
    const auto second_cut = block_data(0x42);
    const auto first_replacement = block_data(0x53);
    const auto second_replacement = block_data(0x64);
    REQUIRE(store->write(bounded_context(), 0, first_cut).ok());
    REQUIRE(store->write(bounded_context(), myrd::kBlockSizeBytes,
                         second_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, first_replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);
    myrd::BlockEntry second_before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_before));

    std::stop_source writer_stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const myrd::OperationContext context{
            writer_stop.get_token(),
            std::chrono::steady_clock::time_point::max()};
        const auto result = store->write(
            context, myrd::kBlockSizeBytes, second_replacement);
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
        observer->record_writer_returned();
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        writer_stop.request_stop();
        observer->release_ready_event();
        if (writer.joinable()) {
            writer.join();
        }
    });

    const auto race = observer->wait_for_ready_or_writer_returned(
        std::chrono::steady_clock::now() + kWaitBound);
    REQUIRE_EQ(race, Task07PressureObserver::RaceResult::ReadyToSleep);
    REQUIRE(observer->capacity_unavailable());
    REQUIRE_EQ(observer->block_index(), 1ull);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), kNoResult);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        1ull);

    myrd::BlockEntry second_waiting{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_waiting));
    REQUIRE_EQ(second_waiting.version, second_before.version);
    REQUIRE_EQ(second_waiting.dirty_epoch, second_before.dirty_epoch);
    REQUIRE_EQ(second_waiting.dirty_base_generation,
               second_before.dirty_base_generation);
    REQUIRE_EQ(second_waiting.dirty_kind, second_before.dirty_kind);
}

TEST_CASE(m2_1_task07_capacity_wait_releases_the_block_stripe) {
    Task07OneRecordCowFixture fixture;
    auto observer = std::make_shared<Task07PressureObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto first_cut = block_data(0x71);
    const auto second_cut = block_data(0x82);
    REQUIRE(store->write(bounded_context(), 0, first_cut).ok());
    REQUIRE(store->write(bounded_context(), myrd::kBlockSizeBytes,
                         second_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0x93)).ok());

    std::stop_source writer_stop;
    std::jthread writer([&] {
        const myrd::OperationContext context{
            writer_stop.get_token(),
            std::chrono::steady_clock::time_point::max()};
        (void)store->write(
            context, myrd::kBlockSizeBytes, block_data(0xA4));
        observer->record_writer_returned();
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        writer_stop.request_stop();
        observer->release_ready_event();
        if (writer.joinable()) {
            writer.join();
        }
    });

    const auto race = observer->wait_for_ready_or_writer_returned(
        std::chrono::steady_clock::now() + kWaitBound);
    REQUIRE_EQ(race, Task07PressureObserver::RaceResult::ReadyToSleep);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        1ull);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::block_stripe_available(
        *store, 1));
}

TEST_CASE(m2_1_task07_batch_consumption_wakes_writer_without_lost_notify) {
    Task07OneRecordCowFixture fixture;
    auto observer = std::make_shared<Task07PressureObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto first_cut = block_data(0x15);
    const auto second_cut = block_data(0x26);
    const auto first_replacement = block_data(0x37);
    const auto second_replacement = block_data(0x48);
    REQUIRE(store->write(bounded_context(), 0, first_cut).ok());
    REQUIRE(store->write(bounded_context(), myrd::kBlockSizeBytes,
                         second_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, first_replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);

    std::stop_source writer_stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const myrd::OperationContext context{
            writer_stop.get_token(),
            std::chrono::steady_clock::time_point::max()};
        const auto result = store->write(
            context, myrd::kBlockSizeBytes, second_replacement);
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
        observer->record_writer_returned();
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        writer_stop.request_stop();
        observer->release_ready_event();
        if (writer.joinable()) {
            writer.join();
        }
    });

    REQUIRE_EQ(observer->wait_for_ready_or_writer_returned(
                   std::chrono::steady_clock::now() + kWaitBound),
               Task07PressureObserver::RaceResult::ReadyToSleep);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        1ull);

    myrd::DirtyScanCursor cursor{};
    const auto first_batch = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(first_batch.ok());
    REQUIRE_EQ(first_batch.value().size(), 1ull);
    REQUIRE_EQ(first_batch.value().front().block_index, 0ull);
    REQUIRE_EQ(first_batch.value().front().data, as_vector(first_cut));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);

    observer->release_ready_event();
    writer.join();
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        0ull);

    const auto second_batch = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(second_batch.ok());
    REQUIRE_EQ(second_batch.value().size(), 1ull);
    REQUIRE_EQ(second_batch.value().front().block_index, 1ull);
    REQUIRE_EQ(second_batch.value().front().data, as_vector(second_cut));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);

    std::vector<std::byte> current_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current_second).ok());
    REQUIRE_EQ(current_second, as_vector(second_replacement));
    REQUIRE(store->validate_invariants_for_test().ok());
    cleanup.release();
}

TEST_CASE(m2_1_task07_manual_clock_stall_aborts_once_and_writer_continues) {
    const auto initial_time = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds{10'000}};
    auto clock = std::make_shared<Task07ManualClock>(initial_time);
    auto observer = std::make_shared<Task07PressureObserver>();
    Task07OneRecordCowFixture fixture;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    dependencies.snapshot_pressure_clock = clock;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto first_cut = block_data(0x5A);
    const auto second_cut = block_data(0x6B);
    const auto second_replacement = block_data(0x7C);
    REQUIRE(store->write(bounded_context(), 0, first_cut).ok());
    REQUIRE(store->write(bounded_context(), myrd::kBlockSizeBytes,
                         second_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0x8D)).ok());

    std::stop_source writer_stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const myrd::OperationContext context{
            writer_stop.get_token(),
            std::chrono::steady_clock::time_point::max()};
        const auto result = store->write(
            context, myrd::kBlockSizeBytes, second_replacement);
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
        observer->record_writer_returned();
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        writer_stop.request_stop();
        observer->release_ready_event();
        myrd::DynamicBlockStoreTestAccess::notify_snapshot_capacity_waiters(
            *store);
        if (writer.joinable()) {
            writer.join();
        }
    });

    REQUIRE_EQ(observer->wait_for_ready_or_writer_returned(
                   std::chrono::steady_clock::now() + kWaitBound),
               Task07PressureObserver::RaceResult::ReadyToSleep);
    observer->release_ready_event();
    REQUIRE(observer->wait_until_sleeping(
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), kNoResult);

    clock->advance(fixture.options().snapshot_writer_stall_limit +
                   std::chrono::milliseconds{1});
    myrd::DynamicBlockStoreTestAccess::notify_snapshot_capacity_waiters(
        *store);
    writer.join();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.snapshot_active_state,
               std::optional{myrd::ConsistentSnapshotState::Aborted});
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    REQUIRE_EQ(stats.snapshot_writer_stalls_total, 1ull);
    REQUIRE_EQ(stats.snapshot_auto_aborts_total, 1ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        0ull);

    const auto status = store->consistent_snapshot_status(begun.value());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().state, myrd::ConsistentSnapshotState::Aborted);
    REQUIRE_EQ(status.value().blocked_writers, 0ull);
    myrd::DirtyScanCursor cursor{};
    const auto rejected_batch = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(!rejected_batch.ok());
    REQUIRE_EQ(error_code(rejected_batch.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    std::vector<std::byte> current_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current_second).ok());
    REQUIRE_EQ(current_second, as_vector(second_replacement));
    REQUIRE(store->validate_invariants_for_test().ok());
    cleanup.release();
}

TEST_CASE(m2_1_task07_close_wakes_capacity_waiter_then_releases_snapshot) {
    auto observer = std::make_shared<Task07PressureObserver>();
    auto lifecycle = std::make_shared<Task07CloseObserver>();
    Task07OneRecordCowFixture fixture;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    dependencies.lifecycle_observer = lifecycle;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    REQUIRE(store->write(bounded_context(), 0, block_data(0x91)).ok());
    REQUIRE(store->write(bounded_context(), myrd::kBlockSizeBytes,
                         block_data(0xA2)).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0xB3)).ok());

    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{},
            myrd::kBlockSizeBytes,
            block_data(0xC4));
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
        observer->record_writer_returned();
    });
    REQUIRE_EQ(observer->wait_for_ready_or_writer_returned(
                   std::chrono::steady_clock::now() + kWaitBound),
               Task07PressureObserver::RaceResult::ReadyToSleep);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        1ull);

    std::atomic<bool> close_returned{};
    std::jthread closer([&] {
        store->close();
        close_returned.store(true, std::memory_order_release);
    });
    REQUIRE(lifecycle->wait_for(
        myrd::DynamicBlockStoreLifecycleEvent::WaitersNotified,
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(!close_returned.load(std::memory_order_acquire));

    observer->release_ready_event();
    writer.join();
    closer.join();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE(close_returned.load(std::memory_order_acquire));
    REQUIRE(store->is_closed());
    const auto stats = store->stats();
    REQUIRE(!stats.snapshot_active_state.has_value());
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(*store),
        0ull);
    const auto finish_after_close =
        store->finish_consistent_snapshot(begun.value());
    REQUIRE(!finish_after_close.ok());
    REQUIRE_EQ(error_code(finish_after_close.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    const auto abort_after_close =
        store->abort_consistent_snapshot(begun.value());
    REQUIRE(!abort_after_close.ok());
    REQUIRE_EQ(error_code(abort_after_close.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
}

TEST_CASE(m2_1_task07_capacity_wait_cancel_and_deadline_leave_snapshot_intact) {
    for (const bool cancel : {true, false}) {
        const auto initial_time = std::chrono::steady_clock::now();
        auto clock = std::make_shared<Task07ManualClock>(initial_time);
        auto observer = std::make_shared<Task07PressureObserver>();
        Task07OneRecordCowFixture fixture;
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto args = backing_args(backing, 2, 7);
        args.options.snapshot_cow_max_bytes =
            fixture.options().snapshot_cow_max_bytes;
        args.options.snapshot_writer_stall_limit =
            fixture.options().snapshot_writer_stall_limit;
        myrd::DynamicBlockStoreTestDependencies dependencies{};
        dependencies.snapshot_pressure_test_observer = observer;
        dependencies.snapshot_pressure_clock = clock;
        auto created = myrd::DynamicBlockStore::create_for_testing(
            std::move(args), std::move(dependencies));
        REQUIRE(created.ok());
        auto store = created.move_value();

        const auto second_cut = block_data(cancel ? 0xD1 : 0xD2);
        REQUIRE(store->write(
            bounded_context(), 0, block_data(cancel ? 0xE1 : 0xE2)).ok());
        REQUIRE(store->write(
            bounded_context(), myrd::kBlockSizeBytes, second_cut).ok());
        const auto begun = store->begin_consistent_snapshot(bounded_context());
        REQUIRE(begun.ok());
        REQUIRE(store->write(
            bounded_context(), 0, block_data(cancel ? 0xF1 : 0xF2)).ok());

        std::stop_source writer_stop;
        std::atomic<std::uint32_t> writer_code{kNoResult};
        std::jthread writer([&] {
            const myrd::OperationContext context{
                writer_stop.get_token(),
                cancel
                    ? std::chrono::steady_clock::time_point::max()
                    : initial_time + std::chrono::milliseconds{100}};
            const auto result = store->write(
                context,
                myrd::kBlockSizeBytes,
                block_data(cancel ? 0xA1 : 0xA2));
            writer_code.store(
                result.ok() ? 0U : error_code(result.error()),
                std::memory_order_release);
            observer->record_writer_returned();
        });
        REQUIRE_EQ(observer->wait_for_ready_or_writer_returned(
                       std::chrono::steady_clock::now() + kWaitBound),
                   Task07PressureObserver::RaceResult::ReadyToSleep);
        observer->release_ready_event();
        REQUIRE(observer->wait_until_sleeping(
            std::chrono::steady_clock::now() + kWaitBound));

        myrd::DynamicBlockStoreTestAccess::notify_snapshot_capacity_waiters(
            *store);
        REQUIRE_EQ(
            myrd::DynamicBlockStoreTestAccess::snapshot_capacity_waiters(
                *store),
            1ull);
        REQUIRE_EQ(writer_code.load(std::memory_order_acquire), kNoResult);
        if (cancel) {
            writer_stop.request_stop();
        } else {
            clock->advance(std::chrono::milliseconds{101});
            myrd::DynamicBlockStoreTestAccess::notify_snapshot_capacity_waiters(
                *store);
        }
        writer.join();

        REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
                   static_cast<std::uint32_t>(
                       cancel ? myrd::OperationErrorCode::cancelled
                              : myrd::OperationErrorCode::timed_out));
        const auto status =
            store->consistent_snapshot_status(begun.value());
        REQUIRE(status.ok());
        REQUIRE_EQ(status.value().state,
                   myrd::ConsistentSnapshotState::Active);
        REQUIRE_EQ(status.value().blocked_writers, 0ull);
        REQUIRE_EQ(status.value().cow_bytes,
                   myrd::kMinimumSnapshotCowBytes);
        REQUIRE_EQ(store->stats().snapshot_writer_stalls_total, 0ull);
        REQUIRE_EQ(store->stats().snapshot_auto_aborts_total, 0ull);
        std::vector<std::byte> current_second(
            static_cast<std::size_t>(myrd::kBlockSizeBytes));
        REQUIRE(store->read(
            bounded_context(), myrd::kBlockSizeBytes, current_second).ok());
        REQUIRE_EQ(current_second, as_vector(second_cut));
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_1_task07_fifo_waiters_make_progress_one_cow_slot_at_a_time) {
    auto observer = std::make_shared<Task07MultiPressureObserver>();
    Task07OneRecordCowFixture fixture;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 3, 7);
    args.options.snapshot_cow_max_bytes =
        fixture.options().snapshot_cow_max_bytes;
    args.options.snapshot_writer_stall_limit =
        fixture.options().snapshot_writer_stall_limit;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto cut0 = block_data(0x11);
    const auto cut1 = block_data(0x22);
    const auto cut2 = block_data(0x33);
    const auto next1 = block_data(0x44);
    const auto next2 = block_data(0x55);
    REQUIRE(store->write(bounded_context(), 0, cut0).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, cut1).ok());
    REQUIRE(store->write(
        bounded_context(), 2 * myrd::kBlockSizeBytes, cut2).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0x66)).ok());

    std::stop_source writer_stop;
    std::atomic<std::uint32_t> code1{kNoResult};
    std::atomic<std::uint32_t> code2{kNoResult};
    Task07CompletionEvent done1;
    Task07CompletionEvent done2;
    const auto writer_body = [&](std::uint64_t block_index,
                                 const auto& replacement,
                                 std::atomic<std::uint32_t>& code,
                                 Task07CompletionEvent& done) {
        const myrd::OperationContext context{
            writer_stop.get_token(),
            std::chrono::steady_clock::time_point::max()};
        const auto result = store->write(
            context, block_index * myrd::kBlockSizeBytes, replacement);
        code.store(result.ok() ? 0U : error_code(result.error()),
                   std::memory_order_release);
        done.signal();
    };
    std::jthread writer1([&] { writer_body(1, next1, code1, done1); });
    REQUIRE(observer->wait_ready(
        1, std::chrono::steady_clock::now() + kWaitBound));
    std::jthread writer2([&] { writer_body(2, next2, code2, done2); });
    REQUIRE(observer->wait_ready(
        2, std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value())
                   .value()
                   .blocked_writers,
               2ull);

    observer->release(2);
    REQUIRE(observer->wait_sleeping(
        2, std::chrono::steady_clock::now() + kWaitBound));
    observer->release(1);
    REQUIRE(observer->wait_sleeping(
        1, std::chrono::steady_clock::now() + kWaitBound));
    myrd::DynamicBlockStoreTestAccess::notify_snapshot_capacity_waiters(
        *store);
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value())
                   .value()
                   .blocked_writers,
               2ull);

    myrd::DirtyScanCursor cursor{};
    const auto batch0 = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(batch0.ok());
    REQUIRE_EQ(batch0.value().front().block_index, 0ull);
    REQUIRE(done1.wait(std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(code1.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(code2.load(std::memory_order_acquire), kNoResult);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);

    const auto batch1 = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(batch1.ok());
    REQUIRE_EQ(batch1.value().front().block_index, 1ull);
    REQUIRE(done2.wait(std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(code2.load(std::memory_order_acquire), 0u);
    writer1.join();
    writer2.join();

    const auto batch2 = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(batch2.ok());
    REQUIRE_EQ(batch2.value().front().block_index, 2ull);
    REQUIRE_EQ(batch2.value().front().data, as_vector(cut2));
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value())
                   .value()
                   .blocked_writers,
               0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_task07_finish_and_abort_zero_current_resources_preserve_peak) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 1, 7));
    const auto first = block_data(0x61);
    REQUIRE(store->write(bounded_context(), 0, first).ok());

    const auto aborted = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(aborted.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0x72)).ok());
    const auto peak_after_abort_record =
        store->stats().snapshot_cow_peak_bytes;
    REQUIRE_EQ(peak_after_abort_record, myrd::kMinimumSnapshotCowBytes);
    REQUIRE(store->abort_consistent_snapshot(aborted.value()).ok());
    auto stats = store->stats();
    REQUIRE(!stats.snapshot_active_state.has_value());
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_peak_bytes, peak_after_abort_record);
    REQUIRE_EQ(stats.snapshot_writer_stalls_total, 0ull);
    REQUIRE_EQ(stats.snapshot_auto_aborts_total, 0ull);

    const auto finished = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(finished.ok());
    REQUIRE(store->write(bounded_context(), 0, block_data(0x83)).ok());
    myrd::DirtyScanCursor cursor{};
    const auto batch = store->copy_consistent_snapshot_batch(
        bounded_context(), finished.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(batch.ok());
    REQUIRE_EQ(batch.value().size(), 1ull);
    REQUIRE(store->finish_consistent_snapshot(finished.value()).ok());
    stats = store->stats();
    REQUIRE(!stats.snapshot_active_state.has_value());
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_peak_bytes, peak_after_abort_record);
    REQUIRE_EQ(stats.snapshot_writer_stalls_total, 0ull);
    REQUIRE_EQ(stats.snapshot_auto_aborts_total, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_task07_preparing_marker_serializes_reader_publish_and_rollback) {
    for (const bool cancel_owner : {false, true}) {
        auto observer = std::make_shared<Task07PreparingObserver>();
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto args = backing_args(backing, 1, 7);
        myrd::DynamicBlockStoreTestDependencies dependencies{};
        dependencies.snapshot_pressure_test_observer = observer;
        auto created = myrd::DynamicBlockStore::create_for_testing(
            std::move(args), std::move(dependencies));
        REQUIRE(created.ok());
        auto store = created.move_value();

        const auto cut = block_data(cancel_owner ? 0xA6 : 0xA5);
        const auto replacement = block_data(cancel_owner ? 0xB6 : 0xB5);
        REQUIRE(store->write(bounded_context(), 0, cut).ok());
        const auto begun = store->begin_consistent_snapshot(bounded_context());
        REQUIRE(begun.ok());

        std::stop_source writer_stop;
        std::atomic<std::uint32_t> writer_code{kNoResult};
        std::jthread writer([&] {
            const myrd::OperationContext context{
                writer_stop.get_token(),
                std::chrono::steady_clock::time_point::max()};
            const auto result = store->write(context, 0, replacement);
            writer_code.store(
                result.ok() ? 0U : error_code(result.error()),
                std::memory_order_release);
        });
        myrd::ScopeExit cleanup([&]() noexcept {
            writer_stop.request_stop();
            observer->release_owner();
            if (writer.joinable()) {
                writer.join();
            }
        });

        REQUIRE(observer->wait_published(
            std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE_EQ(observer->block_index(), 0ull);
        myrd::BlockEntry preparing{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, preparing));
        REQUIRE(preparing.snapshot_preparing_id != 0);
        REQUIRE(preparing.snapshot_reservation_id != 0);
        REQUIRE(myrd::DynamicBlockStoreTestAccess::block_stripe_available(
            *store, 0));
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
                   myrd::kMinimumSnapshotCowBytes);
        REQUIRE(store->validate_invariants_for_test().ok());

        myrd::DirtyScanCursor cursor{};
        std::optional<
            myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
        std::atomic<bool> reader_returned{};
        std::jthread reader([&] {
            copied.emplace(store->copy_consistent_snapshot_batch(
                bounded_context(),
                begun.value(),
                cursor,
                myrd::kBlockSizeBytes));
            reader_returned.store(true, std::memory_order_release);
        });
        REQUIRE(observer->wait_reader_ready(
            std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE(!reader_returned.load(std::memory_order_acquire));

        if (cancel_owner) {
            writer_stop.request_stop();
        }
        observer->release_owner();
        writer.join();
        reader.join();
        cleanup.release();

        REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
                   cancel_owner
                       ? static_cast<std::uint32_t>(
                             myrd::OperationErrorCode::cancelled)
                       : 0u);
        REQUIRE(copied.has_value());
        REQUIRE(copied->ok());
        REQUIRE_EQ(copied->value().size(), 1ull);
        REQUIRE_EQ(copied->value().front().block_index, 0ull);
        REQUIRE_EQ(copied->value().front().data, as_vector(cut));
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.snapshot_preparing_id, 0ull);
        REQUIRE_EQ(after.snapshot_reservation_id, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
        std::vector<std::byte> current(
            static_cast<std::size_t>(myrd::kBlockSizeBytes));
        REQUIRE(store->read(bounded_context(), 0, current).ok());
        REQUIRE_EQ(current,
                   as_vector(cancel_owner ? cut : replacement));
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_1_task07_metadata_preparing_marker_preserves_zero_and_discard) {
    for (const auto cut_kind :
         {LogicalDirtyKind::Zero, LogicalDirtyKind::Discard}) {
        auto observer = std::make_shared<Task07PreparingObserver>();
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto args = backing_args(backing, 1, 7);
        myrd::DynamicBlockStoreTestDependencies dependencies{};
        dependencies.snapshot_pressure_test_observer = observer;
        auto created = myrd::DynamicBlockStore::create_for_testing(
            std::move(args), std::move(dependencies));
        REQUIRE(created.ok());
        auto store = created.move_value();

        publish_logical_kind(*store, cut_kind, 0, {});
        const auto begun = store->begin_consistent_snapshot(bounded_context());
        REQUIRE(begun.ok());
        const auto replacement = block_data(
            cut_kind == LogicalDirtyKind::Zero ? 0x35 : 0x46);

        std::atomic<std::uint32_t> writer_code{kNoResult};
        std::jthread writer([&] {
            const auto result = store->write(
                myrd::OperationContext{}, 0, replacement);
            writer_code.store(
                result.ok() ? 0U : error_code(result.error()),
                std::memory_order_release);
        });
        myrd::ScopeExit cleanup([&]() noexcept {
            observer->release_owner();
            if (writer.joinable()) {
                writer.join();
            }
        });
        REQUIRE(observer->wait_published(
            std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::block_stripe_available(
            *store, 0));

        myrd::DirtyScanCursor cursor{};
        std::optional<
            myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
        std::atomic<bool> reader_returned{};
        std::jthread reader([&] {
            copied.emplace(store->copy_consistent_snapshot_batch(
                myrd::OperationContext{},
                begun.value(),
                cursor,
                myrd::kBlockSizeBytes));
            reader_returned.store(true, std::memory_order_release);
        });
        REQUIRE(observer->wait_reader_ready(
            std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE(!reader_returned.load(std::memory_order_acquire));

        observer->release_owner();
        writer.join();
        reader.join();
        cleanup.release();

        REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
        REQUIRE(copied.has_value());
        REQUIRE(copied->ok());
        REQUIRE_EQ(copied->value().size(), 1ull);
        REQUIRE_EQ(copied->value().front().block_index, 0ull);
        REQUIRE_EQ(copied->value().front().kind, record_kind(cut_kind));
        REQUIRE(copied->value().front().data.empty());
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.snapshot_preparing_id, 0ull);
        REQUIRE_EQ(after.snapshot_reservation_id, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
        std::vector<std::byte> current(
            static_cast<std::size_t>(myrd::kBlockSizeBytes));
        REQUIRE(store->read(bounded_context(), 0, current).ok());
        REQUIRE_EQ(current, as_vector(replacement));
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_1_task07_abort_resolves_coordinator_before_preparing_marker) {
    auto observer = std::make_shared<Task07PreparingObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 1, 7);
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto cut = block_data(0xC5);
    const auto replacement = block_data(0xD6);
    REQUIRE(store->write(bounded_context(), 0, cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, replacement);
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        observer->release_owner();
        if (writer.joinable()) {
            writer.join();
        }
    });
    REQUIRE(observer->wait_published(
        std::chrono::steady_clock::now() + kWaitBound));

    myrd::DirtyScanCursor cursor{};
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
    std::atomic<bool> reader_returned{};
    std::jthread reader([&] {
        copied.emplace(store->copy_consistent_snapshot_batch(
            myrd::OperationContext{},
            begun.value(),
            cursor,
            myrd::kBlockSizeBytes));
        reader_returned.store(true, std::memory_order_release);
    });
    REQUIRE(observer->wait_reader_ready(
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(store->abort_consistent_snapshot(begun.value()).ok());
    REQUIRE(observer->wait_marker_wait_ready(
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(!reader_returned.load(std::memory_order_acquire));

    observer->release_owner();
    writer.join();
    reader.join();
    cleanup.release();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE(copied.has_value());
    REQUIRE(!copied->ok());
    REQUIRE_EQ(error_code(copied->error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.snapshot_preparing_id, 0ull);
    REQUIRE_EQ(after.snapshot_reservation_id, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    std::vector<std::byte> current(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, current).ok());
    REQUIRE_EQ(current, as_vector(replacement));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_task07_close_drains_a_paused_preparing_owner) {
    auto observer = std::make_shared<Task07PreparingObserver>();
    auto lifecycle = std::make_shared<Task07CloseObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 1, 7);
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    dependencies.lifecycle_observer = lifecycle;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    REQUIRE(store->write(bounded_context(), 0, block_data(0xE7)).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, block_data(0xF8));
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
    });
    std::jthread closer;
    myrd::ScopeExit cleanup([&]() noexcept {
        observer->release_owner();
        if (writer.joinable()) {
            writer.join();
        }
        if (closer.joinable()) {
            closer.join();
        }
    });
    REQUIRE(observer->wait_published(
        std::chrono::steady_clock::now() + kWaitBound));

    std::atomic<bool> close_returned{};
    closer = std::jthread([&] {
        store->close();
        close_returned.store(true, std::memory_order_release);
    });
    REQUIRE(lifecycle->wait_for(
        myrd::DynamicBlockStoreLifecycleEvent::WaitersNotified,
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(!close_returned.load(std::memory_order_acquire));

    observer->release_owner();
    writer.join();
    closer.join();
    cleanup.release();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE(close_returned.load(std::memory_order_acquire));
    REQUIRE(store->is_closed());
    const auto stats = store->stats();
    REQUIRE(!stats.snapshot_active_state.has_value());
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_task07_abort_waits_for_active_data_fill_without_holding_locks) {
    auto observer = std::make_shared<Task07ActiveFillObserver>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 1, 7);
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.snapshot_pressure_test_observer = observer;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();

    const auto cut = block_data(0x19);
    const auto replacement = block_data(0x2A);
    REQUIRE(store->write(bounded_context(), 0, cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::jthread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, replacement);
        writer_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
    });
    std::atomic<std::uint32_t> abort_code{kNoResult};
    std::jthread aborter;
    myrd::ScopeExit cleanup([&]() noexcept {
        observer->release();
        if (writer.joinable()) {
            writer.join();
        }
        if (aborter.joinable()) {
            aborter.join();
        }
    });
    REQUIRE(observer->wait_active(
        std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(observer->block_index(), 0ull);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::block_stripe_available(
        *store, 0));

    aborter = std::jthread([&] {
        const auto result = store->abort_consistent_snapshot(begun.value());
        abort_code.store(
            result.ok() ? 0U : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        const auto status = store->consistent_snapshot_status(begun.value());
        return status.ok() &&
               status.value().state ==
                   myrd::ConsistentSnapshotState::Aborted;
    }));
    REQUIRE_EQ(abort_code.load(std::memory_order_acquire), kNoResult);
    REQUIRE(store->validate_invariants_for_test().ok());

    observer->release();
    writer.join();
    aborter.join();
    cleanup.release();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(abort_code.load(std::memory_order_acquire), 0u);
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.snapshot_preparing_id, 0ull);
    REQUIRE_EQ(after.snapshot_reservation_id, 0ull);
    const auto stats = store->stats();
    REQUIRE(!stats.snapshot_active_state.has_value());
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    std::vector<std::byte> current(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, current).ok());
    REQUIRE_EQ(current, as_vector(replacement));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_coordinator_owns_one_exact_token_and_cut_membership) {
    myrd::SnapshotCoordinator coordinator(
        4 * myrd::kSnapshotCowRecordBudgetBytes);

    auto activated = coordinator.activate(7, 10);
    REQUIRE(activated.ok());
    const auto token = activated.value();
    REQUIRE(token.snapshot_id != 0);
    REQUIRE_EQ(token.source_backing_generation, 7ull);
    REQUIRE_EQ(token.cutoff_dirty_epoch, 10ull);

    auto duplicate = coordinator.activate(8, 11);
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(error_code(duplicate.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));

    auto wrong_generation = token;
    wrong_generation.source_backing_generation = 8;
    auto foreign = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        wrong_generation,
        metadata_preimage(1, 3, myrd::DirtyRecordKind::Zero, 7, 10));
    REQUIRE(!foreign.ok());
    REQUIRE_EQ(error_code(foreign.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));

    auto member = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(1, 3, myrd::DirtyRecordKind::Zero, 7, 10));
    REQUIRE(member.ok());
    REQUIRE_EQ(member.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    auto published = coordinator.publish_metadata_preimage(
        bounded_context(), member.value());
    REQUIRE(published.ok());

    auto post_cut = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(2, 4, myrd::DirtyRecordKind::Discard, 7, 11));
    REQUIRE(post_cut.ok());
    REQUIRE_EQ(post_cut.value().disposition(),
               myrd::SnapshotPreimageDisposition::NotMember);

    auto future_generation = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(3, 5, myrd::DirtyRecordKind::Zero, 8, 9));
    REQUIRE(!future_generation.ok());
    REQUIRE_EQ(error_code(future_generation.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));
}

TEST_CASE(m2_1_coordinator_preserves_once_and_uses_only_monotonic_frontier) {
    myrd::SnapshotCoordinator coordinator(
        2 * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(9, 20);
    REQUIRE(activated.ok());
    const auto token = activated.value();

    auto first = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 11, myrd::DirtyRecordKind::Zero, 8, 20));
    REQUIRE(first.ok());
    REQUIRE(coordinator.publish_metadata_preimage(
        bounded_context(), first.value()).ok());

    auto duplicate = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 12, myrd::DirtyRecordKind::Discard, 9, 20));
    REQUIRE(duplicate.ok());
    REQUIRE_EQ(duplicate.value().disposition(),
               myrd::SnapshotPreimageDisposition::AlreadyPreserved);
    const auto saved = coordinator.peek_preimage(4);
    REQUIRE(saved.has_value());
    REQUIRE_EQ(saved->version, 11ull);
    REQUIRE_EQ(saved->kind, myrd::DirtyRecordKind::Zero);
    REQUIRE_EQ(saved->base_generation, 8ull);
    REQUIRE_EQ(saved->dirty_epoch, 20ull);
    REQUIRE(saved->data.empty());

    REQUIRE(coordinator.advance_scan_frontier(token, 0, 5, 1'000'000).ok());
    REQUIRE_EQ(coordinator.snapshot().next_scan_block, 5ull);
    REQUIRE(!coordinator.peek_preimage(4).has_value());

    auto consumed = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 13, myrd::DirtyRecordKind::Discard, 9, 20));
    REQUIRE(consumed.ok());
    REQUIRE_EQ(consumed.value().disposition(),
               myrd::SnapshotPreimageDisposition::Consumed);

    REQUIRE(coordinator.advance_scan_frontier(
        token, 5, 999'999, 1'000'000).ok());
    const auto after_large_advance = coordinator.snapshot();
    REQUIRE_EQ(after_large_advance.next_scan_block, 999'999ull);
    REQUIRE_EQ(after_large_advance.cow_current_bytes, 0ull);
    REQUIRE_EQ(after_large_advance.cow_records, 0ull);
    REQUIRE_EQ(after_large_advance.pending_reservations, 0ull);

    auto rewind = coordinator.advance_scan_frontier(
        token, 999'999, 999'998, 1'000'000);
    REQUIRE(!rewind.ok());
    REQUIRE_EQ(coordinator.snapshot().next_scan_block, 999'999ull);
}

TEST_CASE(m2_1_coordinator_charges_metadata_and_rolls_back_reservations) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(5, 8);
    REQUIRE(activated.ok());
    const auto token = activated.value();

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(1, 2, myrd::DirtyRecordKind::Discard, 5, 8));
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.cow_peak_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 1ull);
    REQUIRE(!coordinator.peek_preimage(1).has_value());
    REQUIRE(coordinator.validate_invariants_for_test().ok());

    auto exhausted = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 5, 8));
    REQUIRE(!exhausted.ok());
    REQUIRE_EQ(error_code(exhausted.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.pending_reservations, 1ull);

    coordinator.rollback_metadata_preimage(reserved.value());
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(1).has_value());
    REQUIRE(coordinator.validate_invariants_for_test().ok());

    auto cancelled_reservation = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(3, 4, myrd::DirtyRecordKind::Zero, 5, 8));
    REQUIRE(cancelled_reservation.ok());
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = coordinator.publish_metadata_preimage(
        myrd::OperationContext{
            stop.get_token(),
            std::chrono::steady_clock::time_point::max()},
        cancelled_reservation.value());
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(error_code(cancelled.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(3).has_value());
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_cancel_during_reservation_leaves_no_ghost_record_or_bytes) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ReservationInserted);
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes, barrier);
    auto activated = coordinator.activate(6, 12);
    REQUIRE(activated.ok());
    const auto token = activated.value();
    std::stop_source stop;
    std::atomic<std::uint32_t> reserve_code{kNoResult};

    std::thread reserver([&] {
        auto result = coordinator.reserve_metadata_before_mutation(
            myrd::OperationContext{
                stop.get_token(),
                std::chrono::steady_clock::time_point::max()},
            token,
            metadata_preimage(
                7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
        reserve_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] { return barrier->reached(); }));
    stop.request_stop();
    barrier->release();
    reserver.join();

    REQUIRE_EQ(reserve_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(7).has_value());
}

TEST_CASE(m2_1_pending_reservation_can_outlive_destroyed_coordinator) {
    alignas(myrd::SnapshotCoordinator)
        std::array<std::byte, sizeof(myrd::SnapshotCoordinator)> storage{};
    std::optional<myrd::SnapshotMetadataReservation> pending;
    auto* coordinator = std::construct_at(
        reinterpret_cast<myrd::SnapshotCoordinator*>(storage.data()),
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator->activate(6, 12);
    REQUIRE(activated.ok());
    auto reserved = coordinator->reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(
            7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    pending.emplace(reserved.move_value());
    REQUIRE_EQ(coordinator->snapshot().pending_reservations, 1ull);
    std::destroy_at(coordinator);

    // Reuse the exact address and reservation fields. A dangling raw owner
    // would now erase the replacement coordinator's unrelated reservation.
    auto* replacement = std::construct_at(
        reinterpret_cast<myrd::SnapshotCoordinator*>(storage.data()),
        myrd::kSnapshotCowRecordBudgetBytes);
    auto replacement_token = replacement->activate(6, 12);
    REQUIRE(replacement_token.ok());
    auto replacement_pending = replacement->reserve_metadata_before_mutation(
        bounded_context(),
        replacement_token.value(),
        metadata_preimage(
            7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
    REQUIRE(replacement_pending.ok());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 1ull);

    pending.reset();
    REQUIRE(!pending.has_value());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 1ull);
    replacement->rollback_metadata_preimage(replacement_pending.value());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 0ull);
    std::destroy_at(replacement);
}

TEST_CASE(m2_1_snapshot_token_id_is_unique_across_equal_coordinators) {
    myrd::SnapshotCoordinator owner(
        myrd::kSnapshotCowRecordBudgetBytes);
    myrd::SnapshotCoordinator stranger(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto owner_token = owner.activate(4, 6);
    auto stranger_token = stranger.activate(4, 6);
    REQUIRE(owner_token.ok());
    REQUIRE(stranger_token.ok());
    REQUIRE(owner_token.value().snapshot_id !=
            stranger_token.value().snapshot_id);

    auto foreign = stranger.reserve_metadata_before_mutation(
        bounded_context(),
        owner_token.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(!foreign.ok());
    REQUIRE_EQ(error_code(foreign.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_pending_duplicate_is_busy_until_rollback) {
    myrd::SnapshotCoordinator coordinator(
        2 * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(4, 6);
    REQUIRE(activated.ok());

    auto first = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(first.ok());
    auto duplicate = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 4, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(error_code(duplicate.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));

    coordinator.rollback_metadata_preimage(first.value());
    auto retry = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 5, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(retry.ok());
    REQUIRE_EQ(retry.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    coordinator.rollback_metadata_preimage(retry.value());
}

TEST_CASE(m2_1_wrong_coordinator_cannot_orphan_a_pending_reservation) {
    myrd::SnapshotCoordinator owner(
        myrd::kSnapshotCowRecordBudgetBytes);
    myrd::SnapshotCoordinator stranger(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto owner_token = owner.activate(4, 6);
    auto stranger_token = stranger.activate(8, 9);
    REQUIRE(owner_token.ok());
    REQUIRE(stranger_token.ok());

    auto reserved = owner.reserve_metadata_before_mutation(
        bounded_context(),
        owner_token.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(reserved.ok());
    auto wrong_publish = stranger.publish_metadata_preimage(
        bounded_context(), reserved.value());
    REQUIRE(!wrong_publish.ok());
    REQUIRE_EQ(error_code(wrong_publish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
    REQUIRE_EQ(owner.snapshot().pending_reservations, 1ull);

    owner.rollback_metadata_preimage(reserved.value());
    const auto after_rollback = owner.snapshot();
    REQUIRE_EQ(after_rollback.cow_current_bytes, 0ull);
    REQUIRE_EQ(after_rollback.pending_reservations, 0ull);
    REQUIRE(!owner.peek_preimage(2).has_value());
}

TEST_CASE(m2_1_coordinator_token_finish_abort_lifecycle_is_exact) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(3, 4);
    REQUIRE(activated.ok());
    const auto first = activated.value();

    auto early_finish = coordinator.finish(first, 2);
    REQUIRE(!early_finish.ok());
    REQUIRE_EQ(error_code(early_finish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotNotFullyScanned));
    REQUIRE(coordinator.status(first).ok());

    REQUIRE(coordinator.advance_scan_frontier(first, 0, 2, 2).ok());
    auto fully_scanned = coordinator.status(first);
    REQUIRE(fully_scanned.ok());
    REQUIRE_EQ(fully_scanned.value().state,
               myrd::ConsistentSnapshotState::FullyScanned);
    auto post_cut = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        first,
        metadata_preimage(
            0, 1, myrd::DirtyRecordKind::Zero, 3, 5));
    REQUIRE(post_cut.ok());
    REQUIRE_EQ(post_cut.value().disposition(),
               myrd::SnapshotPreimageDisposition::NotMember);
    REQUIRE(coordinator.finish(first, 2).ok());

    auto duplicate_finish = coordinator.finish(first, 2);
    REQUIRE(!duplicate_finish.ok());
    REQUIRE_EQ(error_code(duplicate_finish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));

    auto second_begin = coordinator.activate(4, 5);
    REQUIRE(second_begin.ok());
    REQUIRE(second_begin.value().snapshot_id > first.snapshot_id);
    const auto second = second_begin.value();
    REQUIRE(coordinator.abort(second).ok());
    auto duplicate_abort = coordinator.abort(second);
    REQUIRE(!duplicate_abort.ok());
    REQUIRE_EQ(error_code(duplicate_abort.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_coordinator_never_treats_data_as_metadata_only) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kMinimumSnapshotCowBytes);
    auto activated = coordinator.activate(2, 3);
    REQUIRE(activated.ok());
    auto data = metadata_preimage(
        0, 1, myrd::DirtyRecordKind::Data, 2, 3);

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(), activated.value(), data);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
}

TEST_CASE(m2_1_coordinator_rejects_unknown_dirty_record_kind) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(2, 3);
    REQUIRE(activated.ok());
    auto invalid = metadata_preimage(
        0,
        1,
        static_cast<myrd::DirtyRecordKind>(0xFF),
        2,
        3);

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(), activated.value(), invalid);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
}

TEST_CASE(m2_1_coordinator_map_has_bounded_logarithmic_probe_growth) {
    constexpr std::uint64_t kRecordCount = 10'000;
    myrd::SnapshotCoordinator coordinator(
        kRecordCount * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(9, 10);
    REQUIRE(activated.ok());

    for (std::uint64_t block = 0; block < kRecordCount; ++block) {
        auto reserved = coordinator.reserve_metadata_before_mutation(
            bounded_context(),
            activated.value(),
            metadata_preimage(
                block,
                block + 1,
                block % 2 == 0 ? myrd::DirtyRecordKind::Zero
                               : myrd::DirtyRecordKind::Discard,
                9,
                10));
        REQUIRE(reserved.ok());
        REQUIRE(coordinator.publish_metadata_preimage(
                    bounded_context(), reserved.value())
                    .ok());
    }

    const auto filled = coordinator.snapshot();
    REQUIRE_EQ(filled.cow_records, kRecordCount);
    REQUIRE_EQ(
        filled.cow_current_bytes,
        kRecordCount * myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
    const auto probes_after_build =
        coordinator.index_probe_count_for_test();
    REQUIRE(probes_after_build < kRecordCount * 160);

    for (std::uint64_t block = kRecordCount; block-- > 0;) {
        const auto preimage = coordinator.peek_preimage(block);
        REQUIRE(preimage.has_value());
        REQUIRE_EQ(preimage->block_index, block);
        REQUIRE_EQ(preimage->version, block + 1);
    }
    const auto reverse_lookup_probes =
        coordinator.index_probe_count_for_test() - probes_after_build;
    REQUIRE(reverse_lookup_probes < kRecordCount * 32);
    REQUIRE(coordinator.validate_invariants_for_test().ok());

    REQUIRE(coordinator.advance_scan_frontier(
                activated.value(), 0, kRecordCount, kRecordCount)
                .ok());
    const auto consumed = coordinator.snapshot();
    REQUIRE_EQ(consumed.cow_records, 0ull);
    REQUIRE_EQ(consumed.cow_current_bytes, 0ull);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_store_coordinator_lock_path_holds_no_stripe_or_state_lock) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ActivateLocked);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(13).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 13), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    std::atomic<std::uint32_t> begin_code{kNoResult};

    std::thread begin([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        begin_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] { return barrier->reached(); }));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::all_stripes_available(*store));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::state_mutex_available(*store));
    barrier->release();
    begin.join();
    REQUIRE_EQ(begin_code.load(std::memory_order_acquire), 0u);
}

TEST_CASE(m2_1_snapshot_public_contract_has_exact_types_and_signatures) {
    static_assert(std::is_trivially_copyable_v<myrd::ConsistentSnapshotToken>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::snapshot_id),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::source_backing_generation),
                  myrd::BackingGeneration>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::cutoff_dirty_epoch),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::DynamicBlockStoreStats::snapshot_active_state),
                  std::optional<myrd::ConsistentSnapshotState>>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::state),
                  myrd::ConsistentSnapshotState>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::cow_bytes),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::cow_records),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::blocked_writers),
                  std::uint64_t>);

    using BeginSignature = myrd::Result<myrd::ConsistentSnapshotToken>
        (myrd::DynamicBlockStore::*)(const myrd::OperationContext&);
    using CopySignature =
        myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>
        (myrd::DynamicBlockStore::*)(
            const myrd::OperationContext&,
            const myrd::ConsistentSnapshotToken&,
            myrd::DirtyScanCursor&,
            std::size_t);
    using FinishSignature = myrd::Result<void>
        (myrd::DynamicBlockStore::*)(
            const myrd::ConsistentSnapshotToken&);
    using StatusSignature = myrd::Result<myrd::ConsistentSnapshotStatus>
        (myrd::DynamicBlockStore::*)(
            const myrd::ConsistentSnapshotToken&) const;

    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::begin_consistent_snapshot),
                  BeginSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::copy_consistent_snapshot_batch),
                  CopySignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::finish_consistent_snapshot),
                  FinishSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::abort_consistent_snapshot),
                  FinishSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::consistent_snapshot_status),
                  StatusSignature>);
}

TEST_CASE(m2_1_snapshot_states_and_errors_have_stable_strings) {
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::Active),
               std::string_view("Active"));
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::Aborted),
               std::string_view("Aborted"));
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::FullyScanned),
               std::string_view("FullyScanned"));

    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotBusy),
               std::string_view("SnapshotBusy"));
    REQUIRE_EQ(
        myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid),
        std::string_view("SnapshotTokenInvalid"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotAborted),
               std::string_view("SnapshotAborted"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreErrorCode::SnapshotNotFullyScanned),
        std::string_view("SnapshotNotFullyScanned"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreErrorCode::SnapshotResourceExhausted),
        std::string_view("SnapshotResourceExhausted"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall),
        std::string_view("SnapshotCowBytesTooSmall"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive),
        std::string_view("SnapshotCowStallNotPositive"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge),
        std::string_view("SnapshotCowStallTooLarge"));
}

TEST_CASE(m2_1_snapshot_options_enforce_exact_unaligned_budget_boundary) {
    auto options = valid_options();
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_cow_max_bytes = myrd::kMinimumSnapshotCowBytes - 1;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall);

    options = valid_options();
    options.snapshot_cow_max_bytes = myrd::kMinimumSnapshotCowBytes + 1;
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());
}

TEST_CASE(m2_1_snapshot_stall_limit_is_strictly_positive_and_at_most_five_seconds) {
    auto options = valid_options();
    options.snapshot_writer_stall_limit = std::chrono::milliseconds{-1};
    REQUIRE_EQ(
        myrd::validate_dynamic_block_store_options(options).error,
        myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive);

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{0};
    REQUIRE_EQ(
        myrd::validate_dynamic_block_store_options(options).error,
        myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive);

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{1};
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{5'000};
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{5'001};
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge);
}

TEST_CASE(m2_1_cut_drains_the_whole_multiblock_mutation_and_does_not_block_reads) {
    auto first_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto second_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x11))
                       .data_block(1, block_data(0x22))
                       .data_block(3, block_data(0x44))
                       .gate_block(0, first_gate)
                       .gate_block(1, second_gate)
                       .build();
    auto store = require_store(backing_args(backing));

    std::vector<std::byte> spanning_write(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x5A});
    std::vector<std::byte> queued_write(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x6B});
    std::atomic<std::uint32_t> first_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<bool> cut_done{};
    std::atomic<bool> queued_done{};
    myrd::ConsistentSnapshotToken token{};

    std::thread first([&] {
        const auto result = store->write(
            bounded_context(), 512, spanning_write);
        first_code.store(result.ok() ? 0 : error_code(result.error()),
                         std::memory_order_release);
    });
    REQUIRE(first_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
               1ull);

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        if (result.ok()) {
            token = result.value();
            cut_code.store(0, std::memory_order_release);
        } else {
            cut_code.store(error_code(result.error()),
                           std::memory_order_release);
        }
        cut_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    // Reaching the gate accessor proves the waiting begin released the
    // lifecycle/admission mutex. Direct probes prove it also holds neither
    // state nor any block stripe while draining the admitted mutation.
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::state_mutex_available(
                   *store) &&
               myrd::DynamicBlockStoreTestAccess::all_stripes_available(
                   *store);
    }));

    std::atomic<std::uint32_t> overlapping_begin_code{kNoResult};
    std::thread overlapping_begin([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        overlapping_begin_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return overlapping_begin_code.load(std::memory_order_acquire) !=
               kNoResult;
    }));
    REQUIRE_EQ(overlapping_begin_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));
    overlapping_begin.join();

    std::thread queued([&] {
        const auto result = store->write(
            bounded_context(), 2 * myrd::kBlockSizeBytes, queued_write);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
        queued_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    std::vector<std::byte> read_output(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    const auto read = store->read(
        bounded_context(), 3 * myrd::kBlockSizeBytes, read_output);
    REQUIRE(read.ok());
    REQUIRE_EQ(read_output.front(), block_data(0x44).front());

    first_gate->open();
    REQUIRE(second_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(!cut_done.load(std::memory_order_acquire));
    REQUIRE(!queued_done.load(std::memory_order_acquire));

    second_gate->open();
    first.join();
    cut.join();
    queued.join();

    REQUIRE_EQ(first_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire), 0u);
    REQUIRE(token.snapshot_id != 0);
    REQUIRE_EQ(token.source_backing_generation, 7ull);
    REQUIRE_EQ(token.cutoff_dirty_epoch, 2ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.snapshot_active_id, token.snapshot_id);
    REQUIRE_EQ(stats.snapshot_active_state,
               std::optional{myrd::ConsistentSnapshotState::Active});
    REQUIRE_EQ(stats.snapshot_blocked_writers, 1ull);
}

TEST_CASE(m2_1_zero_and_unmap_hold_one_token_across_all_ranges) {
    for (const bool discard : std::array{false, true}) {
        auto first_gate =
            std::make_shared<myrd::test::FakeBackingStoreGate>();
        auto second_gate =
            std::make_shared<myrd::test::FakeBackingStoreGate>();
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, block_data(0x21))
                           .data_block(1, block_data(0x22))
                           .gate_block(0, first_gate)
                           .gate_block(1, second_gate)
                           .build();
        auto store = require_store(backing_args(backing));
        const std::array ranges{
            myrd::DataRange{0, 512},
            myrd::DataRange{myrd::kBlockSizeBytes, 512},
        };
        std::atomic<std::uint32_t> mutation_code{kNoResult};
        std::atomic<std::uint32_t> cut_code{kNoResult};
        std::atomic<bool> cut_done{};
        myrd::ConsistentSnapshotToken token{};

        std::thread mutation([&] {
            const auto result = discard
                ? store->unmap(bounded_context(), ranges)
                : store->zero(bounded_context(), ranges);
            mutation_code.store(
                result.ok() ? 0 : error_code(result.error()),
                std::memory_order_release);
        });
        REQUIRE(first_gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE_EQ(
            myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
            1ull);

        std::thread cut([&] {
            const auto result = store->begin_consistent_snapshot(
                bounded_context());
            if (result.ok()) {
                token = result.value();
                cut_code.store(0, std::memory_order_release);
            } else {
                cut_code.store(error_code(result.error()),
                               std::memory_order_release);
            }
            cut_done.store(true, std::memory_order_release);
        });
        REQUIRE(wait_until([&] {
            return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
                *store);
        }));

        first_gate->open();
        REQUIRE(second_gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE(!cut_done.load(std::memory_order_acquire));
        second_gate->open();

        mutation.join();
        cut.join();
        REQUIRE_EQ(mutation_code.load(std::memory_order_acquire), 0u);
        REQUIRE_EQ(cut_code.load(std::memory_order_acquire), 0u);
        REQUIRE_EQ(token.cutoff_dirty_epoch, 2ull);
    }
}

TEST_CASE(m2_1_cancelled_begin_reopens_admission_without_waiting_for_old_writer) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x31))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x71});
    std::vector<std::byte> queued_data(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x72});
    std::stop_source cut_stop;
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};
    std::atomic<bool> queued_done{};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            myrd::OperationContext{
                cut_stop.get_token(),
                std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));

    std::thread queued([&] {
        const auto result = store->write(
            bounded_context(), myrd::kBlockSizeBytes, queued_data);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
        queued_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    cut_stop.request_stop();
    REQUIRE(wait_until([&] {
        return cut_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE(wait_until([&] {
        return queued_done.load(std::memory_order_acquire);
    }));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire), 0u);
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);
    REQUIRE_EQ(store->stats().snapshot_active_id, 0ull);

    gate->open();
    old_writer.join();
    cut.join();
    queued.join();
}

TEST_CASE(m2_1_timed_out_begin_reopens_admission_after_old_writer_drains) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x35))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x75});
    std::atomic<std::uint32_t> cut_code{kNoResult};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            myrd::OperationContext{
                {}, std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(50)});
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    REQUIRE(wait_until([&] {
        return cut_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(store->stats().snapshot_active_id, 0ull);

    gate->open();
    old_writer.join();
    cut.join();
}

TEST_CASE(m2_1_mutation_admission_honors_cancellation_and_deadline) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x41))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x51});
    std::vector<std::byte> full(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x52});
    std::stop_source mutation_stop;
    std::atomic<std::uint32_t> cancelled_code{kNoResult};
    std::atomic<std::uint32_t> deadline_code{kNoResult};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    std::thread cut([&] {
        (void)store->begin_consistent_snapshot(bounded_context());
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));

    std::thread cancelled([&] {
        const auto result = store->write(
            myrd::OperationContext{
                mutation_stop.get_token(),
                std::chrono::steady_clock::now() + std::chrono::seconds(5)},
            myrd::kBlockSizeBytes,
            full);
        cancelled_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    std::thread deadline([&] {
        const std::array ranges{myrd::DataRange{
            2 * myrd::kBlockSizeBytes, myrd::kBlockSizeBytes}};
        const auto result = store->zero(
            myrd::OperationContext{
                {}, std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(50)},
            ranges);
        deadline_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        // snapshot_blocked_writers is cumulative, so unlike the instantaneous
        // waiter gauge it cannot be missed if the 50 ms deadline expires just
        // before this thread is scheduled.
        return store->stats().snapshot_blocked_writers >= 2;
    }));
    mutation_stop.request_stop();
    REQUIRE(wait_until([&] {
        return cancelled_code.load(std::memory_order_acquire) != kNoResult &&
               deadline_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(cancelled_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(deadline_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);

    gate->open();
    old_writer.join();
    cut.join();
    cancelled.join();
    deadline.join();
}

TEST_CASE(m2_1_close_wakes_cut_and_mutation_admission_waiters) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x61))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x62});
    std::atomic<std::uint32_t> old_writer_code{kNoResult};
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};

    std::thread old_writer([&] {
        const auto result = store->write(bounded_context(), 0, partial);
        old_writer_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    std::thread queued([&] {
        const std::array ranges{myrd::DataRange{
            myrd::kBlockSizeBytes, myrd::kBlockSizeBytes}};
        const auto result = store->unmap(
            bounded_context(), ranges);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    std::atomic<bool> close_done{};
    std::thread closer([&] {
        store->close();
        close_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return old_writer_code.load(std::memory_order_acquire) != kNoResult &&
               cut_code.load(std::memory_order_acquire) != kNoResult &&
               queued_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(old_writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    // The backing gate observes the close stop token, so the in-flight writer
    // may drain and let close finish before this thread is scheduled again.
    // Assert outcomes, not that racy scheduling window.
    gate->open();
    old_writer.join();
    cut.join();
    queued.join();
    closer.join();
    REQUIRE(close_done.load(std::memory_order_acquire));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
               0ull);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);
}

TEST_CASE(m2_1_only_one_concurrent_begin_can_own_the_active_snapshot) {
    auto backing = myrd::test::FakeBackingStoreBuilder(19).build();
    auto store = require_store(backing_args(backing, 2, 19));
    std::atomic<bool> start{};
    std::array<std::uint32_t, 2> codes{kNoResult, kNoResult};
    std::array<myrd::ConsistentSnapshotToken, 2> tokens{};
    std::array<std::thread, 2> begins;
    for (std::size_t index = 0; index < begins.size(); ++index) {
        begins[index] = std::thread([&, index] {
            start.wait(false, std::memory_order_acquire);
            const auto result = store->begin_consistent_snapshot(
                bounded_context());
            if (result.ok()) {
                tokens[index] = result.value();
                codes[index] = 0;
            } else {
                codes[index] = error_code(result.error());
            }
        });
    }
    start.store(true, std::memory_order_release);
    start.notify_all();
    for (auto& begin : begins) {
        begin.join();
    }

    const auto busy = static_cast<std::uint32_t>(
        myrd::DynamicBlockStoreErrorCode::SnapshotBusy);
    REQUIRE((codes[0] == 0 && codes[1] == busy) ||
            (codes[1] == 0 && codes[0] == busy));
    const auto winner = codes[0] == 0 ? 0u : 1u;
    REQUIRE(tokens[winner].snapshot_id != 0);
    REQUIRE_EQ(tokens[winner].source_backing_generation, 19ull);
    REQUIRE_EQ(tokens[winner].cutoff_dirty_epoch, 0ull);
}

TEST_CASE(m2_1_data_cow_preserves_resident_dirty_partial_write_preimage) {
    const auto backing_payload = block_data(0x31);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, backing_payload)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));

    auto reference = as_vector(backing_payload);
    const std::vector<std::byte> pre_cut_patch(4096, std::byte{0xA5});
    REQUIRE(store->write(bounded_context(), 1024, pre_cut_patch).ok());
    std::copy(pre_cut_patch.begin(), pre_cut_patch.end(),
              reference.begin() + 1024);

    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));
    REQUIRE_EQ(cut_entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(cut_entry.dirty_kind, myrd::DirtyRecordKind::Data);

    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    const std::vector<std::byte> post_cut_patch(2048, std::byte{0x5A});
    REQUIRE(store->write(bounded_context(), 2048, post_cut_patch).ok());

    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, preimage));
    REQUIRE_EQ(preimage.block_index, 0ull);
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.kind, cut_entry.dirty_kind);
    REQUIRE_EQ(preimage.base_generation,
               cut_entry.dirty_base_generation);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(preimage.data.size(),
               static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(reference));
    REQUIRE(preimage.data == reference);
}

TEST_CASE(m2_1_data_cow_preserves_only_first_full_block_preimage) {
    const auto backing_payload = block_data(0x42);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, backing_payload)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));

    std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x17});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    std::vector<std::byte> first_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x28});
    REQUIRE(store->write(bounded_context(), 0, first_replacement).ok());
    const auto bytes_after_first = store->stats().snapshot_cow_current_bytes;
    REQUIRE_EQ(bytes_after_first, myrd::kMinimumSnapshotCowBytes);

    std::vector<std::byte> second_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x39});
    REQUIRE(store->write(bounded_context(), 0, second_replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               bytes_after_first);

    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, preimage));
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(reference));
    REQUIRE(preimage.data == reference);
}

TEST_CASE(m2_1_data_cow_payload_allocation_failure_rolls_back_record) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    const auto begun = coordinator.activate(7, 1);
    REQUIRE(begun.ok());
    const auto payload = block_data(0x53);

    const auto reserved = coordinator.reserve_data_before_mutation(
        bounded_context(),
        begun.value(),
        metadata_preimage(
            0, 1, myrd::DirtyRecordKind::Data, 7, 1),
        payload);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    const auto state = coordinator.snapshot();
    REQUIRE_EQ(state.cow_current_bytes, 0ull);
    REQUIRE_EQ(state.cow_records, 0ull);
    REQUIRE_EQ(state.pending_reservations, 0ull);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_data_cow_preserves_each_member_of_cross_block_write) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x61))
                       .data_block(1, block_data(0x72))
                       .build();
    auto args = backing_args(backing, 3, 7);
    args.options.snapshot_cow_max_bytes =
        2 * myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));

    std::vector<std::byte> first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x18});
    std::vector<std::byte> second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x29});
    REQUIRE(store->write(bounded_context(), 0, first).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second).ok());
    myrd::BlockEntry first_cut{};
    myrd::BlockEntry second_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> spanning(1024, std::byte{0x7B});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes - 512, spanning).ok());

    myrd::SnapshotPreimage first_preimage{};
    myrd::SnapshotPreimage second_preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, first_preimage));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, second_preimage));
    REQUIRE_EQ(first_preimage.version, first_cut.version);
    REQUIRE_EQ(second_preimage.version, second_cut.version);
    REQUIRE_EQ(payload_hash(first_preimage.data), payload_hash(first));
    REQUIRE_EQ(payload_hash(second_preimage.data), payload_hash(second));
    REQUIRE(first_preimage.data == first);
    REQUIRE(second_preimage.data == second);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               2 * myrd::kMinimumSnapshotCowBytes);
}

TEST_CASE(m2_1_data_cow_consumed_member_does_not_allocate) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x83))
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const std::vector<std::byte> pre_cut(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x34});
    REQUIRE(store->write(bounded_context(), 0, pre_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::advance_snapshot_frontier(
        *store, begun.value(), 1));

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x45});
    REQUIRE(store->write(bounded_context(), 0, replacement).ok());
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
}

TEST_CASE(m2_1_clean_backing_at_cut_load_mutate_is_excluded_from_cow) {
    const auto original = block_data(0x94);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, original)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE_EQ(begun.value().cutoff_dirty_epoch, 0ull);

    const std::vector<std::byte> patch(512, std::byte{0x56});
    REQUIRE(store->write(bounded_context(), 1024, patch).ok());
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);

    auto reference = as_vector(original);
    std::copy(patch.begin(), patch.end(), reference.begin() + 1024);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(actual == reference);
}

TEST_CASE(m2_1_backing_load_failure_publishes_neither_mutation_nor_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xA5))
                       .fail_block(0)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> patch(512, std::byte{0x67});
    const auto written = store->write(bounded_context(), 1024, patch);
    REQUIRE(!written.ok());
    REQUIRE_EQ(error_code(written.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::backing_read_failed));
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry.dirty_epoch, 0ull);
}

TEST_CASE(m2_1_data_cow_stall_aborts_snapshot_and_writer_continues) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xB6))
                       .data_block(1, block_data(0xC7))
                       .build();
    auto store = require_store(backing_args(backing, 3, 7));
    const std::vector<std::byte> first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x78});
    const std::vector<std::byte> second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x89});
    REQUIRE(store->write(bounded_context(), 0, first).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> first_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x9A});
    REQUIRE(store->write(
        bounded_context(), 0, first_replacement).ok());
    myrd::BlockEntry second_before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_before));
    const std::vector<std::byte> second_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xAB});
    const auto written = store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_replacement);
    REQUIRE(written.ok());
    myrd::BlockEntry second_after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_after));
    REQUIRE_EQ(second_after.state, myrd::BlockState::ResidentDirty);
    REQUIRE(second_after.version > second_before.version);
    REQUIRE(second_after.dirty_epoch > second_before.dirty_epoch);
    REQUIRE_EQ(second_after.dirty_kind, myrd::DirtyRecordKind::Data);

    std::vector<std::byte> current_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current_second).ok());
    REQUIRE_EQ(payload_hash(current_second), payload_hash(second_replacement));
    REQUIRE(current_second == second_replacement);
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, unexpected));
    const auto stats = store->stats();
    REQUIRE_EQ(stats.snapshot_active_state,
               std::optional{myrd::ConsistentSnapshotState::Aborted});
    REQUIRE_EQ(stats.snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(stats.snapshot_cow_records, 0ull);
    REQUIRE_EQ(stats.snapshot_writer_stalls_total, 1ull);
    REQUIRE_EQ(stats.snapshot_auto_aborts_total, 1ull);
}

TEST_CASE(m2_1_published_preimage_remains_visible_across_reader_mutator_interleaving) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::PreimagePublished);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xD8))
                       .build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();

    const std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xBC});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xCD});
    std::atomic<std::uint32_t> writer_code{kNoResult};
    myrd::SnapshotPreimage observed{};
    std::atomic<bool> reader_entered{};
    std::atomic<bool> reader_found{};
    std::thread writer([&] {
        const auto result = store->write(
            bounded_context(), 0, replacement);
        writer_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    std::thread reader;
    myrd::ScopeExit cleanup([&]() noexcept {
        barrier->release();
        if (reader.joinable()) {
            reader.join();
        }
        if (writer.joinable()) {
            writer.join();
        }
    });
    const bool preimage_published =
        wait_until([&] { return barrier->reached(); });
    reader = std::thread([&] {
        reader_entered.store(true, std::memory_order_release);
        reader_found.store(
            myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
                *store, 0, observed),
            std::memory_order_release);
    });
    const bool reader_started = wait_until([&] {
        return reader_entered.load(std::memory_order_acquire);
    });
    barrier->release();
    reader.join();
    writer.join();
    cleanup.release();

    REQUIRE(preimage_published);
    REQUIRE(reader_started);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE(reader_found.load(std::memory_order_acquire));
    REQUIRE_EQ(payload_hash(observed.data), payload_hash(reference));
    REQUIRE(observed.data == reference);
}

TEST_CASE(m2_1_resident_dirty_kind_mismatch_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xE9))
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xDE});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_kind(
        *store, 0, myrd::DirtyRecordKind::Zero));

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xEF});
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(actual == reference);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
}

TEST_CASE(m2_1_resident_dirty_zero_epoch_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto reference = block_data(0x21);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store,
        0,
        myrd::BlockState::ResidentDirty,
        reference,
        7));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE_EQ(begun.value().cutoff_dirty_epoch, 0ull);

    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));
    REQUIRE_EQ(before.dirty_epoch, 0ull);
    const auto replacement = block_data(0x32);
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_resident_dirty_future_generation_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto reference = block_data(0x43);
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
        *store,
        0,
        begun.value().cutoff_dirty_epoch,
        begun.value().source_backing_generation + 1));

    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));
    REQUIRE(before.dirty_base_generation >
            begun.value().source_backing_generation);
    const auto replacement = block_data(0x54);
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_cross_block_cow_stall_aborts_then_completes_suffix) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 3, 7));
    const auto first_reference = block_data(0x65);
    const auto second_reference = block_data(0x76);
    REQUIRE(store->write(bounded_context(), 0, first_reference).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_reference).ok());
    myrd::BlockEntry first_cut{};
    myrd::BlockEntry second_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> spanning(1024, std::byte{0x87});
    const auto written = store->write(
        bounded_context(), myrd::kBlockSizeBytes - 512, spanning);
    REQUIRE(written.ok());

    auto expected_first = as_vector(first_reference);
    std::copy_n(spanning.begin(), 512, expected_first.end() - 512);
    auto expected_second = as_vector(second_reference);
    std::copy_n(spanning.begin() + 512, 512, expected_second.begin());
    std::vector<std::byte> actual_first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    std::vector<std::byte> actual_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual_first).ok());
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, actual_second).ok());
    REQUIRE_EQ(payload_hash(actual_first), payload_hash(expected_first));
    REQUIRE(actual_first == expected_first);
    REQUIRE_EQ(payload_hash(actual_second), payload_hash(expected_second));
    REQUIRE(actual_second == expected_second);

    myrd::BlockEntry first_after{};
    myrd::BlockEntry second_after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_after));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_after));
    REQUIRE(first_after.version > first_cut.version);
    REQUIRE(first_after.dirty_epoch > first_cut.dirty_epoch);
    REQUIRE_EQ(second_after.state, myrd::BlockState::ResidentDirty);
    REQUIRE(second_after.version > second_cut.version);
    REQUIRE(second_after.dirty_epoch > second_cut.dirty_epoch);

    myrd::SnapshotPreimage unexpected_first{};
    myrd::SnapshotPreimage unexpected_second{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected_first));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, unexpected_second));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    REQUIRE_EQ(store->stats().snapshot_writer_stalls_total, 1ull);
    REQUIRE_EQ(store->stats().snapshot_auto_aborts_total, 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_active_snapshot_hard_limit_load_mutate_excludes_clean_backing) {
    const auto first_backing = block_data(0x98);
    const auto second_backing = block_data(0xA9);
    const auto third_backing = block_data(0xBA);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, first_backing)
                       .data_block(1, second_backing)
                       .data_block(2, third_backing)
                       .build();
    auto args = backing_args(backing, 3, 7);
    args.options.soft_limit_bytes = myrd::kBlockSizeBytes;
    args.options.hard_limit_bytes = 2 * myrd::kBlockSizeBytes;
    args.options.eviction_low_watermark_bytes = myrd::kBlockSizeBytes;
    auto store = require_store(std::move(args));
    const auto first_resident = block_data(0xCB);
    const auto second_resident = block_data(0xDC);
    REQUIRE(store->write(bounded_context(), 0, first_resident).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_resident).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    const std::vector<std::byte> patch(512, std::byte{0xED});
    const auto rejected = store->write(
        bounded_context(), 2 * myrd::kBlockSizeBytes + 512, patch);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 2, after));
    REQUIRE_EQ(after.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(after.version, 0ull);
    REQUIRE_EQ(after.dirty_epoch, 0ull);
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 2, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);

    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), 2 * myrd::kBlockSizeBytes, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(third_backing));
    REQUIRE(std::equal(actual.begin(), actual.end(), third_backing.begin()));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_cancel_during_data_reservation_rolls_back_payload_and_write) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ReservationInserted);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    const auto reference = block_data(0xDC);
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));

    const auto replacement = block_data(0xED);
    std::stop_source stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{
                stop.get_token(),
                std::chrono::steady_clock::time_point::max()},
            0,
            replacement);
        writer_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        stop.request_stop();
        barrier->release();
        if (writer.joinable()) {
            writer.join();
        }
    });
    const bool reservation_reached =
        wait_until([&] { return barrier->reached(); });
    stop.request_stop();
    barrier->release();
    writer.join();
    cleanup.release();

    REQUIRE(reservation_reached);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_partial_final_block_cow_owns_zero_padded_full_payload) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.logical_size_bytes = myrd::kBlockSizeBytes + 512;
    auto store = require_store(std::move(args));
    const std::vector<std::byte> reference(512, std::byte{0x3E});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, reference).ok());
    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, cut_entry));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> replacement(512, std::byte{0x4F});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, replacement).ok());
    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, preimage));
    REQUIRE_EQ(preimage.data.size(),
               static_cast<std::size_t>(myrd::kBlockSizeBytes));
    std::vector<std::byte> expected(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0});
    std::copy(reference.begin(), reference.end(), expected.begin());
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(expected));
    REQUIRE(preimage.data == expected);
    REQUIRE(std::all_of(
        preimage.data.begin() + 512,
        preimage.data.end(),
        [](std::byte value) { return value == std::byte{0}; }));
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(preimage.base_generation,
               cut_entry.dirty_base_generation);
    std::vector<std::byte> current(512);
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current).ok());
    REQUIRE(current == replacement);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);
}

TEST_CASE(m2_1_zero_discard_transition_matrix_preserves_exact_cut_identity) {
    struct TransitionCase {
        LogicalDirtyKind before;
        LogicalDirtyKind after;
    };
    constexpr std::array transitions{
        TransitionCase{LogicalDirtyKind::Data, LogicalDirtyKind::Zero},
        TransitionCase{LogicalDirtyKind::Data, LogicalDirtyKind::Discard},
        TransitionCase{LogicalDirtyKind::Zero, LogicalDirtyKind::Data},
        TransitionCase{LogicalDirtyKind::Discard, LogicalDirtyKind::Data},
        TransitionCase{LogicalDirtyKind::Zero, LogicalDirtyKind::Discard},
        TransitionCase{LogicalDirtyKind::Discard, LogicalDirtyKind::Zero},
    };

    for (std::size_t index = 0; index < transitions.size(); ++index) {
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, block_data(
                               static_cast<std::uint8_t>(0x20 + index)))
                           .build();
        auto store = require_store(backing_args(backing, 2, 7));
        const auto old_data = block_data(
            static_cast<std::uint8_t>(0x40 + index));
        const auto new_data = block_data(
            static_cast<std::uint8_t>(0x70 + index));
        publish_logical_kind(
            *store, transitions[index].before, 0, old_data);

        myrd::BlockEntry cut_entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, cut_entry));
        REQUIRE_EQ(cut_entry.dirty_kind,
                   record_kind(transitions[index].before));
        const auto begun = store->begin_consistent_snapshot(
            bounded_context());
        REQUIRE(begun.ok());

        publish_logical_kind(
            *store, transitions[index].after, 0, new_data);

        myrd::SnapshotPreimage preimage{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
            *store, 0, preimage));
        REQUIRE_EQ(preimage.block_index, 0ull);
        REQUIRE_EQ(preimage.version, cut_entry.version);
        REQUIRE_EQ(preimage.kind, cut_entry.dirty_kind);
        REQUIRE_EQ(preimage.base_generation,
                   cut_entry.dirty_base_generation);
        REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
        if (transitions[index].before == LogicalDirtyKind::Data) {
            REQUIRE_EQ(preimage.data.size(),
                       static_cast<std::size_t>(myrd::kBlockSizeBytes));
            REQUIRE(std::equal(preimage.data.begin(), preimage.data.end(),
                               old_data.begin()));
            REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
                       myrd::kMinimumSnapshotCowBytes);
        } else {
            REQUIRE(preimage.data.empty());
            REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
                       myrd::kSnapshotCowRecordBudgetBytes);
        }

        myrd::BlockEntry after_entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after_entry));
        REQUIRE_EQ(after_entry.dirty_kind,
                   record_kind(transitions[index].after));
        REQUIRE(after_entry.version > cut_entry.version);
        REQUIRE(after_entry.dirty_epoch > cut_entry.dirty_epoch);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 1ull);
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_1_final_short_block_preserves_every_zero_discard_transition) {
    struct TransitionCase {
        LogicalDirtyKind before;
        LogicalDirtyKind after;
    };
    constexpr std::array transitions{
        TransitionCase{LogicalDirtyKind::Data, LogicalDirtyKind::Zero},
        TransitionCase{LogicalDirtyKind::Data, LogicalDirtyKind::Discard},
        TransitionCase{LogicalDirtyKind::Zero, LogicalDirtyKind::Data},
        TransitionCase{LogicalDirtyKind::Discard, LogicalDirtyKind::Data},
        TransitionCase{LogicalDirtyKind::Zero, LogicalDirtyKind::Discard},
        TransitionCase{LogicalDirtyKind::Discard, LogicalDirtyKind::Zero},
    };
    constexpr std::uint64_t final_bytes = 512;

    for (std::size_t index = 0; index < transitions.size(); ++index) {
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto args = backing_args(backing, 2, 7);
        args.logical_size_bytes = myrd::kBlockSizeBytes + final_bytes;
        auto store = require_store(std::move(args));
        const auto final_offset = myrd::kBlockSizeBytes;
        const std::array<myrd::DataRange, 1> final_range{{{
            final_offset, final_bytes}}};
        const std::vector<std::byte> old_data(
            static_cast<std::size_t>(final_bytes),
            std::byte{static_cast<std::uint8_t>(0x31 + index)});
        const std::vector<std::byte> new_data(
            static_cast<std::size_t>(final_bytes),
            std::byte{static_cast<std::uint8_t>(0x71 + index)});
        const auto publish_final = [&](LogicalDirtyKind kind,
                                       std::span<const std::byte> data) {
            if (kind == LogicalDirtyKind::Data) {
                REQUIRE(store->write(
                    bounded_context(), final_offset, data).ok());
            } else if (kind == LogicalDirtyKind::Zero) {
                REQUIRE(store->zero(bounded_context(), final_range).ok());
            } else {
                REQUIRE(store->unmap(bounded_context(), final_range).ok());
            }
        };

        publish_final(transitions[index].before, old_data);
        myrd::BlockEntry cut{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 1, cut));
        REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
        publish_final(transitions[index].after, new_data);

        myrd::SnapshotPreimage preimage{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
            *store, 1, preimage));
        REQUIRE_EQ(preimage.block_index, 1ull);
        REQUIRE_EQ(preimage.version, cut.version);
        REQUIRE_EQ(preimage.kind, record_kind(transitions[index].before));
        REQUIRE_EQ(preimage.base_generation, cut.dirty_base_generation);
        REQUIRE_EQ(preimage.dirty_epoch, cut.dirty_epoch);
        if (transitions[index].before == LogicalDirtyKind::Data) {
            REQUIRE_EQ(preimage.data.size(),
                       static_cast<std::size_t>(myrd::kBlockSizeBytes));
            REQUIRE(std::equal(old_data.begin(), old_data.end(),
                               preimage.data.begin()));
            REQUIRE(std::all_of(
                preimage.data.begin() +
                    static_cast<std::ptrdiff_t>(final_bytes),
                preimage.data.end(),
                [](std::byte value) { return value == std::byte{0}; }));
        } else {
            REQUIRE(preimage.data.empty());
        }

        myrd::BlockEntry current{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 1, current));
        REQUIRE_EQ(current.dirty_kind,
                   record_kind(transitions[index].after));
        std::vector<std::byte> actual(
            static_cast<std::size_t>(final_bytes), std::byte{0xFF});
        REQUIRE(store->read(
            bounded_context(), final_offset, actual).ok());
        if (transitions[index].after == LogicalDirtyKind::Data) {
            REQUIRE(actual == new_data);
        } else {
            REQUIRE(std::all_of(
                actual.begin(), actual.end(),
                [](std::byte value) { return value == std::byte{0}; }));
        }
    }
}

TEST_CASE(m2_1_partial_zero_unmap_normalize_data_and_preserve_full_preimage) {
    for (const auto kind : {myrd::DirtyRecordKind::Zero,
                            myrd::DirtyRecordKind::Discard}) {
        const auto predecessor = block_data(
            kind == myrd::DirtyRecordKind::Zero ? 0x31 : 0x42);
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, predecessor)
                           .build();
        auto store = require_store(backing_args(backing, 2, 7));
        const auto reference = block_data(
            kind == myrd::DirtyRecordKind::Zero ? 0x53 : 0x64);
        REQUIRE(store->write(bounded_context(), 0, reference).ok());
        myrd::BlockEntry cut_entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, cut_entry));
        REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

        const std::array<myrd::DataRange, 1> partial{{{512, 1024}}};
        const auto mutated = kind == myrd::DirtyRecordKind::Zero
            ? store->zero(bounded_context(), partial)
            : store->unmap(bounded_context(), partial);
        REQUIRE(mutated.ok());

        myrd::SnapshotPreimage preimage{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
            *store, 0, preimage));
        REQUIRE_EQ(preimage.kind, myrd::DirtyRecordKind::Data);
        REQUIRE_EQ(preimage.version, cut_entry.version);
        REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
        REQUIRE(std::equal(preimage.data.begin(), preimage.data.end(),
                           reference.begin()));

        auto expected = as_vector(reference);
        std::fill(expected.begin() + 512,
                  expected.begin() + 1536,
                  std::byte{0});
        std::vector<std::byte> actual(
            static_cast<std::size_t>(myrd::kBlockSizeBytes));
        REQUIRE(store->read(bounded_context(), 0, actual).ok());
        REQUIRE(actual == expected);
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(after.dirty_kind, myrd::DirtyRecordKind::Data);
    }
}

TEST_CASE(m2_1_clean_backing_and_implicit_zero_post_cut_ranges_are_excluded) {
    for (const bool implicit_zero : {false, true}) {
        for (const auto kind : {myrd::DirtyRecordKind::Zero,
                                myrd::DirtyRecordKind::Discard}) {
            auto backing = myrd::test::FakeBackingStoreBuilder(7)
                               .data_block(0, block_data(0x75))
                               .build();
            auto args = backing_args(backing, 2, 7);
            if (implicit_zero) {
                args.initial_state = myrd::InitialBlockState::CleanZero;
            }
            auto store = require_store(std::move(args));
            const auto begun = store->begin_consistent_snapshot(
                bounded_context());
            REQUIRE(begun.ok());
            REQUIRE_EQ(begun.value().cutoff_dirty_epoch, 0ull);

            const auto range = full_block_range();
            const auto mutated = kind == myrd::DirtyRecordKind::Zero
                ? store->zero(bounded_context(), range)
                : store->unmap(bounded_context(), range);
            REQUIRE(mutated.ok());
            myrd::SnapshotPreimage unexpected{};
            REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
                *store, 0, unexpected));
            REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
            REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
            REQUIRE_EQ(backing->total_load_count(), 0ull);
            myrd::BlockEntry after{};
            REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                *store, 0, after));
            REQUIRE_EQ(after.state, myrd::BlockState::ZeroDirty);
            REQUIRE_EQ(after.dirty_kind, kind);
        }
    }
}

TEST_CASE(m2_1_clean_backing_partial_range_is_data_but_not_cut_member) {
    for (const auto kind : {myrd::DirtyRecordKind::Zero,
                            myrd::DirtyRecordKind::Discard}) {
        const auto predecessor = block_data(
            kind == myrd::DirtyRecordKind::Zero ? 0x86 : 0x97);
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, predecessor)
                           .build();
        auto store = require_store(backing_args(backing, 2, 7));
        REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
        const std::array<myrd::DataRange, 1> partial{{{1024, 512}}};
        const auto mutated = kind == myrd::DirtyRecordKind::Zero
            ? store->zero(bounded_context(), partial)
            : store->unmap(bounded_context(), partial);
        REQUIRE(mutated.ok());
        REQUIRE_EQ(backing->block_load_count(0), 1ull);
        myrd::SnapshotPreimage unexpected{};
        REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
            *store, 0, unexpected));
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(after.dirty_kind, myrd::DirtyRecordKind::Data);
        REQUIRE(after.dirty_epoch > 0);
    }
}

TEST_CASE(m2_1_partial_range_on_zero_override_is_a_true_noop) {
    for (const auto before_kind : {LogicalDirtyKind::Zero,
                                   LogicalDirtyKind::Discard}) {
        for (const auto operation_kind : {myrd::DirtyRecordKind::Zero,
                                          myrd::DirtyRecordKind::Discard}) {
            auto backing = myrd::test::FakeBackingStoreBuilder(7)
                               .data_block(0, block_data(0xA8))
                               .build();
            auto store = require_store(backing_args(backing, 2, 7));
            publish_logical_kind(
                *store, before_kind, 0, block_data(0xB9));
            myrd::BlockEntry before{};
            REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                *store, 0, before));
            REQUIRE(store->begin_consistent_snapshot(
                bounded_context()).ok());

            const std::array<myrd::DataRange, 1> partial{{{512, 512}}};
            const auto result = operation_kind == myrd::DirtyRecordKind::Zero
                ? store->zero(bounded_context(), partial)
                : store->unmap(bounded_context(), partial);
            REQUIRE(result.ok());
            myrd::BlockEntry after{};
            REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                *store, 0, after));
            REQUIRE_EQ(after.state, before.state);
            REQUIRE_EQ(after.version, before.version);
            REQUIRE_EQ(after.dirty_kind, before.dirty_kind);
            REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
            REQUIRE_EQ(after.dirty_base_generation,
                       before.dirty_base_generation);
            myrd::SnapshotPreimage unexpected{};
            REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
                *store, 0, unexpected));
            REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
            REQUIRE_EQ(backing->total_load_count(), 0ull);
        }
    }
}

TEST_CASE(m2_1_metadata_preimage_is_exact_once_and_consumed_is_suppressed) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 3, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 0, block_data(0xA1));
    publish_logical_kind(
        *store, LogicalDirtyKind::Discard, 1, block_data(0xB2));
    myrd::BlockEntry first_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::advance_snapshot_frontier(
        *store, begun.value(), 1));

    publish_logical_kind(
        *store, LogicalDirtyKind::Discard, 0, block_data(0xC3));
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 1, block_data(0xD4));
    const auto bytes_after_first = store->stats().snapshot_cow_current_bytes;
    publish_logical_kind(
        *store, LogicalDirtyKind::Discard, 1, block_data(0xE5));

    myrd::SnapshotPreimage consumed{};
    myrd::SnapshotPreimage preserved{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, consumed));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, preserved));
    REQUIRE_EQ(preserved.kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(preserved.data.empty());
    REQUIRE_EQ(store->stats().snapshot_cow_records, 1ull);
    REQUIRE_EQ(bytes_after_first,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               bytes_after_first);
}

TEST_CASE(m2_1_zero_dirty_invalid_identity_fails_closed_before_transition) {
    enum class InvalidIdentity : std::uint8_t {
        Kind,
        Epoch,
        Generation,
    };
    for (const auto invalid : {InvalidIdentity::Kind,
                               InvalidIdentity::Epoch,
                               InvalidIdentity::Generation}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto store = require_store(backing_args(backing, 2, 7));
        publish_logical_kind(
            *store, LogicalDirtyKind::Zero, 0, block_data(0xF6));
        const auto begun = store->begin_consistent_snapshot(
            bounded_context());
        REQUIRE(begun.ok());
        if (invalid == InvalidIdentity::Kind) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_kind(
                *store, 0, myrd::DirtyRecordKind::Data));
        } else {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
                *store,
                0,
                invalid == InvalidIdentity::Epoch
                    ? 0
                    : begun.value().cutoff_dirty_epoch,
                invalid == InvalidIdentity::Generation
                    ? begun.value().source_backing_generation + 1
                    : begun.value().source_backing_generation));
        }
        myrd::BlockEntry before{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, before));

        const auto range = full_block_range();
        const auto rejected = store->unmap(bounded_context(), range);
        REQUIRE(!rejected.ok());
        REQUIRE_EQ(error_code(rejected.error()),
                   static_cast<std::uint32_t>(
                       myrd::DynamicBlockStoreErrorCode::
                           FatalInternalInvariant));
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.state, before.state);
        REQUIRE_EQ(after.version, before.version);
        REQUIRE_EQ(after.dirty_kind, before.dirty_kind);
        REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
        REQUIRE_EQ(after.dirty_base_generation,
                   before.dirty_base_generation);
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    }
}

TEST_CASE(m2_1_zero_dirty_invalid_identity_fails_closed_before_partial_noop) {
    enum class InvalidIdentity : std::uint8_t {
        Kind,
        Epoch,
        Generation,
    };
    for (const auto invalid : {InvalidIdentity::Kind,
                               InvalidIdentity::Epoch,
                               InvalidIdentity::Generation}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
        auto store = require_store(backing_args(backing, 2, 7));
        publish_logical_kind(
            *store, LogicalDirtyKind::Zero, 0, block_data(0x07));
        const auto begun = store->begin_consistent_snapshot(
            bounded_context());
        REQUIRE(begun.ok());
        if (invalid == InvalidIdentity::Kind) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_kind(
                *store, 0, myrd::DirtyRecordKind::Data));
        } else {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
                *store,
                0,
                invalid == InvalidIdentity::Epoch
                    ? 0
                    : begun.value().cutoff_dirty_epoch,
                invalid == InvalidIdentity::Generation
                    ? begun.value().source_backing_generation + 1
                    : begun.value().source_backing_generation));
        }
        myrd::BlockEntry before{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, before));

        const std::array<myrd::DataRange, 1> partial{{{512, 512}}};
        const auto rejected = store->unmap(bounded_context(), partial);
        REQUIRE(!rejected.ok());
        REQUIRE_EQ(error_code(rejected.error()),
                   static_cast<std::uint32_t>(
                       myrd::DynamicBlockStoreErrorCode::
                           FatalInternalInvariant));
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.state, before.state);
        REQUIRE_EQ(after.version, before.version);
        REQUIRE_EQ(after.dirty_kind, before.dirty_kind);
        REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
        REQUIRE_EQ(after.dirty_base_generation,
                   before.dirty_base_generation);
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    }
}

TEST_CASE(m2_1_cancelled_metadata_reservation_rolls_back_transition) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ReservationInserted);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 0, block_data(0x17));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));

    std::stop_source stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::thread writer([&] {
        const auto range = full_block_range();
        const auto result = store->unmap(
            myrd::OperationContext{
                stop.get_token(),
                std::chrono::steady_clock::time_point::max()},
            range);
        writer_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    myrd::ScopeExit cleanup([&]() noexcept {
        stop.request_stop();
        barrier->release();
        if (writer.joinable()) {
            writer.join();
        }
    });
    const bool reservation_reached =
        wait_until([&] { return barrier->reached(); });
    stop.request_stop();
    barrier->release();
    writer.join();
    cleanup.release();

    REQUIRE(reservation_reached);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_kind, before.dirty_kind);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_snapshot_reader_interleaves_with_successful_metadata_publish) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::PreimagePublished);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 0, block_data(0x27));
    myrd::BlockEntry cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::atomic<bool> reader_started{};
    std::atomic<bool> reader_finished{};
    myrd::SnapshotPreimage observed{};
    std::thread writer([&] {
        const auto result = store->unmap(
            bounded_context(), full_block_range());
        writer_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    std::thread reader;
    myrd::ScopeExit cleanup([&]() noexcept {
        barrier->release();
        if (writer.joinable()) {
            writer.join();
        }
        if (reader.joinable()) {
            reader.join();
        }
    });
    const bool writer_reached_publish =
        wait_until([&] { return barrier->reached(); });
    if (writer_reached_publish) {
        reader = std::thread([&] {
            reader_started.store(true, std::memory_order_release);
            reader_started.notify_all();
            if (myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
                    *store, 0, observed)) {
                reader_finished.store(true, std::memory_order_release);
            }
        });
        reader_started.wait(false, std::memory_order_acquire);
    }
    const bool reader_reached_lookup = writer_reached_publish &&
        wait_until([&] {
            return myrd::DynamicBlockStoreTestAccess::
                snapshot_preimage_lookup_entered(*store);
        });
    barrier->release();
    if (writer.joinable()) {
        writer.join();
    }
    if (reader.joinable()) {
        reader.join();
    }
    cleanup.release();

    REQUIRE(writer_reached_publish);
    REQUIRE(reader_started.load(std::memory_order_acquire));
    REQUIRE(reader_reached_lookup);
    REQUIRE(reader_finished.load(std::memory_order_acquire));
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(observed.kind, myrd::DirtyRecordKind::Zero);
    REQUIRE_EQ(observed.version, cut.version);
    REQUIRE_EQ(observed.dirty_epoch, cut.dirty_epoch);
    REQUIRE(observed.data.empty());
    myrd::BlockEntry current{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, current));
    REQUIRE_EQ(current.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(current.dirty_kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(current.version > cut.version);
}

TEST_CASE(m2_1_metadata_cow_stall_aborts_snapshot_and_unmap_continues) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 3, 7);
    args.options.snapshot_cow_max_bytes =
        myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));
    const auto first_reference = block_data(0x18);
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, first_reference);
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 1, block_data(0x29));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const auto first_replacement = block_data(0x3A);
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, first_replacement);
    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, before));
    const auto range = full_block_range(1);
    const auto unmapped = store->unmap(bounded_context(), range);
    REQUIRE(unmapped.ok());

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, after));
    REQUIRE_EQ(after.state, myrd::BlockState::ZeroDirty);
    REQUIRE(after.version > before.version);
    REQUIRE_EQ(after.dirty_kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(after.dirty_epoch > before.dirty_epoch);
    myrd::SnapshotPreimage first_preimage{};
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, first_preimage));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    REQUIRE_EQ(store->stats().snapshot_writer_stalls_total, 1ull);
    REQUIRE_EQ(store->stats().snapshot_auto_aborts_total, 1ull);
}

TEST_CASE(m2_1_cross_range_cow_stall_aborts_then_completes_range) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 3, 7);
    args.options.snapshot_cow_max_bytes =
        myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));
    publish_logical_kind(
        *store, LogicalDirtyKind::Zero, 0, block_data(0x28));
    const auto second_reference = block_data(0x39);
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 1, second_reference);
    myrd::BlockEntry first_cut{};
    myrd::BlockEntry second_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::array<myrd::DataRange, 1> spanning{{{
        0, 2 * myrd::kBlockSizeBytes}}};
    const auto unmapped = store->unmap(bounded_context(), spanning);
    REQUIRE(unmapped.ok());

    myrd::BlockEntry first_after{};
    myrd::BlockEntry second_after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_after));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_after));
    REQUIRE_EQ(first_after.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(first_after.dirty_kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(first_after.version > first_cut.version);
    REQUIRE_EQ(second_after.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(second_after.dirty_kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(second_after.version > second_cut.version);
    REQUIRE(second_after.dirty_epoch > second_cut.dirty_epoch);

    myrd::SnapshotPreimage first_preimage{};
    myrd::SnapshotPreimage second_preimage{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, first_preimage));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, second_preimage));
    std::vector<std::byte> second_actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, second_actual).ok());
    REQUIRE(std::all_of(second_actual.begin(), second_actual.end(),
                        [](std::byte value) { return value == std::byte{}; }));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_writer_stalls_total, 1ull);
    REQUIRE_EQ(store->stats().snapshot_auto_aborts_total, 1ull);
}

TEST_CASE(m2_1_partial_range_backing_failure_leaves_no_cow_or_mutation) {
    for (const auto kind : {myrd::DirtyRecordKind::Zero,
                            myrd::DirtyRecordKind::Discard}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, block_data(0x4A))
                           .fail_block(0)
                           .build();
        auto store = require_store(backing_args(backing, 2, 7));
        REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
        const std::array<myrd::DataRange, 1> partial{{{512, 512}}};
        const auto rejected = kind == myrd::DirtyRecordKind::Zero
            ? store->zero(bounded_context(), partial)
            : store->unmap(bounded_context(), partial);
        REQUIRE(!rejected.ok());
        REQUIRE_EQ(error_code(rejected.error()),
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::backing_read_failed));
        myrd::BlockEntry after{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, after));
        REQUIRE_EQ(after.state, myrd::BlockState::Unresident);
        REQUIRE_EQ(after.version, 0ull);
        REQUIRE_EQ(after.dirty_epoch, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
        REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    }
}

TEST_CASE(m2_1_zero_discard_exact_ack_keeps_predecessor_data_unreachable) {
    for (const auto kind : {myrd::DirtyRecordKind::Zero,
                            myrd::DirtyRecordKind::Discard}) {
        auto predecessor = myrd::test::FakeBackingStoreBuilder(7)
                               .data_block(0, block_data(0x5B))
                               .build();
        auto store = require_store(backing_args(predecessor, 2, 7));
        const auto before_kind = kind == myrd::DirtyRecordKind::Zero
            ? LogicalDirtyKind::Discard
            : LogicalDirtyKind::Zero;
        publish_logical_kind(
            *store, before_kind, 0, block_data(0x6C));
        myrd::BlockEntry cut{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, cut));
        const auto begun = store->begin_consistent_snapshot(bounded_context());
        REQUIRE(begun.ok());
        publish_logical_kind(
            *store,
            kind == myrd::DirtyRecordKind::Zero
                ? LogicalDirtyKind::Zero
                : LogicalDirtyKind::Discard,
            0,
            block_data(0x7D));

        myrd::SnapshotPreimage cut_record{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
            *store, 0, cut_record));
        REQUIRE_EQ(cut_record.kind, record_kind(before_kind));
        REQUIRE_EQ(cut_record.version, cut.version);
        REQUIRE_EQ(cut_record.dirty_epoch, cut.dirty_epoch);
        REQUIRE(cut_record.data.empty());

        myrd::DirtyScanCursor cursor{};
        const auto copied = store->copy_consistent_snapshot_batch(
            bounded_context(),
            begun.value(),
            cursor,
            myrd::kBlockSizeBytes);
        REQUIRE(copied.ok());
        REQUIRE_EQ(copied.value().size(), 1ull);
        const auto record = copied.value().front();
        REQUIRE_EQ(record.kind, cut_record.kind);
        REQUIRE_EQ(record.version, cut_record.version);
        REQUIRE(record.data.empty());
        REQUIRE_EQ(cursor.next_block_index, store->total_blocks());

        auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(
                             *predecessor)
                             .all_zero_block(0)
                             .build();
        REQUIRE(store->install_backing_view({8, successor}).ok());
        const auto stale_ack = store->mark_persisted(
            cut_record.block_index,
            cut_record.version,
            cut_record.kind,
            8);
        REQUIRE(!stale_ack.ok());
        REQUIRE_EQ(error_code(stale_ack.error()),
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::version_changed));
        myrd::BlockEntry still_dirty{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, still_dirty));
        REQUIRE_EQ(still_dirty.state, myrd::BlockState::ZeroDirty);
        REQUIRE(still_dirty.version != record.version);
        REQUIRE_EQ(still_dirty.dirty_kind, kind);
        REQUIRE(store->finish_consistent_snapshot(begun.value()).ok());
        REQUIRE(store->mark_persisted(
            0,
            still_dirty.version,
            still_dirty.dirty_kind,
            8).ok());
        myrd::BlockEntry persisted{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, persisted));
        REQUIRE_EQ(persisted.state, myrd::BlockState::ZeroClean);
        REQUIRE_EQ(persisted.clean_generation, 8ull);
        const auto eviction =
            myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
        REQUIRE_EQ(eviction.evicted_blocks, 0ull);

        std::vector<std::byte> actual(
            static_cast<std::size_t>(myrd::kBlockSizeBytes),
            std::byte{0xFF});
        REQUIRE(store->read(bounded_context(), 0, actual).ok());
        REQUIRE(std::all_of(actual.begin(), actual.end(),
                            [](std::byte value) {
                                return value == std::byte{0};
                            }));
        REQUIRE_EQ(predecessor->total_load_count(), 0ull);
        REQUIRE_EQ(successor->total_load_count(), 0ull);
    }
}

TEST_CASE(m2_1_task06_streams_one_exact_cut_in_ascending_batches) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 6, 7);
    args.options.snapshot_cow_max_bytes =
        2 * myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));
    const auto cut_data = block_data(0x31);
    publish_logical_kind(*store, LogicalDirtyKind::Data, 0, cut_data);
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 1, {});
    publish_logical_kind(*store, LogicalDirtyKind::Discard, 2, {});

    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0x42));
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 2, {});
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 4, block_data(0x53));

    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 3ull);
    REQUIRE_EQ(copied.value()[0].block_index, 0ull);
    REQUIRE_EQ(copied.value()[0].kind, myrd::DirtyRecordKind::Data);
    REQUIRE_EQ(copied.value()[0].data, as_vector(cut_data));
    REQUIRE_EQ(copied.value()[1].block_index, 1ull);
    REQUIRE_EQ(copied.value()[1].kind, myrd::DirtyRecordKind::Zero);
    REQUIRE(copied.value()[1].data.empty());
    REQUIRE_EQ(copied.value()[2].block_index, 2ull);
    REQUIRE_EQ(copied.value()[2].kind, myrd::DirtyRecordKind::Discard);
    REQUIRE(copied.value()[2].data.empty());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());

    const auto status = store->consistent_snapshot_status(begun.value());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().state,
               myrd::ConsistentSnapshotState::FullyScanned);
    REQUIRE_EQ(status.value().cow_bytes, 0ull);
    REQUIRE_EQ(status.value().cow_records, 0ull);
    REQUIRE_EQ(store->stats().snapshot_consistent_batches_total, 1ull);
    REQUIRE_EQ(store->stats().snapshot_consistent_records_total, 3ull);
    REQUIRE(store->finish_consistent_snapshot(begun.value()).ok());

    const auto stale_copy = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(!stale_copy.ok());
    REQUIRE_EQ(error_code(stale_copy.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_task06_cursor_and_payload_limits_never_duplicate_or_skip) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 4, 7));
    const auto first_data = block_data(0x61);
    const auto second_data = block_data(0x72);
    publish_logical_kind(*store, LogicalDirtyKind::Data, 0, first_data);
    publish_logical_kind(*store, LogicalDirtyKind::Data, 2, second_data);
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    myrd::DirtyScanCursor wrong_cursor{1};
    const auto rejected = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        wrong_cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(wrong_cursor.next_block_index, 1ull);

    myrd::DirtyScanCursor cursor{};
    const auto first = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value().size(), 1ull);
    REQUIRE_EQ(first.value().front().block_index, 0ull);
    REQUIRE_EQ(first.value().front().data, as_vector(first_data));
    REQUIRE_EQ(cursor.next_block_index, 2ull);

    const auto second = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(second.ok());
    REQUIRE_EQ(second.value().size(), 1ull);
    REQUIRE_EQ(second.value().front().block_index, 2ull);
    REQUIRE_EQ(second.value().front().data, as_vector(second_data));
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE(store->finish_consistent_snapshot(begun.value()).ok());
}

TEST_CASE(m2_1_task06_concurrent_batches_commit_the_frontier_exactly_once) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 1, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0x6D));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        pause_consistent_snapshot_after_first_record(*store));

    myrd::DirtyScanCursor first_cursor{};
    myrd::DirtyScanCursor second_cursor{};
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> first;
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> second;
    std::thread first_reader([&] {
        first.emplace(store->copy_consistent_snapshot_batch(
            bounded_context(), begun.value(), first_cursor,
            myrd::kBlockSizeBytes));
    });
    std::thread second_reader([&] {
        second.emplace(store->copy_consistent_snapshot_batch(
            bounded_context(), begun.value(), second_cursor,
            myrd::kBlockSizeBytes));
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::
                   consistent_snapshot_records_staged(*store) == 2;
    }));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        release_consistent_snapshot_record_pause(*store));
    first_reader.join();
    second_reader.join();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto successes = static_cast<unsigned>(first->ok()) +
                           static_cast<unsigned>(second->ok());
    REQUIRE_EQ(successes, 1u);
    const auto& winner = first->ok() ? *first : *second;
    const auto& loser = first->ok() ? *second : *first;
    const auto winner_cursor = first->ok()
        ? first_cursor.next_block_index
        : second_cursor.next_block_index;
    const auto loser_cursor = first->ok()
        ? second_cursor.next_block_index
        : first_cursor.next_block_index;
    REQUIRE_EQ(winner.value().size(), 1ull);
    REQUIRE_EQ(winner.value().front().block_index, 0ull);
    REQUIRE_EQ(winner_cursor, store->total_blocks());
    REQUIRE(!loser.ok());
    REQUIRE_EQ(error_code(loser.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));
    REQUIRE_EQ(loser_cursor, 0ull);
}

TEST_CASE(m2_1_task06_checks_payload_limit_before_copying_the_next_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.options.snapshot_cow_max_bytes =
        2 * myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0x71));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 1, block_data(0x82));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0x93));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 1, block_data(0xA4));
    myrd::DynamicBlockStoreTestAccess::
        reset_consistent_snapshot_payload_copy_bytes(*store);

    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 1ull);
    REQUIRE_EQ(cursor.next_block_index, 1ull);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::
                   consistent_snapshot_payload_copy_bytes(*store),
               myrd::kBlockSizeBytes);
}

TEST_CASE(m2_1_task06_final_short_data_block_is_zero_padded) {
    auto options = valid_options();
    auto store = require_store(myrd::DynamicBlockStoreCreateArgs{
        myrd::kBlockSizeBytes + 512,
        512,
        options,
        myrd::InitialBlockState::CleanZero,
        {},
        7,
    });
    std::array<std::byte, 512> tail{};
    std::fill(tail.begin(), tail.end(), std::byte{0xA7});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, tail).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 1ull);
    REQUIRE_EQ(copied.value().front().block_index, 1ull);
    REQUIRE_EQ(copied.value().front().data.size(),
               static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(std::all_of(
        copied.value().front().data.begin(),
        copied.value().front().data.begin() + 512,
        [](std::byte value) { return value == std::byte{0xA7}; }));
    REQUIRE(std::all_of(
        copied.value().front().data.begin() + 512,
        copied.value().front().data.end(),
        [](std::byte value) { return value == std::byte{0}; }));
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
}

TEST_CASE(m2_1_task06_returns_safe_partial_then_surfaces_cancellation) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 4, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0x81));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 1, block_data(0x92));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        pause_consistent_snapshot_after_first_record(*store));

    std::stop_source stop;
    myrd::DirtyScanCursor cursor{};
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
    std::thread reader([&] {
        copied.emplace(store->copy_consistent_snapshot_batch(
            {stop.get_token(),
             std::chrono::steady_clock::now() + std::chrono::seconds(5)},
            begun.value(),
            cursor,
            2 * myrd::kBlockSizeBytes));
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::
            consistent_snapshot_record_staged(*store);
    }));
    stop.request_stop();
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        release_consistent_snapshot_record_pause(*store));
    reader.join();

    REQUIRE(copied.has_value());
    REQUIRE(copied->ok());
    REQUIRE_EQ(copied->value().size(), 1ull);
    REQUIRE_EQ(copied->value().front().block_index, 0ull);
    REQUIRE_EQ(cursor.next_block_index, 1ull);
    const auto before_retry = cursor.next_block_index;
    const auto cancelled = store->copy_consistent_snapshot_batch(
        {stop.get_token(),
         std::chrono::steady_clock::now() + std::chrono::seconds(5)},
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(error_code(cancelled.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(cursor.next_block_index, before_retry);
}

TEST_CASE(m2_1_task06_pre_first_record_cancellation_has_zero_progress) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 3, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 2, block_data(0xA3));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    std::stop_source stop;
    stop.request_stop();
    myrd::DirtyScanCursor cursor{};

    const auto cancelled = store->copy_consistent_snapshot_batch(
        {stop.get_token(),
         std::chrono::steady_clock::now() + std::chrono::seconds(5)},
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(error_code(cancelled.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    const auto status = store->consistent_snapshot_status(begun.value());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().state, myrd::ConsistentSnapshotState::Active);
}

TEST_CASE(m2_1_task06_cancellation_wakes_transient_wait_with_infinite_deadline) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xB4))
                       .gate_block(0, gate)
                       .build();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_before_snapshot_wait = true;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 1, 7), std::move(dependencies));
    REQUIRE(created.ok());
    auto store = created.move_value();
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    std::vector<std::byte> patch(512, std::byte{0xC5});
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::thread writer([&] {
        const auto result = store->write(bounded_context(), 0, patch);
        writer_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));

    std::stop_source stop;
    myrd::DirtyScanCursor cursor{};
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
    std::atomic<bool> reader_done{};
    std::thread reader([&] {
        copied.emplace(store->copy_consistent_snapshot_batch(
            {stop.get_token(),
             std::chrono::steady_clock::time_point::max()},
            begun.value(),
            cursor,
            myrd::kBlockSizeBytes));
        reader_done.store(true, std::memory_order_release);
    });
    const bool wait_gap_reached = wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::snapshot_wait_gap_reached(
            *store);
    });
    std::thread canceller([&] { stop.request_stop(); });
    const bool cancellation_started = wait_until([&] {
        return stop.stop_requested();
    });
    const bool wait_gap_released =
        myrd::DynamicBlockStoreTestAccess::release_snapshot_wait_gap(*store);
    const bool cancellation_woke_reader = wait_until([&] {
        return reader_done.load(std::memory_order_acquire);
    });

    // Always release the backing load before asserting so a failing red test
    // cannot strand either worker in the test process.
    gate->open();
    canceller.join();
    writer.join();
    reader.join();

    REQUIRE(wait_gap_reached);
    REQUIRE(cancellation_started);
    REQUIRE(wait_gap_released);
    REQUIRE(cancellation_woke_reader);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE(copied.has_value());
    REQUIRE(!copied->ok());
    REQUIRE_EQ(error_code(copied->error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    const auto status = store->consistent_snapshot_status(begun.value());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().state, myrd::ConsistentSnapshotState::Active);
    const auto retry = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor, myrd::kBlockSizeBytes);
    REQUIRE(retry.ok());
    REQUIRE(retry.value().empty());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
}

TEST_CASE(m2_1_task06_close_releases_consistent_snapshot_record_pause) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 1, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0xD6));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        pause_consistent_snapshot_after_first_record(*store));

    myrd::DirtyScanCursor cursor{};
    std::optional<myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>> copied;
    std::thread reader([&] {
        copied.emplace(store->copy_consistent_snapshot_batch(
            myrd::OperationContext{},
            begun.value(),
            cursor,
            myrd::kBlockSizeBytes));
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::
            consistent_snapshot_record_staged(*store);
    }));

    std::atomic<bool> close_done{};
    std::thread closer([&] {
        store->close();
        close_done.store(true, std::memory_order_release);
    });
    const bool close_released_pause = wait_until([&] {
        return close_done.load(std::memory_order_acquire);
    });

    // On the old implementation this is the cleanup path. On the fixed
    // implementation close already cleared the pause and this is a no-op.
    (void)myrd::DynamicBlockStoreTestAccess::
        release_consistent_snapshot_record_pause(*store);
    reader.join();
    closer.join();

    REQUIRE(close_released_pause);
    REQUIRE(copied.has_value());
    REQUIRE(!copied->ok());
    REQUIRE_EQ(error_code(copied->error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
}

TEST_CASE(m2_1_task06_record_limit_and_public_abort_are_exact) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 3, 7);
    args.options.max_snapshot_batch_records = 1;
    auto store = require_store(std::move(args));
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 0, {});
    publish_logical_kind(*store, LogicalDirtyKind::Discard, 1, {});
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    myrd::DirtyScanCursor cursor{};
    const auto first = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value().size(), 1ull);
    REQUIRE_EQ(first.value().front().block_index, 0ull);
    REQUIRE_EQ(cursor.next_block_index, 1ull);
    REQUIRE(store->abort_consistent_snapshot(begun.value()).ok());

    const auto duplicate = store->abort_consistent_snapshot(begun.value());
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(error_code(duplicate.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
    const auto stale_status = store->consistent_snapshot_status(begun.value());
    REQUIRE(!stale_status.ok());
    REQUIRE_EQ(error_code(stale_status.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));

    const auto second_begin =
        store->begin_consistent_snapshot(bounded_context());
    REQUIRE(second_begin.ok());
    REQUIRE(second_begin.value().snapshot_id > begun.value().snapshot_id);
}

TEST_CASE(m2_1_task06_blocks_publication_until_the_cut_is_fully_scanned) {
    auto predecessor = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(predecessor, 2, 7));
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 0, {});
    myrd::BlockEntry cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    const auto early_ack = store->mark_persisted(
        0, cut.version, cut.dirty_kind, 8);
    REQUIRE(!early_ack.ok());
    REQUIRE_EQ(error_code(early_ack.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(
                         *predecessor)
                         .all_zero_block(0)
                         .build();
    const auto early_install = store->install_backing_view({8, successor});
    REQUIRE(!early_install.ok());
    REQUIRE_EQ(error_code(early_install.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));

    const auto early_finish = store->finish_consistent_snapshot(begun.value());
    REQUIRE(!early_finish.ok());
    REQUIRE_EQ(error_code(early_finish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotNotFullyScanned));
    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 1ull);
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE(store->install_backing_view({8, successor}).ok());
    REQUIRE(store->mark_persisted(
        copied.value().front().block_index,
        copied.value().front().version,
        copied.value().front().kind,
        8).ok());
    REQUIRE(store->finish_consistent_snapshot(begun.value()).ok());
    const auto duplicate_abort = store->abort_consistent_snapshot(begun.value());
    REQUIRE(!duplicate_abort.ok());
    REQUIRE_EQ(error_code(duplicate_abort.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_task06_ack_rechecks_the_cutoff_dirty_epoch) {
    auto predecessor = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(predecessor, 2, 7));
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 0, {});
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 1ull);
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
        *store,
        0,
        begun.value().cutoff_dirty_epoch + 1,
        copied.value().front().base_generation));

    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(
                         *predecessor)
                         .all_zero_block(0)
                         .build();
    REQUIRE(store->install_backing_view({8, successor}).ok());
    const auto rejected = store->mark_persisted(
        copied.value().front().block_index,
        copied.value().front().version,
        copied.value().front().kind,
        8);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::version_changed));
    myrd::BlockEntry still_dirty{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, still_dirty));
    REQUIRE_EQ(still_dirty.state, myrd::BlockState::ZeroDirty);
}

TEST_CASE(m2_1_task06_mutation_continues_while_token_is_fully_scanned) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0xC5));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value()).value().state,
               myrd::ConsistentSnapshotState::FullyScanned);

    const auto replacement = block_data(0xD6);
    REQUIRE(store->write(bounded_context(), 0, replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(actual, as_vector(replacement));
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value()).value().state,
               myrd::ConsistentSnapshotState::FullyScanned);
}

TEST_CASE(m2_1_task06_post_cut_successor_block_can_be_written_twice) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    publish_logical_kind(
        *store, LogicalDirtyKind::Data, 0, block_data(0xB7));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(), begun.value(), cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());

    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(
                         *backing)
                         .all_zero_block(1)
                         .build();
    REQUIRE(store->install_backing_view({8, successor}).ok());
    const auto first = block_data(0xC8);
    const auto second = block_data(0xD9);
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, first).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    REQUIRE_EQ(store->consistent_snapshot_status(begun.value()).value().state,
               myrd::ConsistentSnapshotState::FullyScanned);

    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, actual).ok());
    REQUIRE_EQ(actual, as_vector(second));
}

TEST_CASE(m2_1_task06_scan_fails_closed_on_invalid_dirty_identity) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    publish_logical_kind(*store, LogicalDirtyKind::Zero, 0, {});
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
        *store, 0, 0, 7));
    myrd::DirtyScanCursor cursor{};

    const auto rejected = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    const auto status = store->consistent_snapshot_status(begun.value());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().state, myrd::ConsistentSnapshotState::Active);
}

TEST_CASE(m2_1_task06_sparse_scan_does_not_walk_one_tibibyte_of_blocks) {
    constexpr std::uint64_t kLogicalBytes = 1ull << 40;
    auto options = valid_options();
    auto store = require_store(myrd::DynamicBlockStoreCreateArgs{
        kLogicalBytes,
        512,
        options,
        myrd::InitialBlockState::CleanZero,
        {},
        7,
    });
    const auto dirty_block = store->total_blocks() - 1;
    const auto dirty_offset = dirty_block * myrd::kBlockSizeBytes;
    REQUIRE(store->write(
        bounded_context(), dirty_offset, block_data(0xB4)).ok());
    const auto leaves_before = myrd::DynamicBlockStoreTestAccess::
        allocated_leaf_count(*store);
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    myrd::DynamicBlockStoreTestAccess::reset_consistent_snapshot_scan_visits(
        *store);

    myrd::DirtyScanCursor cursor{};
    const auto copied = store->copy_consistent_snapshot_batch(
        bounded_context(),
        begun.value(),
        cursor,
        myrd::kBlockSizeBytes);
    REQUIRE(copied.ok());
    REQUIRE_EQ(copied.value().size(), 1ull);
    REQUIRE_EQ(copied.value().front().block_index, dirty_block);
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::allocated_leaf_count(*store),
               leaves_before);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        consistent_snapshot_entry_visits(*store) <=
        myrd::kBlocksPerLeaf);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        consistent_snapshot_leaf_slot_visits(*store) <=
        leaves_before + 1);
}
