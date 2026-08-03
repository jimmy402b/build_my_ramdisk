#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>

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
#include <span>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;
constexpr std::uint64_t kSector = 512;

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
    std::uint64_t logical_size = 4 * kBlock,
    myrd::BackingGeneration generation = 3) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_size,
        static_cast<std::uint32_t>(kSector),
        myrd::DynamicBlockStoreOptions{
            2 * kBlock,
            4 * kBlock,
            kBlock,
            kBlock,
            8,
            64,
        },
        myrd::InitialBlockState::BackingUnresident,
        std::move(backing),
        generation,
    };
}

std::unique_ptr<myrd::DynamicBlockStore> require_store(
    myrd::DynamicBlockStoreCreateArgs args,
    myrd::DynamicBlockStoreTestDependencies dependencies = {}) {
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    return created.move_value();
}

void seed_state(myrd::DynamicBlockStore& store,
                myrd::BlockState state,
                std::span<const std::byte> resident_data,
                std::uint64_t block_index = 0) {
    switch (state) {
    case myrd::BlockState::Unresident:
        break;
    case myrd::BlockState::ZeroClean:
    case myrd::BlockState::ZeroDirty:
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
            store, block_index, state, 3));
        break;
    case myrd::BlockState::ResidentClean:
    case myrd::BlockState::ResidentDirty:
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            store, block_index, state, resident_data, 3));
        break;
    default:
        REQUIRE(false);
    }
}

myrd::Result<void> apply_range(myrd::DynamicBlockStore& store,
                               bool discard,
                               std::span<const myrd::DataRange> ranges,
                               const myrd::OperationContext& context = {}) {
    return discard
        ? store.unmap(context, ranges)
        : store.zero(context, ranges);
}

std::vector<std::byte> read_bytes(myrd::DynamicBlockStore& store,
                                  std::uint64_t offset,
                                  std::size_t length) {
    std::vector<std::byte> output(length, std::byte{0xEE});
    REQUIRE(store.read(myrd::OperationContext{}, offset, output).ok());
    return output;
}

bool all_zero(std::span<const std::byte> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte byte) {
        return byte == std::byte{0};
    });
}

class SelectiveDecommitVirtualMemoryApi final
    : public myrd::IVirtualMemoryApi {
public:
    void fail_decommit_call(std::uint64_t call) noexcept {
        fail_decommit_call_.store(call, std::memory_order_release);
    }

    std::uint64_t decommit_calls() const noexcept {
        return decommit_calls_.load(std::memory_order_acquire);
    }

    void* allocate(void* address,
                   std::size_t size_bytes,
                   std::uint32_t allocation_type,
                   std::uint32_t protection) noexcept override {
        auto* result = ::VirtualAlloc(
            address, size_bytes, allocation_type, protection);
        last_error_.store(
            result == nullptr ? ::GetLastError() : ERROR_SUCCESS,
            std::memory_order_release);
        return result;
    }

    bool free(void* address,
              std::size_t size_bytes,
              std::uint32_t free_type) noexcept override {
        if (free_type == MEM_DECOMMIT) {
            const auto call = decommit_calls_.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            if (call == fail_decommit_call_.load(
                            std::memory_order_acquire)) {
                last_error_.store(ERROR_NOT_ENOUGH_MEMORY,
                                  std::memory_order_release);
                return false;
            }
        }
        const auto result = ::VirtualFree(address, size_bytes, free_type);
        last_error_.store(
            result ? ERROR_SUCCESS : ::GetLastError(),
            std::memory_order_release);
        return result != FALSE;
    }

    bool query(const void* address,
               myrd::VirtualMemoryRegionInfo& information) noexcept override {
        MEMORY_BASIC_INFORMATION native{};
        if (::VirtualQuery(address, &native, sizeof(native)) == 0) {
            last_error_.store(::GetLastError(), std::memory_order_release);
            return false;
        }
        information.allocation_base = native.AllocationBase;
        information.base_address = native.BaseAddress;
        information.region_size_bytes = native.RegionSize;
        information.state = native.State == MEM_COMMIT
            ? myrd::VirtualMemoryRegionState::Committed
            : myrd::VirtualMemoryRegionState::Reserved;
        last_error_.store(ERROR_SUCCESS, std::memory_order_release);
        return true;
    }

    std::uint32_t last_error() const noexcept override {
        return last_error_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> fail_decommit_call_{};
    std::atomic<std::uint64_t> decommit_calls_{};
    std::atomic<std::uint32_t> last_error_{ERROR_SUCCESS};
};

bool wait_for_entry_state(myrd::DynamicBlockStore& store,
                          std::uint64_t block_index,
                          myrd::BlockState expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        myrd::BlockEntry entry{};
        if (myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                store, block_index, entry) &&
            entry.state == expected) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

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

    bool wait_until_entered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(
            lock,
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            [this] { return entered_; });
    }

    void release() noexcept {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

} // namespace

