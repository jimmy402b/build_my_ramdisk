#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "fake_virtual_memory_api.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"

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
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;
constexpr std::size_t kSector = 512;

myrd::test::FakeBackingStore::BlockData block_data(std::uint8_t seed) {
    myrd::test::FakeBackingStore::BlockData data{};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = std::byte{
            static_cast<std::uint8_t>(seed + (index % 251))};
    }
    return data;
}

myrd::DynamicBlockStoreCreateArgs clean_zero_args(
    std::uint64_t logical_size = 5 * kBlock,
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
            myrd::kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250},
        },
        myrd::InitialBlockState::CleanZero,
        {},
        generation,
    };
}

myrd::DynamicBlockStoreCreateArgs backing_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t logical_size = 5 * kBlock,
    myrd::BackingGeneration generation = 3) {
    auto args = clean_zero_args(logical_size, generation);
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = std::move(backing);
    return args;
}

std::unique_ptr<myrd::DynamicBlockStore> require_store(
    myrd::DynamicBlockStoreCreateArgs args,
    myrd::DynamicBlockStoreTestDependencies dependencies = {}) {
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    return created.move_value();
}

std::vector<std::byte> read_bytes(
    myrd::DynamicBlockStore& store,
    std::uint64_t offset,
    std::size_t length) {
    std::vector<std::byte> output(length, std::byte{0xED});
    REQUIRE(store.read(myrd::OperationContext{}, offset, output).ok());
    return output;
}

std::uint32_t result_code(const myrd::Result<void>& result) {
    return result.has_error()
        ? result.error().code
        : (std::numeric_limits<std::uint32_t>::max)();
}

bool all_equal(std::span<const std::byte> bytes, std::byte value) {
    return std::all_of(bytes.begin(), bytes.end(), [value](std::byte byte) {
        return byte == value;
    });
}

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

bool wait_for_waiter_count(const myrd::DynamicBlockStore& store,
                           std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (store.stripe_waiter_count_for_test() != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return store.stripe_waiter_count_for_test() == expected;
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

TEST_CASE(m2_write_full_block_covers_every_stable_source_without_backing_load) {
    const auto replacement = block_data(0x81);
    const auto old_data = block_data(0x22);
    for (const auto source_state : {
             myrd::BlockState::ZeroClean,
             myrd::BlockState::ZeroDirty,
             myrd::BlockState::Unresident,
             myrd::BlockState::ResidentClean,
             myrd::BlockState::ResidentDirty}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(40)
                           .data_block(0, old_data)
                           .build();
        auto store = require_store(source_state == myrd::BlockState::Unresident
            ? backing_args(backing)
            : clean_zero_args());
        if (source_state == myrd::BlockState::ZeroDirty) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
                *store, 0, source_state, 3));
        } else if (source_state == myrd::BlockState::ResidentClean ||
                   source_state == myrd::BlockState::ResidentDirty) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
                *store, 0, source_state, old_data, 3));
        }

        REQUIRE(store->write(
            myrd::OperationContext{}, 0, replacement).ok());
        const auto output = read_bytes(*store, 0, kBlock);
        REQUIRE(std::equal(
            output.begin(), output.end(), replacement.begin()));
        REQUIRE_EQ(backing->block_load_count(0), 0ull);
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(entry.prior_state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(entry.version, 1ull);
        REQUIRE_EQ(entry.dirty_base_generation, 3ull);
        REQUIRE_EQ(entry.dirty_kind, myrd::DirtyRecordKind::Data);
        REQUIRE_EQ(store->stats().committed_bytes, kBlock);
    }
}

TEST_CASE(m2_write_implicit_zero_full_block_materializes_one_dirty_leaf) {
    auto store = require_store(clean_zero_args());
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    const auto input = block_data(0x17);
    REQUIRE(store->write(myrd::OperationContext{}, kBlock, input).ok());
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 1ull);
    const auto output = read_bytes(*store, kBlock, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), input.begin()));
    const auto stats = store->stats();
    REQUIRE_EQ(stats.resident_dirty_blocks, 1ull);
    REQUIRE_EQ(stats.zero_clean_blocks, store->total_blocks() - 1);
}

