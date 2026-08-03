#include "test_framework.h"

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
