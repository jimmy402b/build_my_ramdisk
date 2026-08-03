#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
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

myrd::DynamicBlockStoreCreateArgs clean_zero_args(
    std::uint64_t logical_size = 8 * kBlock,
    std::uint64_t max_snapshot_bytes = 4 * kBlock,
    std::size_t max_snapshot_records = 8,
    myrd::BackingGeneration generation = 3) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_size,
        static_cast<std::uint32_t>(kSector),
        myrd::DynamicBlockStoreOptions{
            2 * kBlock,
            8 * kBlock,
            kBlock,
            max_snapshot_bytes,
            max_snapshot_records,
            64,
        },
        myrd::InitialBlockState::CleanZero,
        {},
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

std::vector<myrd::DirtyBlockSnapshot> require_batch(
    myrd::DynamicBlockStore& store,
    myrd::DirtyScanCursor& cursor,
    std::size_t max_bytes) {
    auto result = store.copy_dirty_batch(
        myrd::OperationContext{}, cursor, max_bytes);
    REQUIRE(result.ok());
    return result.move_value();
}

void write_block(myrd::DynamicBlockStore& store,
                 std::uint64_t block_index,
                 const myrd::test::FakeBackingStore::BlockData& data) {
    REQUIRE(store.write(
        myrd::OperationContext{}, block_index * kBlock, data).ok());
}

void full_range(myrd::DynamicBlockStore& store,
                std::uint64_t block_index,
                bool discard) {
    const myrd::DataRange range{block_index * kBlock, kBlock};
    const auto ranges = std::span<const myrd::DataRange>(&range, 1);
    REQUIRE(discard
        ? store.unmap(myrd::OperationContext{}, ranges).ok()
        : store.zero(myrd::OperationContext{}, ranges).ok());
}

} // namespace

TEST_CASE(m2_snapshot_empty_scan_completes_without_materializing_metadata) {
    auto store = require_store(clean_zero_args());
    myrd::DirtyScanCursor cursor{};
    auto batch = require_batch(*store, cursor, kBlock);
    REQUIRE(batch.empty());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 0ull);

    batch = require_batch(*store, cursor, kBlock);
    REQUIRE(batch.empty());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    cursor.next_block_index = 0;
    batch = require_batch(*store, cursor, kBlock);
    REQUIRE(batch.empty());
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
}

TEST_CASE(m2_snapshot_scans_sparse_dirty_blocks_in_ascending_order) {
    auto store = require_store(clean_zero_args(3 * 1024 * kBlock));
    const auto bytes = block_data(0x21);
    write_block(*store, 2049, bytes);
    write_block(*store, 1, bytes);
    full_range(*store, 1025, false);
    myrd::DirtyScanCursor cursor{};

    const auto batch = require_batch(*store, cursor, 4 * kBlock);

    REQUIRE_EQ(batch.size(), 3u);
    REQUIRE_EQ(batch[0].block_index, 1ull);
    REQUIRE_EQ(batch[1].block_index, 1025ull);
    REQUIRE_EQ(batch[2].block_index, 2049ull);
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());
    REQUIRE_EQ(store->allocated_leaf_count_for_test(), 3ull);
}

TEST_CASE(m2_snapshot_preserves_data_zero_discard_metadata_and_payloads) {
    auto store = require_store(clean_zero_args());
    const auto data = block_data(0x31);
    write_block(*store, 0, data);
    full_range(*store, 1, false);
    full_range(*store, 2, true);
    myrd::DirtyScanCursor cursor{};

    const auto batch = require_batch(*store, cursor, 4 * kBlock);

    REQUIRE_EQ(batch.size(), 3u);
    REQUIRE_EQ(batch[0].block_index, 0ull);
    REQUIRE_EQ(batch[0].version, 1ull);
    REQUIRE_EQ(batch[0].kind, myrd::DirtyRecordKind::Data);
    REQUIRE_EQ(batch[0].base_generation, 3ull);
    REQUIRE_EQ(batch[0].data.size(), static_cast<std::size_t>(kBlock));
    REQUIRE(std::equal(
        batch[0].data.begin(), batch[0].data.end(), data.begin()));
    REQUIRE_EQ(batch[1].kind, myrd::DirtyRecordKind::Zero);
    REQUIRE_EQ(batch[1].base_generation, 3ull);
    REQUIRE(batch[1].data.empty());
    REQUIRE_EQ(batch[2].kind, myrd::DirtyRecordKind::Discard);
    REQUIRE_EQ(batch[2].base_generation, 3ull);
    REQUIRE(batch[2].data.empty());
    REQUIRE_EQ(store->stats().snapshot_records_total, 3ull);
}

