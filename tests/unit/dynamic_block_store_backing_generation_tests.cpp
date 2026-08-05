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
#include <thread>
#include <utility>
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
            2 * kBlock,
            8,
            64,
            myrd::kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250},
        },
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

std::vector<std::byte> read_block(myrd::DynamicBlockStore& store,
                                  std::uint64_t block_index) {
    std::vector<std::byte> output(kBlock);
    REQUIRE(store.read(
        myrd::OperationContext{}, block_index * kBlock, output).ok());
    return output;
}

std::uint32_t result_code(const myrd::Result<void>& result) {
    return result.ok()
        ? 0
        : result.error().code;
}

bool wait_for_install_waiters(const myrd::DynamicBlockStore& store,
                              std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (store.backing_install_waiter_count_for_test() != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return store.backing_install_waiter_count_for_test() == expected;
}

} // namespace

TEST_CASE(m2_backing_install_validates_generation_and_preserves_coherent_view) {
    auto initial = myrd::test::FakeBackingStoreBuilder(10).build();
    auto store = require_store(backing_args(initial));
    auto view = store->backing_view_for_test();
    REQUIRE_EQ(view.generation, 3ull);
    REQUIRE(view.store == initial);

    auto successor = myrd::test::FakeBackingStoreBuilder(11).build();
    REQUIRE(store->install_backing_view({4, successor}).ok());
    view = store->backing_view_for_test();
    REQUIRE_EQ(view.generation, 4ull);
    REQUIRE(view.store == successor);
    REQUIRE_EQ(store->backing_generation(), 4ull);
    REQUIRE_EQ(store->stats().backing_view_installs_total, 1ull);

    const auto before_conflicts = store->stats().backing_generation_conflicts;
    for (auto invalid : {
             myrd::BackingView{5, {}},
             myrd::BackingView{4, initial},
             myrd::BackingView{3, initial}}) {
        const auto result = store->install_backing_view(std::move(invalid));
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::DynamicBlockStoreErrorCode::
                           BackingGenerationConflict));
    }
    view = store->backing_view_for_test();
    REQUIRE_EQ(view.generation, 4ull);
    REQUIRE(view.store == successor);
    REQUIRE_EQ(store->stats().backing_generation_conflicts,
               before_conflicts + 3);

    auto final_view = myrd::test::FakeBackingStoreBuilder(12).build();
    REQUIRE(store->install_backing_view(
        {(std::numeric_limits<std::uint64_t>::max)(), final_view}).ok());
    const auto wrapped = store->install_backing_view({0, initial});
    REQUIRE(!wrapped.ok());
    REQUIRE_EQ(store->backing_generation(),
               (std::numeric_limits<std::uint64_t>::max)());
}

TEST_CASE(m2_backing_view_generation_and_pointer_are_never_torn) {
    auto initial = myrd::test::FakeBackingStoreBuilder(20).build();
    auto store = require_store(backing_args(initial));
    std::vector<std::pair<myrd::BackingGeneration,
                          std::shared_ptr<const myrd::IBackingStore>>> views;
    views.emplace_back(3, initial);
    for (std::uint64_t generation = 4; generation <= 32; ++generation) {
        views.emplace_back(
            generation,
            myrd::test::FakeBackingStoreBuilder(generation).build());
    }
    std::atomic<bool> stop{};
    std::atomic<bool> mismatch{};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto observed = store->backing_view_for_test();
            const auto found = std::find_if(
                views.begin(), views.end(), [&](const auto& candidate) {
                    return candidate.first == observed.generation;
                });
            if (found == views.end() || found->second != observed.store) {
                mismatch.store(true, std::memory_order_release);
                return;
            }
        }
    });
    bool installs_succeeded = true;
    for (std::size_t index = 1; index < views.size(); ++index) {
        installs_succeeded = installs_succeeded &&
            store->install_backing_view(
                {views[index].first, views[index].second}).ok();
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    REQUIRE(installs_succeeded);
    REQUIRE(!mismatch.load(std::memory_order_acquire));
    REQUIRE_EQ(store->backing_generation(), 32ull);
}

TEST_CASE(m2_backing_retired_view_lives_until_last_shared_reference_exits) {
    auto tracker =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    auto initial = myrd::test::FakeBackingStoreBuilder(30)
                       .lifetime_tracker(tracker)
                       .build();
    auto store = require_store(backing_args(initial));
    auto held = store->backing_view_for_test();
    initial.reset();
    auto successor = myrd::test::FakeBackingStoreBuilder(31).build();
    REQUIRE(store->install_backing_view({4, successor}).ok());
    REQUIRE_EQ(tracker->live_count(), 1ull);
    REQUIRE_EQ(tracker->destroyed_count(), 0ull);
    held.store.reset();
    REQUIRE_EQ(tracker->live_count(), 0ull);
    REQUIRE_EQ(tracker->destroyed_count(), 1ull);
}

