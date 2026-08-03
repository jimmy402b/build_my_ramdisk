#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/zero_backing_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;

myrd::DynamicBlockStoreCreateArgs valid_args(
    std::uint64_t logical_size = 3 * kBlock + 4096) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_size,
        512,
        myrd::DynamicBlockStoreOptions{
            2 * kBlock,
            4 * kBlock,
            kBlock,
            kBlock,
            8,
            64,
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

std::vector<std::byte> pattern(std::uint8_t seed) {
    std::vector<std::byte> bytes(kBlock);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = std::byte{
            static_cast<std::uint8_t>(seed + (index % 251))};
    }
    return bytes;
}

bool all_equal(std::span<const std::byte> bytes, std::byte value) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [value](std::byte byte) { return byte == value; });
}

myrd::test::FakeBackingStore::BlockData block_data(std::byte value) {
    myrd::test::FakeBackingStore::BlockData data{};
    data.fill(value);
    return data;
}

std::uint32_t result_code(const myrd::Result<void>& result) {
    return result.has_error()
               ? result.error().code
               : (std::numeric_limits<std::uint32_t>::max)();
}

bool wait_for_stripe_waiters(const myrd::DynamicBlockStore& store,
                             std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (store.stripe_waiter_count_for_test() != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return store.stripe_waiter_count_for_test() == expected;
}

class CountingBackingStore final : public myrd::IBackingStore {
public:
    myrd::Result<myrd::BlockLoadResult> load_block(
        const myrd::OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        loads_.fetch_add(1, std::memory_order_relaxed);
        return myrd::Result<myrd::BlockLoadResult>::success(
            {myrd::BlockLoadKind::AllZero});
    }

    std::uint64_t loads() const noexcept {
        return loads_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<std::uint64_t> loads_{0};
};

class CancelAfterFirstSegment final
    : public myrd::IDynamicBlockStoreReadObserver {
public:
    explicit CancelAfterFirstSegment(std::stop_source& source) noexcept
        : source_(source) {}

    void after_resident_pin(std::uint64_t) noexcept override {}

    void after_segment(std::uint64_t block_index) noexcept override {
        if (block_index == 0) {
            source_.request_stop();
        }
    }

private:
    std::stop_source& source_;
};

class ExpireAfterFirstSegment final
    : public myrd::IDynamicBlockStoreReadObserver {
public:
    void set_deadline(
        std::chrono::steady_clock::time_point deadline) noexcept {
        deadline_ = deadline;
    }

    void after_resident_pin(std::uint64_t) noexcept override {}

    void after_segment(std::uint64_t block_index) noexcept override {
        if (block_index == 0) {
            while (std::chrono::steady_clock::now() < deadline_) {
                std::this_thread::yield();
            }
        }
    }

private:
    std::chrono::steady_clock::time_point deadline_{
        std::chrono::steady_clock::time_point::max()};
};

class BlockingPinObserver final
    : public myrd::IDynamicBlockStoreReadObserver {
public:
    void after_resident_pin(std::uint64_t block_index) noexcept override {
        if (block_index != 0) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void after_segment(std::uint64_t) noexcept override {}

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return entered_; });
    }

    void release() {
        std::scoped_lock lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

class ReentrantCloseReadObserver final
    : public myrd::IDynamicBlockStoreReadObserver {
public:
    void bind(myrd::DynamicBlockStore* store) noexcept { store_ = store; }

    void after_resident_pin(std::uint64_t) noexcept override {}

    void after_segment(std::uint64_t) noexcept override {
        if (store_ != nullptr && !reentered_.exchange(true)) {
            store_->close();
        }
    }

    bool reentered() const noexcept {
        return reentered_.load(std::memory_order_acquire);
    }

private:
    myrd::DynamicBlockStore* store_{};
    std::atomic<bool> reentered_{false};
};

} // namespace

TEST_CASE(m2_read_implicit_clean_zero_is_sparse_for_full_partial_and_cross_block) {
    auto backing = std::make_shared<CountingBackingStore>();
    auto args = valid_args();
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    const myrd::OperationContext context;

    std::vector<std::byte> full(kBlock, std::byte{0x7f});
    REQUIRE(store->read(context, 0, full).ok());
    REQUIRE(all_equal(full, std::byte{0}));

    std::vector<std::byte> guarded(2048, std::byte{0x7f});
    auto partial = std::span(guarded).subspan(512, 1024);
    REQUIRE(store->read(context, 512, partial).ok());
    REQUIRE(all_equal(partial, std::byte{0}));
    REQUIRE(all_equal(std::span(guarded).first(512), std::byte{0x7f}));
    REQUIRE(all_equal(std::span(guarded).last(512), std::byte{0x7f}));

    std::vector<std::byte> cross(1024, std::byte{0x7f});
    REQUIRE(store->read(context, kBlock - 512, cross).ok());
    REQUIRE(all_equal(cross, std::byte{0}));

    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(backing->loads(), 0ull);
}

TEST_CASE(m2_read_explicit_zero_clean_and_zero_dirty_never_commit_or_load) {
    auto backing = std::make_shared<CountingBackingStore>();
    auto args = valid_args();
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
        *store, 1, myrd::BlockState::ZeroClean, 0));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
        *store, 2, myrd::BlockState::ZeroDirty, 0));

    std::vector<std::byte> output(kBlock + 1024, std::byte{0x55});
    const myrd::OperationContext context;
    REQUIRE(store->read(context, kBlock + 512, output).ok());
    REQUIRE(all_equal(output, std::byte{0}));
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(backing->loads(), 0ull);
}

