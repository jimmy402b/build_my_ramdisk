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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;

myrd::test::FakeBackingStore::BlockData block_data(std::byte value) {
    myrd::test::FakeBackingStore::BlockData data{};
    data.fill(value);
    return data;
}

myrd::DynamicBlockStoreCreateArgs backing_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t logical_size = 5 * kBlock) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_size,
        512,
        myrd::DynamicBlockStoreOptions{
            3 * kBlock,
            4 * kBlock,
            2 * kBlock,
            kBlock,
            8,
            64,
        },
        myrd::InitialBlockState::BackingUnresident,
        std::move(backing),
        3,
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

} // namespace

TEST_CASE(m2_read_commit_failure_rolls_back_transition_and_budget) {
    auto backing = myrd::test::FakeBackingStoreBuilder(30)
                       .data_block(0, block_data(std::byte{0x44}))
                       .build();
    auto memory_api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    memory_api->fail(
        myrd::test::FakeVirtualMemoryOperation::Commit,
        ERROR_NOT_ENOUGH_MEMORY);
    auto store = require_store(
        backing_args(backing),
        myrd::DynamicBlockStoreTestDependencies{memory_api});

    std::vector<std::byte> output(kBlock, std::byte{0xA5});
    const auto result = store->read(myrd::OperationContext{}, 0, output);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::commit_failed));
    REQUIRE(std::all_of(output.begin(), output.end(), [](std::byte value) {
        return value == std::byte{0xA5};
    }));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);

    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 0ull);
    REQUIRE_EQ(stats.loading_blocks, 0ull);
    REQUIRE_EQ(stats.commit_failures, 1ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_read_at_hard_limit_uses_uncached_read_through_and_reloads) {
    const auto expected = block_data(std::byte{0xD1});
    auto backing = myrd::test::FakeBackingStoreBuilder(31)
                       .data_block(4, expected)
                       .build();
    auto store = require_store(backing_args(backing));
    const auto resident = block_data(std::byte{0x19});
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident,
            3));
    }
    const auto before = store->stats();
    REQUIRE_EQ(before.committed_bytes, 4 * kBlock);

    for (std::uint64_t attempt = 1; attempt <= 2; ++attempt) {
        std::vector<std::byte> output(kBlock, std::byte{0});
        REQUIRE(store->read(
            myrd::OperationContext{}, 4 * kBlock, output).ok());
        REQUIRE(std::equal(output.begin(), output.end(), expected.begin()));
        REQUIRE_EQ(backing->block_load_count(4), attempt);

        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 4, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
        const auto stats = store->stats();
        REQUIRE_EQ(stats.committed_bytes, before.committed_bytes);
        REQUIRE_EQ(stats.uncached_reads_total, attempt);
        REQUIRE_EQ(stats.hard_limit_rejections, attempt);
        REQUIRE_EQ(stats.loading_blocks, 0ull);
    }
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_read_at_hard_limit_is_single_flight_for_concurrent_waiters) {
    constexpr std::uint32_t reader_count = 8;
    const auto expected = block_data(std::byte{0xE2});
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(32)
                       .data_block(4, expected)
                       .gate_block(4, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    const auto resident = block_data(std::byte{0x2A});
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident,
            3));
    }
    const auto committed_before = store->stats().committed_bytes;

    std::array<std::vector<std::byte>, reader_count> outputs;
    std::array<std::uint32_t, reader_count> codes{};
    std::array<std::thread, reader_count> readers;
    for (std::uint32_t index = 0; index < reader_count; ++index) {
        outputs[index].assign(kBlock, std::byte{0});
        readers[index] = std::thread([&, index] {
            const auto result = store->read(
                myrd::OperationContext{}, 4 * kBlock, outputs[index]);
            codes[index] = result.ok() ? 0 : result_code(result);
        });
    }
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    const bool all_waiters_registered =
        wait_for_stripe_waiters(*store, reader_count - 1);
    gate->open();
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(owner_entered);
    REQUIRE(all_waiters_registered);
    for (std::uint32_t index = 0; index < reader_count; ++index) {
        REQUIRE_EQ(codes[index], 0u);
        REQUIRE(std::equal(
            outputs[index].begin(), outputs[index].end(), expected.begin()));
    }
    REQUIRE_EQ(backing->block_load_count(4), 1ull);
    auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, committed_before);
    REQUIRE_EQ(stats.uncached_reads_total,
               static_cast<std::uint64_t>(reader_count));
    REQUIRE_EQ(stats.hard_limit_rejections, 1ull);

    std::vector<std::byte> reload(kBlock, std::byte{0});
    REQUIRE(store->read(
        myrd::OperationContext{}, 4 * kBlock, reload).ok());
    REQUIRE(std::equal(reload.begin(), reload.end(), expected.begin()));
    REQUIRE_EQ(backing->block_load_count(4), 2ull);
    stats = store->stats();
    REQUIRE_EQ(stats.uncached_reads_total,
               static_cast<std::uint64_t>(reader_count + 1));
    REQUIRE_EQ(stats.hard_limit_rejections, 2ull);
    REQUIRE_EQ(stats.committed_bytes, committed_before);
    REQUIRE(store->validate_invariants_for_test().ok());
}