TEST_CASE(m2_ranges_full_zero_and_unmap_cover_all_stable_states_without_load) {
    const auto resident = block_data(0x31);
    for (const bool discard : {false, true}) {
        for (const bool zero_backing : {false, true}) {
            for (const auto source_state : {
                     myrd::BlockState::Unresident,
                     myrd::BlockState::ResidentClean,
                     myrd::BlockState::ResidentDirty,
                     myrd::BlockState::ZeroClean,
                     myrd::BlockState::ZeroDirty}) {
                auto builder = myrd::test::FakeBackingStoreBuilder(20);
                auto backing = zero_backing
                    ? builder.all_zero_block(0).build()
                    : builder.data_block(0, block_data(0x80)).build();
                auto store = require_store(backing_args(backing));
                seed_state(*store, source_state, resident);
                const myrd::DataRange range{0, kBlock};

                REQUIRE(apply_range(
                    *store, discard,
                    std::span<const myrd::DataRange>(&range, 1)).ok());

                myrd::BlockEntry entry{};
                REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                    *store, 0, entry));
                REQUIRE_EQ(entry.state, myrd::BlockState::ZeroDirty);
                REQUIRE_EQ(entry.prior_state, myrd::BlockState::ZeroDirty);
                REQUIRE_EQ(entry.version, 1ull);
                REQUIRE(entry.dirty_epoch != 0);
                REQUIRE_EQ(entry.dirty_base_generation, 3ull);
                REQUIRE_EQ(entry.dirty_kind,
                    discard ? myrd::DirtyRecordKind::Discard
                            : myrd::DirtyRecordKind::Zero);
                REQUIRE_EQ(backing->block_load_count(0), 0ull);
                REQUIRE_EQ(store->stats().committed_bytes, 0ull);
                REQUIRE(all_zero(read_bytes(*store, 0, kBlock)));
                REQUIRE_EQ(backing->block_load_count(0), 0ull);
                REQUIRE(store->validate_invariants_for_test().ok());
            }
        }
    }
}