TEST_CASE(m2_read_resident_clean_dirty_and_three_block_splice_are_exact) {
    auto backing = std::make_shared<CountingBackingStore>();
    auto args = valid_args();
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    const auto first = pattern(3);
    const auto second = pattern(71);
    const auto third = pattern(149);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, first, 0));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 1, myrd::BlockState::ResidentDirty, second, 0));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 2, myrd::BlockState::ResidentClean, third, 0));

    std::vector<std::byte> full(kBlock);
    const myrd::OperationContext context;
    REQUIRE(store->read(context, 0, full).ok());
    REQUIRE(full == first);

    std::vector<std::byte> partial(2048);
    REQUIRE(store->read(context, kBlock + 1024, partial).ok());
    REQUIRE(std::equal(partial.begin(), partial.end(), second.begin() + 1024));

    std::vector<std::byte> splice(2 * kBlock, std::byte{0});
    REQUIRE(store->read(context, kBlock - 512, splice).ok());
    REQUIRE(std::equal(splice.begin(), splice.begin() + 512,
                       first.end() - 512));
    REQUIRE(std::equal(splice.begin() + 512,
                       splice.begin() + 512 + kBlock,
                       second.begin()));
    REQUIRE(std::equal(splice.begin() + 512 + kBlock,
                       splice.end(),
                       third.begin()));
    REQUIRE_EQ(backing->loads(), 0ull);
}

TEST_CASE(m2_read_last_partial_logical_block_never_crosses_public_capacity) {
    auto store = require_store(valid_args(2 * kBlock + 4096));
    const auto tail = pattern(211);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 2, myrd::BlockState::ResidentClean, tail, 0));

    std::vector<std::byte> output(4096, std::byte{0});
    const myrd::OperationContext context;
    REQUIRE(store->read(context, 2 * kBlock, output).ok());
    REQUIRE(std::equal(output.begin(), output.end(), tail.begin()));
}

TEST_CASE(m2_read_rejects_resident_metadata_without_committed_memory) {
    auto store = require_store(valid_args());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_uncommitted_resident(
        *store, 0, myrd::BlockState::ResidentClean, 0));

    std::vector<std::byte> output(512, std::byte{0x5a});
    const myrd::OperationContext context;
    const auto result = store->read(context, 0, output);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::internal_invariant));
    REQUIRE(all_equal(output, std::byte{0x5a}));

    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.pin_count, 0ull);
}

TEST_CASE(m2_read_rejects_invalid_ranges_before_touching_destination) {
    auto store = require_store(valid_args());
    const myrd::OperationContext context;
    const auto untouched = std::byte{0x6d};

    std::vector<std::byte> empty;
    REQUIRE(!store->read(context, 0, empty).ok());

    std::vector<std::byte> buffer(512, untouched);
    REQUIRE(!store->read(context, 1, buffer).ok());
    REQUIRE(all_equal(buffer, untouched));

    std::vector<std::byte> misaligned(513, untouched);
    REQUIRE(!store->read(context, 0, misaligned).ok());
    REQUIRE(all_equal(misaligned, untouched));

    const auto overflow_offset =
        (std::numeric_limits<std::uint64_t>::max)() - 511;
    REQUIRE(!store->read(context, overflow_offset, buffer).ok());
    REQUIRE(all_equal(buffer, untouched));

    REQUIRE(!store->read(context, store->size_bytes(), buffer).ok());
    REQUIRE(all_equal(buffer, untouched));

    const auto mismatch = myrd::validate_and_split_block_range(
        store->size_bytes(), store->sector_size(), 0, 512, 511);
    REQUIRE(!mismatch.ok());
    REQUIRE_EQ(mismatch.error, myrd::BlockMathError::BufferLengthMismatch);
}