TEST_CASE(m2_backing_closed_store_rejects_install_without_changing_view) {
    auto initial = myrd::test::FakeBackingStoreBuilder(40).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(41).build();
    auto store = require_store(backing_args(initial));
    store->close();
    const auto result = store->install_backing_view({4, successor});
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    const auto view = store->backing_view_for_test();
    REQUIRE_EQ(view.generation, 3ull);
    REQUIRE(view.store == nullptr);
}

TEST_CASE(m2_backing_close_releases_store_owned_reader_before_destruction) {
    auto tracker =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    auto initial = myrd::test::FakeBackingStoreBuilder(44)
                       .lifetime_tracker(tracker)
                       .build();
    auto store = require_store(backing_args(initial));
    initial.reset();
    REQUIRE_EQ(tracker->live_count(), 1ull);
    REQUIRE_EQ(tracker->destroyed_count(), 0ull);

    store->close();

    REQUIRE(store->is_closed());
    REQUIRE_EQ(tracker->live_count(), 0ull);
    REQUIRE_EQ(tracker->destroyed_count(), 1ull);
    const auto view = store->backing_view_for_test();
    REQUIRE_EQ(view.generation, 3ull);
    REQUIRE(view.store == nullptr);
}

TEST_CASE(m2_backing_install_wait_is_interrupted_by_close) {
    auto initial = myrd::test::FakeBackingStoreBuilder(45).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(46).build();
    auto store = require_store(backing_args(initial));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::
        begin_backing_publication(*store));

    std::atomic<bool> install_finished{};
    std::atomic<std::uint32_t> install_code{};
    std::thread installer([&] {
        const auto result = store->install_backing_view({4, successor});
        install_code.store(result.ok() ? 0 : result.error().code,
                           std::memory_order_release);
        install_finished.store(true, std::memory_order_release);
    });
    const bool installer_waiting = wait_for_install_waiters(*store, 1);

    std::thread closer([&] { store->close(); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!install_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool interrupted_before_publication_finished =
        install_finished.load(std::memory_order_acquire);

    const bool publication_ended =
        myrd::DynamicBlockStoreTestAccess::end_backing_publication(*store);
    installer.join();
    closer.join();

    REQUIRE(publication_ended);
    REQUIRE(installer_waiting);
    REQUIRE(interrupted_before_publication_finished);
    REQUIRE_EQ(install_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(store->backing_generation(), 3ull);
}

TEST_CASE(m2_ack_generation_and_version_errors_leave_dirty_state_unchanged) {
    auto initial = myrd::test::FakeBackingStoreBuilder(50).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(51).build();
    auto store = require_store(backing_args(initial));
    const auto bytes = block_data(0x51);
    REQUIRE(store->write(myrd::OperationContext{}, 0, bytes).ok());

    const auto before_generation_conflicts =
        store->stats().backing_generation_conflicts;
    for (const auto persisted : {3ull, 4ull}) {
        const auto result = store->mark_persisted(
            0, 1, myrd::DirtyRecordKind::Data, persisted);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::DynamicBlockStoreErrorCode::
                           BackingGenerationConflict));
    }
    REQUIRE_EQ(store->stats().backing_generation_conflicts,
               before_generation_conflicts + 2);
    REQUIRE(store->install_backing_view({4, successor}).ok());
    const auto too_new = store->mark_persisted(
        0, 1, myrd::DirtyRecordKind::Data, 5);
    REQUIRE(!too_new.ok());

    const auto before_version_conflicts =
        store->stats().snapshot_version_conflicts;
    for (const auto result : {
             store->mark_persisted(
                 0, 2, myrd::DirtyRecordKind::Data, 4),
             store->mark_persisted(
                 0, 1, myrd::DirtyRecordKind::Zero, 4),
             store->mark_persisted(
                 2, 0, myrd::DirtyRecordKind::Data, 4)}) {
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::version_changed));
    }
    REQUIRE_EQ(store->stats().snapshot_version_conflicts,
               before_version_conflicts + 3);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 1ull);
    REQUIRE_EQ(entry.dirty_base_generation, 3ull);
    REQUIRE(read_block(*store, 0) ==
            std::vector<std::byte>(bytes.begin(), bytes.end()));

    const auto out_of_range = store->mark_persisted(
        store->total_blocks(), 1, myrd::DirtyRecordKind::Data, 4);
    REQUIRE(!out_of_range.ok());
    REQUIRE_EQ(out_of_range.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::invalid_range));
}