TEST_CASE(m2_ranges_partial_operations_preserve_data_or_noop_on_explicit_zero) {
    const auto original = block_data(0x42);
    for (const bool discard : {false, true}) {
        for (const bool zero_backing : {false, true}) {
            for (const auto source_state : {
                     myrd::BlockState::Unresident,
                     myrd::BlockState::ResidentClean,
                     myrd::BlockState::ResidentDirty,
                     myrd::BlockState::ZeroClean,
                     myrd::BlockState::ZeroDirty}) {
                auto builder = myrd::test::FakeBackingStoreBuilder(21);
                auto backing = zero_backing
                    ? builder.all_zero_block(0).build()
                    : builder.data_block(0, original).build();
                auto store = require_store(backing_args(backing));
                seed_state(*store, source_state, original);
                const myrd::DataRange range{2 * kSector, kSector};

                REQUIRE(apply_range(
                    *store, discard,
                    std::span<const myrd::DataRange>(&range, 1)).ok());

                myrd::BlockEntry entry{};
                REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                    *store, 0, entry));
                if (source_state == myrd::BlockState::ZeroClean ||
                    source_state == myrd::BlockState::ZeroDirty) {
                    REQUIRE_EQ(entry.state, source_state);
                    REQUIRE_EQ(entry.version, 0ull);
                    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
                    REQUIRE_EQ(backing->block_load_count(0), 0ull);
                    REQUIRE(all_zero(read_bytes(*store, 0, kBlock)));
                } else {
                    auto expected = source_state ==
                            myrd::BlockState::Unresident && zero_backing
                        ? myrd::test::FakeBackingStore::BlockData{}
                        : original;
                    std::fill_n(expected.begin() + 2 * kSector,
                                static_cast<std::size_t>(kSector),
                                std::byte{0});
                    REQUIRE(read_bytes(*store, 0, kBlock) ==
                            std::vector<std::byte>(
                                expected.begin(), expected.end()));
                    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
                    REQUIRE_EQ(entry.version, 1ull);
                    REQUIRE_EQ(entry.dirty_kind,
                               myrd::DirtyRecordKind::Data);
                    REQUIRE_EQ(backing->block_load_count(0),
                               source_state == myrd::BlockState::Unresident
                                   ? 1ull : 0ull);
                }
                REQUIRE(store->validate_invariants_for_test().ok());
            }
        }
    }
}

TEST_CASE(m2_ranges_partial_unresident_all_zero_still_publishes_data_record) {
    auto backing = myrd::test::FakeBackingStoreBuilder(22)
                       .all_zero_block(0)
                       .build();
    auto store = require_store(backing_args(backing));
    const myrd::DataRange range{kSector, kSector};
    REQUIRE(store->zero(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1)).ok());
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 1ull);
    REQUIRE_EQ(entry.dirty_kind, myrd::DirtyRecordKind::Data);
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
}

TEST_CASE(m2_ranges_repeated_full_operations_publish_latest_kind_and_version) {
    auto backing = myrd::test::FakeBackingStoreBuilder(23)
                       .data_block(0, block_data(0x51))
                       .build();
    auto store = require_store(backing_args(backing));
    auto successor = myrd::test::FakeBackingStoreBuilder(29).build();
    REQUIRE(store->install_backing_view({7, successor}).ok());
    const myrd::DataRange range{0, kBlock};
    const auto ranges = std::span<const myrd::DataRange>(&range, 1);
    REQUIRE(store->zero(myrd::OperationContext{}, ranges).ok());
    REQUIRE(store->unmap(myrd::OperationContext{}, ranges).ok());
    REQUIRE(store->zero(myrd::OperationContext{}, ranges).ok());

    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(entry.version, 3ull);
    REQUIRE_EQ(entry.dirty_kind, myrd::DirtyRecordKind::Zero);
    REQUIRE_EQ(entry.dirty_base_generation, 7ull);
    REQUIRE_EQ(backing->block_load_count(0), 0ull);
    REQUIRE_EQ(store->stats().zero_bytes_total, 2 * kBlock);
    REQUIRE_EQ(store->stats().unmap_bytes_total, kBlock);
}

TEST_CASE(m2_ranges_cross_block_uses_load_only_for_partial_segments) {
    const auto first = block_data(0x53);
    const auto middle = block_data(0x63);
    const auto last = block_data(0x73);
    auto backing = myrd::test::FakeBackingStoreBuilder(30)
                       .data_block(0, first)
                       .data_block(1, middle)
                       .data_block(2, last)
                       .build();
    auto store = require_store(backing_args(backing, 3 * kBlock));
    const myrd::DataRange range{
        kBlock - kSector, kBlock + 2 * kSector};

    REQUIRE(store->unmap(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1)).ok());

    auto expected_first = first;
    std::fill_n(expected_first.end() - kSector,
                static_cast<std::size_t>(kSector), std::byte{0});
    auto expected_last = last;
    std::fill_n(expected_last.begin(),
                static_cast<std::size_t>(kSector), std::byte{0});
    REQUIRE(read_bytes(*store, 0, kBlock) ==
            std::vector<std::byte>(
                expected_first.begin(), expected_first.end()));
    REQUIRE(all_zero(read_bytes(*store, kBlock, kBlock)));
    REQUIRE(read_bytes(*store, 2 * kBlock, kBlock) ==
            std::vector<std::byte>(
                expected_last.begin(), expected_last.end()));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    REQUIRE_EQ(backing->block_load_count(1), 0ull);
    REQUIRE_EQ(backing->block_load_count(2), 1ull);
    for (std::uint64_t block = 0; block < 3; ++block) {
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, block, entry));
        REQUIRE_EQ(entry.version, 1ull);
        REQUIRE_EQ(entry.state,
            block == 1 ? myrd::BlockState::ZeroDirty
                       : myrd::BlockState::ResidentDirty);
    }
}