TEST_CASE(m2_write_partial_unresident_loads_once_and_preserves_other_bytes) {
    const auto original = block_data(0x31);
    auto backing = myrd::test::FakeBackingStoreBuilder(41)
                       .data_block(0, original)
                       .build();
    auto store = require_store(backing_args(backing));
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xF4});
    REQUIRE(store->write(
        myrd::OperationContext{}, 2 * kSector, patch).ok());

    auto expected = original;
    std::copy(patch.begin(), patch.end(), expected.begin() + 2 * kSector);
    const auto output = read_bytes(*store, 0, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), expected.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 1ull);
    REQUIRE_EQ(entry.dirty_base_generation, 3ull);
}

TEST_CASE(m2_write_partial_all_zero_and_zero_states_start_from_a_zero_image) {
    for (const auto source_state : {
             myrd::BlockState::Unresident,
             myrd::BlockState::ZeroClean,
             myrd::BlockState::ZeroDirty}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(42)
                           .all_zero_block(0)
                           .build();
        auto store = require_store(source_state == myrd::BlockState::Unresident
            ? backing_args(backing)
            : clean_zero_args());
        if (source_state == myrd::BlockState::ZeroDirty) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
                *store, 0, source_state, 3));
        }
        std::array<std::byte, kSector> patch{};
        patch.fill(std::byte{0x7C});
        REQUIRE(store->write(
            myrd::OperationContext{}, 3 * kSector, patch).ok());
        const auto output = read_bytes(*store, 0, kBlock);
        REQUIRE(all_equal(
            std::span(output).first(3 * kSector), std::byte{0}));
        REQUIRE(std::equal(
            std::span(output).subspan(3 * kSector, kSector).begin(),
            std::span(output).subspan(3 * kSector, kSector).end(),
            patch.begin()));
        REQUIRE(all_equal(
            std::span(output).subspan(4 * kSector), std::byte{0}));
        REQUIRE_EQ(backing->block_load_count(0),
                   source_state == myrd::BlockState::Unresident ? 1ull : 0ull);
    }
}

TEST_CASE(m2_write_partial_resident_changes_only_requested_bytes) {
    const auto original = block_data(0x55);
    for (const auto source_state : {
             myrd::BlockState::ResidentClean,
             myrd::BlockState::ResidentDirty}) {
        auto store = require_store(clean_zero_args());
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, 0, source_state, original, 3));
        std::array<std::byte, kSector> patch{};
        patch.fill(std::byte{0x09});
        REQUIRE(store->write(
            myrd::OperationContext{}, 5 * kSector, patch).ok());
        auto expected = original;
        std::copy(patch.begin(), patch.end(), expected.begin() + 5 * kSector);
        const auto output = read_bytes(*store, 0, kBlock);
        REQUIRE(std::equal(output.begin(), output.end(), expected.begin()));
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(entry.version, 1ull);
    }
}

TEST_CASE(m2_write_cross_block_and_full_last_logical_block_are_exact) {
    const auto logical_size = 2 * kBlock + 4096;
    auto store = require_store(clean_zero_args(logical_size));
    std::array<std::byte, 2 * kSector> cross{};
    cross.fill(std::byte{0xB1});
    REQUIRE(store->write(
        myrd::OperationContext{}, kBlock - kSector, cross).ok());
    const auto cross_output = read_bytes(
        *store, kBlock - kSector, cross.size());
    REQUIRE(std::equal(
        cross_output.begin(), cross_output.end(), cross.begin()));

    auto backing = myrd::test::FakeBackingStoreBuilder(43).build();
    auto last_store = require_store(backing_args(backing, logical_size));
    std::vector<std::byte> last(4096, std::byte{0xD8});
    REQUIRE(last_store->write(
        myrd::OperationContext{}, 2 * kBlock, last).ok());
    const auto last_output = read_bytes(*last_store, 2 * kBlock, last.size());
    REQUIRE(last_output == last);
    REQUIRE_EQ(backing->block_load_count(2), 0ull);
}