TEST_CASE(m2_ack_exact_data_and_zero_records_publish_clean_generation) {
    auto initial = myrd::test::FakeBackingStoreBuilder(60).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(61).build();
    auto store = require_store(backing_args(initial));
    REQUIRE(store->write(
        myrd::OperationContext{}, 0, block_data(0x61)).ok());
    const myrd::DataRange range{kBlock, kBlock};
    REQUIRE(store->unmap(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1)).ok());
    REQUIRE(store->install_backing_view({4, successor}).ok());

    REQUIRE(store->mark_persisted(
        0, 1, myrd::DirtyRecordKind::Data, 4).ok());
    REQUIRE(store->mark_persisted(
        1, 1, myrd::DirtyRecordKind::Discard, 4).ok());

    myrd::BlockEntry data{}, zero{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, data));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, zero));
    REQUIRE_EQ(data.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(zero.state, myrd::BlockState::ZeroClean);
    REQUIRE_EQ(data.clean_generation, 4ull);
    REQUIRE_EQ(zero.clean_generation, 4ull);
    REQUIRE_EQ(data.version, 1ull);
    REQUIRE_EQ(zero.version, 1ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.resident_dirty_blocks, 0ull);
    REQUIRE_EQ(stats.resident_clean_blocks, 1ull);
    REQUIRE_EQ(stats.zero_dirty_blocks, 0ull);
    REQUIRE_EQ(stats.zero_clean_blocks, 1ull);

    const auto already_clean = store->mark_persisted(
        0, 1, myrd::DirtyRecordKind::Data, 4);
    REQUIRE(!already_clean.ok());
    REQUIRE_EQ(already_clean.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::version_changed));
}

TEST_CASE(m2_ack_old_snapshot_cannot_clean_a_newer_dirty_version) {
    auto initial = myrd::test::FakeBackingStoreBuilder(70).build();
    auto successor = myrd::test::FakeBackingStoreBuilder(71).build();
    auto store = require_store(backing_args(initial));
    const auto old_data = block_data(0x71);
    const auto new_data = block_data(0x81);
    REQUIRE(store->write(myrd::OperationContext{}, 0, old_data).ok());
    myrd::DirtyScanCursor cursor{};
    auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    REQUIRE(store->install_backing_view({4, successor}).ok());
    REQUIRE(store->write(myrd::OperationContext{}, 0, new_data).ok());

    const auto stale = store->mark_persisted(
        0,
        snapshot.value()[0].version,
        snapshot.value()[0].kind,
        4);

    REQUIRE(!stale.ok());
    REQUIRE_EQ(stale.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::version_changed));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 2ull);
    REQUIRE_EQ(entry.dirty_base_generation, 4ull);
    REQUIRE(read_block(*store, 0) ==
            std::vector<std::byte>(new_data.begin(), new_data.end()));
}

TEST_CASE(m2_resurrection_data_snapshot_install_ack_evict_reload_is_exact) {
    const auto old_data = block_data(0x11);
    const auto new_data = block_data(0xD1);
    auto initial = myrd::test::FakeBackingStoreBuilder(80)
                       .data_block(0, old_data)
                       .build();
    auto store = require_store(backing_args(initial));
    REQUIRE(store->write(myrd::OperationContext{}, 0, new_data).ok());
    myrd::DirtyScanCursor cursor{};
    auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(*initial)
                         .data_block(0, new_data)
                         .build();
    REQUIRE(store->install_backing_view({4, successor}).ok());
    REQUIRE(store->mark_persisted(
        0,
        snapshot.value()[0].version,
        snapshot.value()[0].kind,
        4).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::try_decommit_resident_clean(
        *store, 0));

    const auto reloaded = read_block(*store, 0);

    REQUIRE(reloaded ==
            std::vector<std::byte>(new_data.begin(), new_data.end()));
    REQUIRE(reloaded !=
            std::vector<std::byte>(old_data.begin(), old_data.end()));
    REQUIRE_EQ(initial->block_load_count(0), 0ull);
    REQUIRE_EQ(successor->block_load_count(0), 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry.clean_generation, 4ull);
}

TEST_CASE(m2_resurrection_zero_snapshot_install_ack_never_reads_old_backing) {
    const auto old_data = block_data(0x22);
    auto initial = myrd::test::FakeBackingStoreBuilder(90)
                       .data_block(0, old_data)
                       .build();
    auto store = require_store(backing_args(initial));
    const myrd::DataRange range{0, kBlock};
    REQUIRE(store->unmap(
        myrd::OperationContext{},
        std::span<const myrd::DataRange>(&range, 1)).ok());
    myrd::DirtyScanCursor cursor{};
    auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    REQUIRE_EQ(snapshot.value()[0].kind, myrd::DirtyRecordKind::Discard);
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(*initial)
                         .all_zero_block(0)
                         .build();
    REQUIRE(store->install_backing_view({4, successor}).ok());
    REQUIRE(store->mark_persisted(
        0,
        snapshot.value()[0].version,
        snapshot.value()[0].kind,
        4).ok());

    const auto output = read_block(*store, 0);

    REQUIRE(std::all_of(output.begin(), output.end(), [](std::byte byte) {
        return byte == std::byte{0};
    }));
    REQUIRE_EQ(initial->block_load_count(0), 0ull);
    REQUIRE_EQ(successor->block_load_count(0), 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ZeroClean);
    REQUIRE_EQ(entry.clean_generation, 4ull);
}