TEST_CASE(m2_read_observes_cancel_and_deadline_at_start_without_copy) {
    auto store = require_store(valid_args());
    std::vector<std::byte> output(512, std::byte{0x33});

    std::stop_source source;
    source.request_stop();
    myrd::OperationContext cancelled{source.get_token()};
    const auto cancelled_result = store->read(cancelled, 0, output);
    REQUIRE(!cancelled_result.ok());
    REQUIRE_EQ(cancelled_result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE(all_equal(output, std::byte{0x33}));

    const myrd::OperationContext expired{
        {}, std::chrono::steady_clock::now()};
    const auto expired_result = store->read(expired, 0, output);
    REQUIRE(!expired_result.ok());
    REQUIRE_EQ(expired_result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE(all_equal(output, std::byte{0x33}));
}

TEST_CASE(m2_read_observes_cancellation_between_blocks_and_keeps_prefix) {
    std::stop_source source;
    auto observer = std::make_shared<CancelAfterFirstSegment>(source);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.read_observer = observer;
    auto store = require_store(valid_args(), dependencies);

    std::vector<std::byte> output(2 * kBlock, std::byte{0x42});
    const myrd::OperationContext context{source.get_token()};
    const auto result = store->read(context, 0, output);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE(all_equal(std::span(output).first(kBlock), std::byte{0}));
    REQUIRE(all_equal(std::span(output).subspan(kBlock), std::byte{0x42}));
}

TEST_CASE(m2_read_observes_deadline_between_blocks_and_keeps_prefix) {
    auto observer = std::make_shared<ExpireAfterFirstSegment>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.read_observer = observer;
    auto store = require_store(valid_args(), dependencies);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    observer->set_deadline(deadline);

    std::vector<std::byte> output(2 * kBlock, std::byte{0x24});
    const myrd::OperationContext context{{}, deadline};
    const auto result = store->read(context, 0, output);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE(all_equal(std::span(output).first(kBlock), std::byte{0}));
    REQUIRE(all_equal(std::span(output).subspan(kBlock), std::byte{0x24}));
}

TEST_CASE(m2_read_resident_pin_blocks_decommit_and_is_released_after_copy) {
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.read_observer = observer;
    auto store = require_store(valid_args(), dependencies);
    const auto data = pattern(17);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, data, 0));

    std::vector<std::byte> output(kBlock, std::byte{0});
    std::atomic<bool> read_ok{false};
    std::thread reader([&] {
        const myrd::OperationContext context;
        read_ok.store(store->read(context, 0, output).ok(),
                      std::memory_order_release);
    });

    observer->wait_until_entered();
    myrd::BlockEntry during{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, during));
    REQUIRE_EQ(during.pin_count, 1ull);
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::try_decommit_resident_clean(
        *store, 0));

    observer->release();
    reader.join();
    REQUIRE(read_ok.load(std::memory_order_acquire));
    REQUIRE(output == data);

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.pin_count, 0ull);
    REQUIRE(after.reference_bit);
    REQUIRE(after.last_access_epoch != 0);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::try_decommit_resident_clean(
        *store, 0));
}

TEST_CASE(m2_read_observer_may_reenter_close_without_deadlock) {
    auto observer = std::make_shared<ReentrantCloseReadObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.read_observer = observer;
    auto store = require_store(valid_args(), dependencies);
    observer->bind(store.get());

    std::mutex watchdog_mutex;
    std::condition_variable watchdog_condition;
    bool completed = false;
    std::jthread watchdog([&](std::stop_token) {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_condition.wait_for(
                lock,
                std::chrono::seconds(2),
                [&] { return completed; })) {
            std::abort();
        }
    });

    std::vector<std::byte> output(512, std::byte{0x7f});
    const myrd::OperationContext context;
    const auto result = store->read(context, 0, output);
    {
        std::scoped_lock lock(watchdog_mutex);
        completed = true;
    }
    watchdog_condition.notify_all();
    watchdog.join();

    REQUIRE(result.ok());
    REQUIRE(observer->reentered());
    REQUIRE(store->is_closed());
    REQUIRE(all_equal(output, std::byte{0}));
}