TEST_CASE(m2_write_invalid_ranges_fail_before_metadata_or_stats_change) {
    auto store = require_store(clean_zero_args());
    const auto before = store->stats();
    std::array<std::byte, kSector> sector{};
    std::array<std::byte, kSector - 1> short_sector{};
    const std::span<const std::byte> empty{};
    for (const auto result : {
             store->write(myrd::OperationContext{}, 0, empty),
             store->write(myrd::OperationContext{}, 1, sector),
             store->write(myrd::OperationContext{}, 0, short_sector),
             store->write(
                 myrd::OperationContext{}, store->size_bytes(), sector)}) {
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::invalid_range));
    }
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);
    REQUIRE(store->stats() == before);
}

TEST_CASE(m2_write_hard_limit_rejects_new_commit_but_allows_resident_overwrite) {
    const auto old = block_data(0x66);
    const auto replacement = block_data(0xA2);
    auto backing = myrd::test::FakeBackingStoreBuilder(44)
                       .data_block(4, old)
                       .build();
    auto store = require_store(backing_args(backing));
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            old,
            3));
    }
    const auto committed = store->stats().committed_bytes;
    const auto rejected = store->write(
        myrd::OperationContext{}, 4 * kBlock, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE_EQ(backing->block_load_count(4), 0ull);
    REQUIRE_EQ(store->stats().committed_bytes, committed);
    myrd::BlockEntry before_partial{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, before_partial));

    std::array<std::byte, kSector> partial{};
    partial.fill(std::byte{0x5A});
    const auto partial_rejected = store->write(
        myrd::OperationContext{}, 4 * kBlock + kSector, partial);
    REQUIRE(!partial_rejected.ok());
    REQUIRE_EQ(partial_rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE_EQ(backing->block_load_count(4), 1ull);
    myrd::BlockEntry rejected_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, rejected_entry));
    REQUIRE_EQ(rejected_entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(rejected_entry.version, 0ull);
    REQUIRE_EQ(rejected_entry.dirty_base_generation,
               before_partial.dirty_base_generation);
    REQUIRE_EQ(store->stats().committed_bytes, committed);

    REQUIRE(store->write(
        myrd::OperationContext{}, 0, replacement).ok());
    const auto output = read_bytes(*store, 0, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), replacement.begin()));
    REQUIRE_EQ(store->stats().committed_bytes, committed);
}