TEST_CASE(m2_snapshot_enforces_byte_and_record_caps_without_losing_progress) {
    auto store = require_store(clean_zero_args(6 * kBlock, 2 * kBlock, 2));
    write_block(*store, 0, block_data(0x41));
    full_range(*store, 1, false);
    full_range(*store, 2, true);
    write_block(*store, 3, block_data(0x51));
    write_block(*store, 4, block_data(0x61));
    myrd::DirtyScanCursor cursor{};

    auto batch = require_batch(*store, cursor, kBlock);
    REQUIRE_EQ(batch.size(), 2u);
    REQUIRE_EQ(batch[0].block_index, 0ull);
    REQUIRE_EQ(batch[1].block_index, 1ull);
    REQUIRE_EQ(cursor.next_block_index, 2ull);

    batch = require_batch(*store, cursor, 8 * kBlock);
    REQUIRE_EQ(batch.size(), 2u);
    REQUIRE_EQ(batch[0].block_index, 2ull);
    REQUIRE_EQ(batch[1].block_index, 3ull);
    REQUIRE_EQ(cursor.next_block_index, 4ull);

    batch = require_batch(*store, cursor, 8 * kBlock);
    REQUIRE_EQ(batch.size(), 1u);
    REQUIRE_EQ(batch[0].block_index, 4ull);
    REQUIRE_EQ(cursor.next_block_index, store->total_blocks());

    cursor.next_block_index = 0;
    auto configured_store = require_store(
        clean_zero_args(3 * kBlock, kBlock, 8));
    write_block(*configured_store, 0, block_data(0x71));
    write_block(*configured_store, 1, block_data(0x81));
    batch = require_batch(*configured_store, cursor, 4 * kBlock);
    REQUIRE_EQ(batch.size(), 1u);
    REQUIRE_EQ(cursor.next_block_index, 1ull);
}

TEST_CASE(m2_snapshot_partial_final_block_zeroes_outside_logical_size) {
    constexpr std::uint64_t logical_size = kBlock + 4096;
    auto store = require_store(clean_zero_args(logical_size));
    auto resident = block_data(0x91);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 1, myrd::BlockState::ResidentDirty, resident, 3));
    myrd::DirtyScanCursor cursor{1};

    const auto batch = require_batch(*store, cursor, kBlock);

    REQUIRE_EQ(batch.size(), 1u);
    REQUIRE_EQ(batch[0].data.size(), static_cast<std::size_t>(kBlock));
    REQUIRE(std::equal(batch[0].data.begin(),
                       batch[0].data.begin() + 4096,
                       resident.begin()));
    REQUIRE(std::all_of(
        batch[0].data.begin() + 4096,
        batch[0].data.end(),
        [](std::byte byte) { return byte == std::byte{0}; }));
}

TEST_CASE(m2_snapshot_owned_copy_is_isolated_from_later_mutation) {
    auto store = require_store(clean_zero_args());
    const auto old_data = block_data(0xA1);
    const auto new_data = block_data(0xB1);
    write_block(*store, 0, old_data);
    myrd::DirtyScanCursor cursor{};
    const auto batch = require_batch(*store, cursor, kBlock);

    write_block(*store, 0, new_data);

    REQUIRE_EQ(batch.size(), 1u);
    REQUIRE(std::equal(
        batch[0].data.begin(), batch[0].data.end(), old_data.begin()));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.version, 2ull);
}

TEST_CASE(m2_snapshot_pin_serializes_exact_persisted_ack) {
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_after_snapshot_pin = true;
    auto store = require_store(clean_zero_args(), std::move(dependencies));
    write_block(*store, 0, block_data(0xB7));

    myrd::DirtyScanCursor cursor{};
    std::atomic<std::uint32_t> snapshot_code{};
    std::atomic<std::size_t> snapshot_records{};
    std::thread snapshot([&] {
        const auto result = store->copy_dirty_batch(
            myrd::OperationContext{}, cursor, kBlock);
        snapshot_code.store(result.ok() ? 0 : result.error().code,
                            std::memory_order_release);
        snapshot_records.store(result.ok() ? result.value().size() : 0,
                               std::memory_order_release);
    });

    const auto pin_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!myrd::DynamicBlockStoreTestAccess::snapshot_pin_gap_reached(
               *store) &&
           std::chrono::steady_clock::now() < pin_deadline) {
        std::this_thread::yield();
    }
    const bool pin_reached =
        myrd::DynamicBlockStoreTestAccess::snapshot_pin_gap_reached(*store);
    auto successor = myrd::test::FakeBackingStoreBuilder(92).build();
    const bool installed = store->install_backing_view({4, successor}).ok();

    std::atomic<bool> ack_finished{};
    std::atomic<std::uint32_t> ack_code{};
    std::thread acknowledger([&] {
        const auto result = store->mark_persisted(
            0, 1, myrd::DirtyRecordKind::Data, 4);
        ack_code.store(result.ok() ? 0 : result.error().code,
                       std::memory_order_release);
        ack_finished.store(true, std::memory_order_release);
    });
    const auto ack_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ack_finished.load(std::memory_order_acquire) &&
           store->stripe_waiter_count_for_test() == 0 &&
           std::chrono::steady_clock::now() < ack_deadline) {
        std::this_thread::yield();
    }
    const bool ack_engaged =
        ack_finished.load(std::memory_order_acquire) ||
        store->stripe_waiter_count_for_test() != 0;
    const bool pin_released =
        myrd::DynamicBlockStoreTestAccess::release_snapshot_pin_gap(*store);

    snapshot.join();
    acknowledger.join();

    REQUIRE(pin_reached);
    REQUIRE(installed);
    REQUIRE(ack_engaged);
    REQUIRE(pin_released);
    REQUIRE_EQ(snapshot_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(snapshot_records.load(std::memory_order_acquire), 1u);
    REQUIRE_EQ(ack_code.load(std::memory_order_acquire), 0u);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry.clean_generation, 4ull);
}