TEST_CASE(m2_read_unresident_data_loads_once_and_publishes_clean_generation) {
    const auto data = block_data(std::byte{0xC7});
    auto backing = myrd::test::FakeBackingStoreBuilder(10)
                       .data_block(0, data)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.initial_backing_generation = 7;
    args.backing_store = backing;
    auto store = require_store(std::move(args));

    std::vector<std::byte> output(kBlock, std::byte{0});
    REQUIRE(store->read(myrd::OperationContext{}, 0, output).ok());
    REQUIRE(std::equal(output.begin(), output.end(), data.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    REQUIRE_EQ(backing->last_load_destination_address(0) % kBlock, 0ull);

    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry.clean_generation, 7ull);
    REQUIRE_EQ(store->stats().backing_reads_total, 1ull);
    REQUIRE_EQ(store->stats().backing_read_bytes_total, kBlock);
}

TEST_CASE(m2_read_unresident_all_zero_publishes_zero_clean_without_commit) {
    auto backing = myrd::test::FakeBackingStoreBuilder(11)
                       .all_zero_block(1)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.initial_backing_generation = 9;
    args.backing_store = backing;
    auto store = require_store(std::move(args));

    std::vector<std::byte> output(kBlock, std::byte{0xA5});
    REQUIRE(store->read(myrd::OperationContext{}, kBlock, output).ok());
    REQUIRE(all_equal(output, std::byte{0}));
    REQUIRE_EQ(backing->block_load_count(1), 1ull);

    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ZeroClean);
    REQUIRE_EQ(entry.clean_generation, 9ull);
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
}

TEST_CASE(m2_read_same_block_misses_are_single_flight_for_2_8_and_32_readers) {
    for (const auto reader_count : {2u, 8u, 32u}) {
        auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
        const auto expected = block_data(
            std::byte{static_cast<std::uint8_t>(0x40u + reader_count)});
        auto backing = myrd::test::FakeBackingStoreBuilder(12)
                           .data_block(0, expected)
                           .gate_block(0, gate)
                           .build();
        auto args = valid_args();
        args.initial_state = myrd::InitialBlockState::BackingUnresident;
        args.backing_store = backing;
        auto store = require_store(std::move(args));

        std::vector<std::vector<std::byte>> outputs(
            reader_count, std::vector<std::byte>(kBlock, std::byte{0}));
        std::vector<std::uint32_t> codes(reader_count, 0);
        std::atomic<std::uint32_t> ready{0};
        std::vector<std::thread> readers;
        readers.reserve(reader_count);
        for (std::uint32_t index = 0; index < reader_count; ++index) {
            readers.emplace_back([&, index] {
                ready.fetch_add(1, std::memory_order_release);
                const auto result = store->read(
                    myrd::OperationContext{}, 0, outputs[index]);
                codes[index] = result.ok() ? 0 : result_code(result);
            });
        }
        while (ready.load(std::memory_order_acquire) != reader_count) {
            std::this_thread::yield();
        }
        REQUIRE(gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
        const bool all_waiters_registered =
            wait_for_stripe_waiters(*store, reader_count - 1);
        gate->open();
        for (auto& reader : readers) {
            reader.join();
        }
        REQUIRE(all_waiters_registered);
        for (std::size_t index = 0; index < reader_count; ++index) {
            REQUIRE_EQ(codes[index], 0u);
            REQUIRE(std::equal(outputs[index].begin(),
                               outputs[index].end(),
                               expected.begin()));
        }
        REQUIRE_EQ(backing->block_load_count(0), 1ull);
    }
}

TEST_CASE(m2_read_different_block_misses_execute_backing_io_in_parallel) {
    auto gate0 = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto gate1 = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto expected0 = block_data(std::byte{0x51});
    const auto expected1 = block_data(std::byte{0xA8});
    auto backing = myrd::test::FakeBackingStoreBuilder(13)
                       .data_block(0, expected0)
                       .data_block(1, expected1)
                       .gate_block(0, gate0)
                       .gate_block(1, gate1)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    std::vector<std::byte> first(kBlock), second(kBlock);
    std::atomic<std::uint32_t> ready{0};
    std::array<std::uint32_t, 2> codes{};
    std::thread reader0([&] {
        ready.fetch_add(1, std::memory_order_release);
        const auto result = store->read(myrd::OperationContext{}, 0, first);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    std::thread reader1([&] {
        ready.fetch_add(1, std::memory_order_release);
        const auto result = store->read(
            myrd::OperationContext{}, kBlock, second);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    REQUIRE(gate0->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE(gate1->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    gate0->open();
    gate1->open();
    reader0.join();
    reader1.join();
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE(std::equal(first.begin(), first.end(), expected0.begin()));
    REQUIRE(std::equal(second.begin(), second.end(), expected1.begin()));
    REQUIRE_EQ(backing->total_load_count(), 2ull);
}

TEST_CASE(m2_read_waiter_cancellation_does_not_cancel_load_owner) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(14)
                       .gate_block(0, gate)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));

    std::vector<std::byte> owner_output(kBlock), waiter_output(kBlock);
    std::atomic<std::uint32_t> owner_code{0};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, owner_output);
        owner_code.store(result.ok() ? 0 : result_code(result),
                         std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));

    std::stop_source waiter_stop;
    std::atomic<std::uint32_t> waiter_code{0};
    std::thread waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{waiter_stop.get_token()},
            0,
            waiter_output);
        waiter_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool waiter_registered = wait_for_stripe_waiters(*store, 1);
    waiter_stop.request_stop();
    waiter.join();
    REQUIRE(waiter_registered);
    REQUIRE_EQ(waiter_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    gate->open();
    owner.join();
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire), 0u);
}