TEST_CASE(m2_write_hard_limit_owner_still_serves_waiting_reader_read_through) {
    const auto backing_data = block_data(0x73);
    const auto resident_data = block_data(0x28);
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(49)
                       .data_block(4, backing_data)
                       .gate_block(4, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident_data,
            3));
    }
    const auto committed_before = store->stats().committed_bytes;
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xA9});
    std::vector<std::byte> reader_output(kBlock);
    std::array<std::uint32_t, 2> codes{};

    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 4 * kBlock + kSector, patch);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 4 * kBlock, reader_output);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    const bool reader_registered = wait_for_waiter_count(*store, 1);
    gate->open();
    writer.join();
    reader.join();

    REQUIRE(owner_entered);
    REQUIRE(reader_registered);
    REQUIRE_EQ(codes[0],
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE(std::equal(
        reader_output.begin(), reader_output.end(), backing_data.begin()));
    REQUIRE_EQ(backing->block_load_count(4), 1ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, committed_before);
    REQUIRE_EQ(stats.uncached_reads_total, 1ull);
    REQUIRE_EQ(stats.hard_limit_rejections, 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry.version, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_hard_limit_waiter_does_not_turn_reader_owner_into_failure) {
    const auto backing_data = block_data(0x74);
    const auto resident_data = block_data(0x29);
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(50)
                       .data_block(4, backing_data)
                       .gate_block(4, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident_data,
            3));
    }
    std::vector<std::byte> reader_output(kBlock);
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xAA});
    std::array<std::uint32_t, 2> codes{};

    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 4 * kBlock, reader_output);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 4 * kBlock + kSector, patch);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    const bool writer_registered = wait_for_waiter_count(*store, 1);
    gate->open();
    reader.join();
    writer.join();

    REQUIRE(owner_entered);
    REQUIRE(writer_registered);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1],
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE(std::equal(
        reader_output.begin(), reader_output.end(), backing_data.begin()));
    const auto mixed_load_count = backing->block_load_count(4);
    const auto mixed_stats = store->stats();
    REQUIRE_EQ(mixed_load_count, 1ull);
    REQUIRE_EQ(mixed_stats.uncached_reads_total, 1ull);
    REQUIRE_EQ(mixed_stats.hard_limit_rejections, 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry.version, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_backing_and_commit_failures_preserve_logical_content) {
    auto failing_backing = myrd::test::FakeBackingStoreBuilder(45)
                               .fail_block(0)
                               .build();
    auto backing_store = require_store(backing_args(failing_backing));
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xE3});
    const auto backing_failure = backing_store->write(
        myrd::OperationContext{}, kSector, patch);
    REQUIRE(!backing_failure.ok());
    REQUIRE_EQ(backing_failure.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::backing_read_failed));
    myrd::BlockEntry backing_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *backing_store, 0, backing_entry));
    REQUIRE_EQ(backing_entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(backing_entry.version, 0ull);
    REQUIRE_EQ(backing_entry.dirty_base_generation, 3ull);

    auto memory_api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    memory_api->fail(
        myrd::test::FakeVirtualMemoryOperation::Commit,
        ERROR_NOT_ENOUGH_MEMORY);
    auto commit_store = require_store(
        clean_zero_args(),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    const auto commit_failure = commit_store->write(
        myrd::OperationContext{}, 0, block_data(0x77));
    REQUIRE(!commit_failure.ok());
    REQUIRE_EQ(commit_failure.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::commit_failed));
    const auto output = read_bytes(*commit_store, 0, kBlock);
    REQUIRE(all_equal(output, std::byte{0}));
    REQUIRE_EQ(commit_store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(commit_store->stats().commit_failures, 1ull);
    myrd::BlockEntry commit_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *commit_store, 0, commit_entry));
    REQUIRE_EQ(commit_entry.version, 0ull);
    REQUIRE_EQ(commit_entry.dirty_base_generation, 3ull);

    const auto old_data = block_data(0x35);
    auto partial_backing = myrd::test::FakeBackingStoreBuilder(51)
                               .data_block(0, old_data)
                               .build();
    auto partial_memory_api =
        std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    partial_memory_api->fail(
        myrd::test::FakeVirtualMemoryOperation::Commit,
        ERROR_NOT_ENOUGH_MEMORY);
    auto partial_store = require_store(
        backing_args(partial_backing),
        myrd::DynamicBlockStoreTestDependencies{partial_memory_api});
    const auto partial_failure = partial_store->write(
        myrd::OperationContext{}, kSector, patch);
    REQUIRE(!partial_failure.ok());
    REQUIRE_EQ(partial_failure.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::commit_failed));
    REQUIRE_EQ(partial_backing->block_load_count(0), 1ull);
    myrd::BlockEntry partial_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *partial_store, 0, partial_entry));
    REQUIRE_EQ(partial_entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(partial_entry.version, 0ull);
    REQUIRE_EQ(partial_entry.dirty_base_generation, 3ull);
    REQUIRE_EQ(partial_store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(partial_store->stats().resident_dirty_blocks, 0ull);
    REQUIRE_EQ(partial_store->stats().commit_failures, 1ull);
    REQUIRE(partial_store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_cancel_during_private_backing_load_rolls_back_unresident) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(52)
                       .data_block(0, block_data(0x44))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xD5});
    std::stop_source stop;
    std::uint32_t code{};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{stop.get_token()}, kSector, patch);
        code = result.ok() ? 0 : result_code(result);
    });
    const bool load_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    stop.request_stop();
    gate->open();
    writer.join();

    REQUIRE(load_entered);
    REQUIRE_EQ(code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry.version, 0ull);
    REQUIRE_EQ(entry.dirty_base_generation, 3ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 0ull);
    REQUIRE_EQ(stats.resident_clean_blocks, 0ull);
    REQUIRE_EQ(stats.resident_dirty_blocks, 0ull);
    REQUIRE_EQ(stats.loading_blocks, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_owner_cancel_does_not_cancel_independent_read_waiter) {
    const auto old_data = block_data(0x45);
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(53)
                       .data_block(0, old_data)
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::array<std::byte, kSector> patch{};
    patch.fill(std::byte{0xD6});
    std::vector<std::byte> reader_output(kBlock, std::byte{0xEE});
    std::stop_source writer_stop;
    std::array<std::uint32_t, 2> codes{};

    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{writer_stop.get_token()}, kSector, patch);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, reader_output);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    const bool reader_registered = wait_for_waiter_count(*store, 1);
    writer_stop.request_stop();
    writer.join();
    gate->open();
    reader.join();

    REQUIRE(owner_entered);
    REQUIRE(reader_registered);
    REQUIRE_EQ(codes[0],
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE(std::equal(
        reader_output.begin(), reader_output.end(), old_data.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 2ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry.version, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_version_overflow_is_fatal_and_preserves_content) {
    const auto original = block_data(0x6B);
    auto store = require_store(clean_zero_args());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentDirty, original, 3));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_version(
        *store, 0, (std::numeric_limits<std::uint64_t>::max)()));
    const auto before = store->stats();

    const auto result = store->write(
        myrd::OperationContext{}, 0, block_data(0xC4));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant));
    const auto output = read_bytes(*store, 0, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), original.begin()));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version,
               (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE_EQ(entry.dirty_base_generation, 3ull);
    REQUIRE(store->stats() == before);
}