TEST_CASE(m2_snapshot_invalid_limits_and_cancellation_preserve_cursor_and_dirty) {
    auto store = require_store(clean_zero_args());
    write_block(*store, 0, block_data(0xC1));
    for (const std::size_t max_bytes : {
             std::size_t{0},
             static_cast<std::size_t>(kBlock - 1),
             static_cast<std::size_t>(kBlock + kSector)}) {
        myrd::DirtyScanCursor cursor{};
        const auto result = store->copy_dirty_batch(
            myrd::OperationContext{}, cursor, max_bytes);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::invalid_range));
        REQUIRE_EQ(cursor.next_block_index, 0ull);
    }

    myrd::DirtyScanCursor out_of_range{store->total_blocks() + 1};
    const auto bad_cursor = store->copy_dirty_batch(
        myrd::OperationContext{}, out_of_range, kBlock);
    REQUIRE(!bad_cursor.ok());
    REQUIRE_EQ(out_of_range.next_block_index,
               store->total_blocks() + 1);

    std::stop_source stop;
    stop.request_stop();
    myrd::DirtyScanCursor cancelled_cursor{};
    const auto cancelled = store->copy_dirty_batch(
        myrd::OperationContext{stop.get_token()},
        cancelled_cursor,
        kBlock);
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(cancelled.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(cancelled_cursor.next_block_index, 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 1ull);
}

TEST_CASE(m2_snapshot_waiting_cancellation_cannot_lose_wakeup) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(91)
                       .gate_block(0, gate)
                       .build();
    auto args = clean_zero_args();
    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    args.backing_store = backing;
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_before_snapshot_wait = true;
    auto store = require_store(std::move(args), std::move(dependencies));

    std::vector<std::byte> owner_output(kBlock);
    std::atomic<std::uint32_t> owner_code{};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, owner_output);
        owner_code.store(result.ok() ? 0 : result.error().code,
                         std::memory_order_release);
    });
    const bool load_started = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));

    std::stop_source stop;
    myrd::DirtyScanCursor cursor{};
    std::atomic<bool> snapshot_finished{};
    std::atomic<std::uint32_t> snapshot_code{};
    std::thread snapshot([&] {
        const auto result = store->copy_dirty_batch(
            myrd::OperationContext{stop.get_token()}, cursor, kBlock);
        snapshot_code.store(result.ok() ? 0 : result.error().code,
                            std::memory_order_release);
        snapshot_finished.store(true, std::memory_order_release);
    });
    const auto wait_gap_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!myrd::DynamicBlockStoreTestAccess::snapshot_wait_gap_reached(
               *store) &&
           std::chrono::steady_clock::now() < wait_gap_deadline) {
        std::this_thread::yield();
    }
    const bool reached_wait_gap =
        myrd::DynamicBlockStoreTestAccess::snapshot_wait_gap_reached(*store);
    std::thread canceller([&] { stop.request_stop(); });
    const auto cancellation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stop.stop_requested() &&
           std::chrono::steady_clock::now() < cancellation_deadline) {
        std::this_thread::yield();
    }
    const bool wait_gap_released =
        myrd::DynamicBlockStoreTestAccess::release_snapshot_wait_gap(*store);

    const auto completion_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!snapshot_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::yield();
    }
    const bool cancelled_before_load_finished =
        snapshot_finished.load(std::memory_order_acquire);

    gate->open();
    canceller.join();
    snapshot.join();
    owner.join();

    REQUIRE(load_started);
    REQUIRE(reached_wait_gap);
    REQUIRE(wait_gap_released);
    REQUIRE(cancelled_before_load_finished);
    REQUIRE_EQ(snapshot_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
}

TEST_CASE(m2_snapshot_allocation_failure_preserves_cursor_and_dirty) {
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.fail_snapshot_allocation = true;
    auto store = require_store(clean_zero_args(), std::move(dependencies));
    write_block(*store, 0, block_data(0xD1));
    myrd::DirtyScanCursor cursor{};

    const auto result = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);

    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE_EQ(cursor.next_block_index, 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 1ull);
}