TEST_CASE(m2_read_waiter_deadline_does_not_cancel_load_owner) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(15)
                       .gate_block(0, gate)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));

    std::vector<std::byte> owner_output(kBlock), waiter_output(kBlock);
    std::atomic<std::uint32_t> owner_code{0};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, owner_output);
        owner_code.store(result.ok() ? 0 : result_code(result),
                         std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    std::atomic<std::uint32_t> waiter_code{0};
    std::thread waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{{}, deadline}, 0, waiter_output);
        waiter_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool waiter_registered = wait_for_stripe_waiters(*store, 1);
    waiter.join();
    REQUIRE(waiter_registered);
    REQUIRE_EQ(waiter_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    gate->open();
    owner.join();
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire), 0u);
}

TEST_CASE(m2_read_owner_cancellation_rolls_back_and_allows_retry) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto expected = block_data(std::byte{0x6A});
    auto backing = myrd::test::FakeBackingStoreBuilder(151)
                       .data_block(0, expected)
                       .gate_block(0, gate)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    std::stop_source owner_stop;
    std::vector<std::byte> cancelled_output(kBlock, std::byte{0xA5});
    std::vector<std::byte> waiter_output(kBlock, std::byte{0x5A});
    std::atomic<std::uint32_t> owner_code{0};
    std::atomic<std::uint32_t> waiter_code{0};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{owner_stop.get_token()},
            0,
            cancelled_output);
        owner_code.store(result.ok() ? 0 : result_code(result),
                         std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    std::thread waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, waiter_output);
        waiter_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool waiter_registered = wait_for_stripe_waiters(*store, 1);
    if (!waiter_registered) {
        gate->open();
    }
    owner_stop.request_stop();
    owner.join();
    waiter.join();
    REQUIRE(waiter_registered);
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(waiter_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE(all_equal(cancelled_output, std::byte{0xA5}));
    REQUIRE(all_equal(waiter_output, std::byte{0x5A}));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(store->stats().loading_blocks, 0ull);

    gate->open();
    std::vector<std::byte> retry_output(kBlock, std::byte{0});
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, retry_output).ok());
    REQUIRE(std::equal(
        retry_output.begin(), retry_output.end(), expected.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 2ull);
}