TEST_CASE(m2_write_cancel_deadline_and_late_block_failure_keep_precise_prefix) {
    auto untouched_store = require_store(clean_zero_args());
    const auto input = block_data(0x90);
    std::stop_source stopped;
    stopped.request_stop();
    const auto cancelled = untouched_store->write(
        myrd::OperationContext{stopped.get_token()}, 0, input);
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(cancelled.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    const auto timed_out = untouched_store->write(
        myrd::OperationContext{
            {}, std::chrono::steady_clock::now() - std::chrono::seconds(1)},
        0,
        input);
    REQUIRE(!timed_out.ok());
    REQUIRE_EQ(timed_out.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE_EQ(untouched_store->allocated_leaf_count_for_test(), 0ull);

    auto failing = myrd::test::FakeBackingStoreBuilder(46)
                       .fail_block(1)
                       .build();
    auto prefix_store = require_store(backing_args(failing, 2 * kBlock));
    std::vector<std::byte> request(kBlock + kSector, std::byte{0x4D});
    const auto result = prefix_store->write(
        myrd::OperationContext{}, 0, request);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::backing_read_failed));
    const auto prefix = read_bytes(*prefix_store, 0, kBlock);
    REQUIRE(all_equal(prefix, std::byte{0x4D}));
    myrd::BlockEntry first{}, second{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *prefix_store, 0, first));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *prefix_store, 1, second));
    REQUIRE_EQ(first.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(first.version, 1ull);
    REQUIRE_EQ(second.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(second.version, 0ull);
}

TEST_CASE(m2_write_captures_generation_at_publish_and_increments_each_block_once) {
    auto backing = myrd::test::FakeBackingStoreBuilder(47).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(48).build();
    auto store = require_store(backing_args(backing, 2 * kBlock, 3));
    REQUIRE(store->install_backing_view({7, successor}).ok());
    std::vector<std::byte> input(2 * kBlock, std::byte{0xC2});
    REQUIRE(store->write(myrd::OperationContext{}, 0, input).ok());
    for (std::uint64_t block = 0; block < 2; ++block) {
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, block, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
        REQUIRE_EQ(entry.version, 1ull);
        REQUIRE_EQ(entry.dirty_base_generation, 7ull);
        REQUIRE(entry.dirty_epoch != 0);
    }
}

TEST_CASE(m2_write_identical_bytes_still_publish_one_new_version) {
    const auto bytes = block_data(0x2E);
    auto store = require_store(clean_zero_args());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentDirty, bytes, 3));

    REQUIRE(store->write(myrd::OperationContext{}, 0, bytes).ok());
    myrd::BlockEntry first{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first));
    REQUIRE_EQ(first.version, 1ull);
    REQUIRE(first.dirty_epoch != 0);

    REQUIRE(store->write(myrd::OperationContext{}, 0, bytes).ok());
    myrd::BlockEntry second{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, second));
    REQUIRE_EQ(second.version, 2ull);
    REQUIRE(second.dirty_epoch > first.dirty_epoch);
    REQUIRE_EQ(second.dirty_base_generation, 3ull);
    const auto output = read_bytes(*store, 0, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), bytes.begin()));
}