TEST_CASE(m2_ranges_validate_entire_batch_before_any_mutation) {
    auto backing = myrd::test::FakeBackingStoreBuilder(24)
                       .data_block(0, block_data(0x61))
                       .build();
    auto store = require_store(backing_args(backing));
    const std::array<myrd::DataRange, 2> ranges{{
        {0, kBlock},
        {kBlock + 1, kSector},
    }};
    const auto before = store->stats();
    const auto result = store->unmap(myrd::OperationContext{}, ranges);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE(store->stats() == before);

    const std::span<const myrd::DataRange> empty{};
    REQUIRE(store->zero(myrd::OperationContext{}, empty).ok());
    const std::array invalid{
        myrd::DataRange{0, 0},
        myrd::DataRange{0, kSector - 1},
        myrd::DataRange{store->size_bytes(), kSector},
        myrd::DataRange{
            (std::numeric_limits<std::uint64_t>::max)() - kSector + 1,
            kSector},
    };
    for (const auto& range : invalid) {
        const auto rejected = store->zero(
            myrd::OperationContext{},
            std::span<const myrd::DataRange>(&range, 1));
        REQUIRE(!rejected.ok());
        REQUIRE_EQ(rejected.error().code,
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::invalid_range));
    }
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
}

TEST_CASE(m2_ranges_full_final_short_block_is_zero_without_backing_load) {
    constexpr std::uint64_t logical_size = 2 * kBlock + 4096;
    auto backing = myrd::test::FakeBackingStoreBuilder(25)
                       .data_block(2, block_data(0x71))
                       .build();
    auto store = require_store(backing_args(backing, logical_size));
    const myrd::DataRange range{2 * kBlock, 4096};
    REQUIRE(store->unmap(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1)).ok());
    REQUIRE(all_zero(read_bytes(*store, 2 * kBlock, 4096)));
    REQUIRE_EQ(backing->block_load_count(2), 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 2, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(entry.dirty_kind, myrd::DirtyRecordKind::Discard);
}