TEST_CASE(m2_read_owner_failure_rolls_back_and_wakes_waiters_once) {
    constexpr std::size_t reader_count = 8;
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(16)
                       .gate_block(0, gate)
                       .fail_block(0)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    std::array<std::vector<std::byte>, reader_count> outputs;
    std::array<std::uint32_t, reader_count> codes{};
    for (auto& output : outputs) {
        output.assign(kBlock, std::byte{0xA6});
    }
    std::atomic<std::uint32_t> ready{0};
    std::array<std::thread, reader_count> readers;
    for (std::size_t index = 0; index < readers.size(); ++index) {
        readers[index] = std::thread([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            const auto result = store->read(
                myrd::OperationContext{}, 0, outputs[index]);
            codes[index] = result.ok() ? 0 : result_code(result);
        });
    }
    while (ready.load(std::memory_order_acquire) != readers.size()) {
        std::this_thread::yield();
    }
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    const bool all_waiters_registered =
        wait_for_stripe_waiters(*store, reader_count - 1);
    gate->open();
    for (auto& reader : readers) {
        reader.join();
    }
    REQUIRE(all_waiters_registered);
    for (std::size_t index = 0; index < reader_count; ++index) {
        const auto code = codes[index];
        REQUIRE_EQ(code, static_cast<std::uint32_t>(
                             myrd::OperationErrorCode::backing_read_failed));
        REQUIRE(all_equal(outputs[index], std::byte{0xA6}));
    }
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(store->stats().loading_blocks, 0ull);
}

TEST_CASE(m2_read_close_cancels_owner_load_and_restores_unresident) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(17)
                       .gate_block(0, gate)
                       .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    auto store = require_store(std::move(args));
    std::vector<std::byte> output(kBlock);
    std::atomic<std::uint32_t> code{0};
    std::thread reader([&] {
        const auto result = store->read(myrd::OperationContext{}, 0, output);
        code.store(result.ok() ? 0 : result_code(result),
                   std::memory_order_release);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    store->close();
    reader.join();
    REQUIRE_EQ(code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(store->stats().loading_blocks, 0ull);
}

TEST_CASE(m2_read_old_generation_result_is_discarded_and_retried) {
    auto old_gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto lifetime =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    const auto old_data = block_data(std::byte{0x31});
    const auto new_data = block_data(std::byte{0xE2});
    auto old_backing = myrd::test::FakeBackingStoreBuilder(18)
                           .data_block(0, old_data)
                           .gate_block(0, old_gate)
                           .lifetime_tracker(lifetime)
                           .build();
    std::weak_ptr<const myrd::test::FakeBackingStore> old_weak = old_backing;
    auto successor = myrd::test::FakeBackingStoreBuilder(19)
                         .data_block(0, new_data)
                         .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.initial_backing_generation = 1;
    args.backing_store = old_backing;
    auto store = require_store(std::move(args));
    old_backing.reset();

    std::vector<std::byte> output(kBlock);
    std::atomic<std::uint32_t> code{0};
    std::thread reader([&] {
        const auto result = store->read(myrd::OperationContext{}, 0, output);
        code.store(result.ok() ? 0 : result_code(result),
                   std::memory_order_release);
    });
    REQUIRE(old_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE(store->install_backing_view({2, successor}).ok());
    REQUIRE(!old_weak.expired());
    old_gate->open();
    reader.join();
    REQUIRE_EQ(code.load(std::memory_order_acquire), 0u);
    REQUIRE(std::equal(output.begin(), output.end(), new_data.begin()));
    REQUIRE(old_weak.expired());
    REQUIRE_EQ(successor->block_load_count(0), 1ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.stale_backing_load_retries, 1ull);
    REQUIRE_EQ(stats.backing_reads_total, 2ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.clean_generation, 2ull);
}

TEST_CASE(m2_read_stale_all_zero_result_does_not_modify_failed_destination) {
    auto old_gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto old_backing = myrd::test::FakeBackingStoreBuilder(20)
                           .all_zero_block(0)
                           .gate_block(0, old_gate)
                           .build();
    auto failing_successor = myrd::test::FakeBackingStoreBuilder(21)
                                 .fail_block(0)
                                 .build();
    auto args = valid_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.initial_backing_generation = 4;
    args.backing_store = old_backing;
    auto store = require_store(std::move(args));
    std::vector<std::byte> output(kBlock, std::byte{0xD4});
    std::atomic<std::uint32_t> code{0};
    std::thread reader([&] {
        const auto result = store->read(myrd::OperationContext{}, 0, output);
        code.store(result.ok() ? 0 : result_code(result),
                   std::memory_order_release);
    });
    REQUIRE(old_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE(store->install_backing_view({5, failing_successor}).ok());
    old_gate->open();
    reader.join();

    REQUIRE_EQ(code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::backing_read_failed));
    REQUIRE(all_equal(output, std::byte{0xD4}));
    REQUIRE_EQ(old_backing->block_load_count(0), 1ull);
    REQUIRE_EQ(failing_successor->block_load_count(0), 1ull);
    REQUIRE_EQ(store->stats().stale_backing_load_retries, 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
}