TEST_CASE(m2_write_cancel_while_waiting_for_pin_restores_resident_state) {
    const auto old_data = block_data(0x19);
    const auto replacement = block_data(0xE1);
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(clean_zero_args(), dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentDirty, old_data, 3));
    const auto before = store->stats();

    std::vector<std::byte> old_output(kBlock);
    std::uint32_t old_reader_code{};
    std::thread old_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, old_output);
        old_reader_code = result.ok() ? 0 : result_code(result);
    });
    const bool old_reader_pinned = observer->wait_until_entered();
    std::stop_source writer_stop;
    std::uint32_t writer_code{};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{writer_stop.get_token()},
            0,
            replacement);
        writer_code = result.ok() ? 0 : result_code(result);
    });
    const bool writer_committing = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    writer_stop.request_stop();
    writer.join();

    myrd::BlockEntry restored{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, restored));
    REQUIRE(old_reader_pinned);
    REQUIRE(writer_committing);
    REQUIRE_EQ(writer_code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(restored.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(restored.version, 0ull);
    REQUIRE_EQ(restored.dirty_base_generation, 3ull);
    REQUIRE(store->stats() == before);

    observer->release();
    old_reader.join();
    REQUIRE_EQ(old_reader_code, 0u);
    REQUIRE(std::equal(
        old_output.begin(), old_output.end(), old_data.begin()));
    const auto output = read_bytes(*store, 0, kBlock);
    REQUIRE(std::equal(output.begin(), output.end(), old_data.begin()));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_write_reader_race_observes_only_complete_old_or_new_blocks) {
    const auto old_data = block_data(0x10);
    const auto new_data = block_data(0xD0);
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(clean_zero_args(), dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, old_data, 3));

    std::vector<std::byte> old_output(kBlock), new_output(kBlock);
    std::array<std::uint32_t, 3> codes{};
    std::thread old_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, old_output);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool old_reader_pinned = observer->wait_until_entered();
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, new_data);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    const bool writer_committing = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    std::thread new_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, new_output);
        codes[2] = result.ok() ? 0 : result_code(result);
    });
    const bool both_writer_and_reader_waiting = wait_for_waiter_count(*store, 2);
    observer->release();
    old_reader.join();
    writer.join();
    new_reader.join();

    REQUIRE(old_reader_pinned);
    REQUIRE(writer_committing);
    REQUIRE(both_writer_and_reader_waiting);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE_EQ(codes[2], 0u);
    REQUIRE(std::equal(
        old_output.begin(), old_output.end(), old_data.begin()));
    REQUIRE(std::equal(
        new_output.begin(), new_output.end(), new_data.begin()));
    REQUIRE(store->validate_invariants_for_test().ok());
}