TEST_CASE(m2_ranges_decommit_failure_restores_exact_resident_state_and_bytes) {
    const auto original = block_data(0x81);
    auto backing = myrd::test::FakeBackingStoreBuilder(26).build();
    auto memory_api = std::make_shared<SelectiveDecommitVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.virtual_memory_api = memory_api;
    auto store = require_store(backing_args(backing), dependencies);
    seed_state(*store, myrd::BlockState::ResidentDirty, original);
    myrd::BlockEntry before_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before_entry));
    const auto before_stats = store->stats();
    memory_api->fail_decommit_call(1);
    const myrd::DataRange range{0, kBlock};

    const auto result = store->zero(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1));

    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::decommit_failed));
    myrd::BlockEntry after_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after_entry));
    REQUIRE_EQ(after_entry.state, before_entry.state);
    REQUIRE_EQ(after_entry.version, before_entry.version);
    REQUIRE_EQ(after_entry.dirty_epoch, before_entry.dirty_epoch);
    REQUIRE_EQ(after_entry.dirty_kind, before_entry.dirty_kind);
    REQUIRE_EQ(after_entry.dirty_base_generation,
               before_entry.dirty_base_generation);
    REQUIRE_EQ(store->stats().committed_bytes,
               before_stats.committed_bytes);
    REQUIRE_EQ(store->stats().resident_dirty_blocks,
               before_stats.resident_dirty_blocks);
    REQUIRE_EQ(store->stats().decommit_failures,
               before_stats.decommit_failures + 1);
    REQUIRE(read_bytes(*store, 0, kBlock) ==
            std::vector<std::byte>(original.begin(), original.end()));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_ranges_late_decommit_failure_keeps_completed_prefix_only) {
    const auto first_data = block_data(0x91);
    const auto second_data = block_data(0xA1);
    auto backing = myrd::test::FakeBackingStoreBuilder(27).build();
    auto memory_api = std::make_shared<SelectiveDecommitVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.virtual_memory_api = memory_api;
    auto store = require_store(backing_args(backing, 2 * kBlock), dependencies);
    seed_state(*store, myrd::BlockState::ResidentClean, first_data, 0);
    seed_state(*store, myrd::BlockState::ResidentDirty, second_data, 1);
    memory_api->fail_decommit_call(2);
    const myrd::DataRange range{0, 2 * kBlock};

    const auto result = store->unmap(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1));

    REQUIRE(!result.ok());
    myrd::BlockEntry first{}, second{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second));
    REQUIRE_EQ(first.state, myrd::BlockState::ZeroDirty);
    REQUIRE_EQ(first.version, 1ull);
    REQUIRE_EQ(first.dirty_kind, myrd::DirtyRecordKind::Discard);
    REQUIRE_EQ(second.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(second.version, 0ull);
    REQUIRE(read_bytes(*store, kBlock, kBlock) ==
            std::vector<std::byte>(second_data.begin(), second_data.end()));
    REQUIRE_EQ(store->stats().committed_bytes, kBlock);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_ranges_cancelled_request_never_reports_false_success) {
    auto backing = myrd::test::FakeBackingStoreBuilder(28).build();
    auto store = require_store(backing_args(backing));
    const myrd::DataRange range{0, kBlock};
    std::stop_source stop;
    stop.request_stop();
    const auto result = store->zero(
        myrd::OperationContext{stop.get_token()},
        std::span<const myrd::DataRange>(&range, 1));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
}

TEST_CASE(m2_ranges_close_while_waiting_for_pin_rolls_back_full_operation) {
    const auto original = block_data(0xB1);
    auto backing = myrd::test::FakeBackingStoreBuilder(31).build();
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(backing_args(backing), dependencies);
    seed_state(*store, myrd::BlockState::ResidentDirty, original);
    const auto before = store->stats();
    std::vector<std::byte> reader_output(kBlock);
    std::uint32_t reader_code{};
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, reader_output);
        reader_code = result.ok() ? 0 : result.error().code;
    });
    const bool reader_pinned = observer->wait_until_entered();
    const myrd::DataRange range{0, kBlock};
    std::uint32_t zero_code{};
    std::thread zeroer([&] {
        const auto result = store->zero(
            myrd::OperationContext{},
            std::span<const myrd::DataRange>(&range, 1));
        zero_code = result.ok() ? 0 : result.error().code;
    });
    const bool zero_waiting = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    std::thread closer([&] { store->close(); });
    zeroer.join();

    myrd::BlockEntry restored{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, restored));
    REQUIRE(reader_pinned);
    REQUIRE(zero_waiting);
    REQUIRE_EQ(zero_code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(restored.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(restored.version, 0ull);
    REQUIRE_EQ(store->stats().committed_bytes, before.committed_bytes);
    REQUIRE_EQ(store->stats().resident_dirty_blocks,
               before.resident_dirty_blocks);

    observer->release();
    reader.join();
    closer.join();
    REQUIRE_EQ(reader_code, 0u);
    REQUIRE(reader_output ==
            std::vector<std::byte>(original.begin(), original.end()));
    REQUIRE(store->is_closed());
}
