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

myrd::DynamicBlockStoreCreateArgs limited_backing_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t logical_blocks,
    std::uint64_t low_blocks,
    std::uint64_t soft_blocks,
    std::uint64_t hard_blocks) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_blocks * kBlock,
        512,
        myrd::DynamicBlockStoreOptions{
            soft_blocks * kBlock,
            hard_blocks * kBlock,
            low_blocks * kBlock,
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

bool wait_for_commit_reservation(myrd::DynamicBlockStore& store,
                                 std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(
               store) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(
               store) == expected;
}

bool wait_for_synchronous_eviction_waiters(
    myrd::DynamicBlockStore& store,
    std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (myrd::DynamicBlockStoreTestAccess::synchronous_eviction_waiters(
               store) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return myrd::DynamicBlockStoreTestAccess::synchronous_eviction_waiters(
               store) == expected;
}

bool wait_for_true(const std::atomic<bool>& value) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}

class GatedRealVirtualMemoryApi final : public myrd::IVirtualMemoryApi {
public:
    explicit GatedRealVirtualMemoryApi(bool gate_commits = true) noexcept
        : gate_commits_(gate_commits),
          commits_released_(!gate_commits) {}

    bool wait_until_commit_entered(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(gate_mutex_);
        return gate_condition_.wait_until(
            lock, deadline, [this] { return commit_entered_; });
    }

    void release_commits() {
        {
            std::scoped_lock lock(gate_mutex_);
            commits_released_ = true;
        }
        gate_condition_.notify_all();
    }

    void block_next_decommit() {
        std::scoped_lock lock(gate_mutex_);
        block_decommit_ = true;
        decommit_entered_ = false;
        decommit_released_ = false;
    }

    bool wait_until_decommit_entered(
        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(gate_mutex_);
        return gate_condition_.wait_until(
            lock, deadline, [this] { return decommit_entered_; });
    }

    void release_decommit() {
        {
            std::scoped_lock lock(gate_mutex_);
            decommit_released_ = true;
        }
        gate_condition_.notify_all();
    }

    std::uint64_t decommit_calls() const noexcept {
        return decommit_calls_.load(std::memory_order_acquire);
    }

    void* allocate(void* address,
                   std::size_t size_bytes,
                   std::uint32_t allocation_type,
                   std::uint32_t protection) noexcept override {
        if (address != nullptr &&
            (allocation_type & MEM_COMMIT) != 0 && gate_commits_) {
            try {
                std::unique_lock lock(gate_mutex_);
                commit_entered_ = true;
                gate_condition_.notify_all();
                gate_condition_.wait(
                    lock, [this] { return commits_released_; });
            } catch (...) {
                std::terminate();
            }
        }
        auto* result = ::VirtualAlloc(
            address, size_bytes, allocation_type, protection);
        last_error_.store(
            result != nullptr ? ERROR_SUCCESS : ::GetLastError(),
            std::memory_order_release);
        return result;
    }

    bool free(void* address,
              std::size_t size_bytes,
              std::uint32_t free_type) noexcept override {
        if (free_type == MEM_DECOMMIT) {
            decommit_calls_.fetch_add(1, std::memory_order_relaxed);
            try {
                std::unique_lock lock(gate_mutex_);
                if (block_decommit_) {
                    block_decommit_ = false;
                    decommit_entered_ = true;
                    gate_condition_.notify_all();
                    gate_condition_.wait(
                        lock, [this] { return decommit_released_; });
                }
            } catch (...) {
                std::terminate();
            }
        }
        const auto ok =
            ::VirtualFree(address, size_bytes, free_type) != FALSE;
        last_error_.store(
            ok ? ERROR_SUCCESS : ::GetLastError(),
            std::memory_order_release);
        return ok;
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
            : (native.State == MEM_RESERVE
                ? myrd::VirtualMemoryRegionState::Reserved
                : (native.State == MEM_FREE
                    ? myrd::VirtualMemoryRegionState::Free
                    : myrd::VirtualMemoryRegionState::Unknown));
        last_error_.store(ERROR_SUCCESS, std::memory_order_release);
        return true;
    }

    std::uint32_t last_error() const noexcept override {
        return last_error_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint32_t> last_error_{ERROR_SUCCESS};
    std::atomic<std::uint64_t> decommit_calls_{0};
    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    bool gate_commits_{};
    bool commit_entered_{};
    bool commits_released_{};
    bool block_decommit_{};
    bool decommit_entered_{};
    bool decommit_released_{};
};

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
    REQUIRE_EQ(stats.commit_reservation_rollbacks_total, 1ull);
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

TEST_CASE(m2_hard_pressure_evicts_clean_data_before_read_through) {
    const auto expected = block_data(std::byte{0xB4});
    auto backing = myrd::test::FakeBackingStoreBuilder(40)
                       .data_block(4, expected)
                       .build();
    auto store = require_store(backing_args(backing));
    const auto resident = block_data(std::byte{0x37});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, resident, 3));
    for (std::uint64_t block = 1; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident,
            3));
    }

    std::vector<std::byte> output(kBlock, std::byte{0});
    REQUIRE(store->read(
        myrd::OperationContext{}, 4 * kBlock, output).ok());
    REQUIRE(std::equal(output.begin(), output.end(), expected.begin()));

    myrd::BlockEntry victim{};
    myrd::BlockEntry loaded{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, victim));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, loaded));
    REQUIRE_EQ(victim.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(loaded.state, myrd::BlockState::ResidentClean);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 4 * kBlock);
    REQUIRE_EQ(stats.evicted_blocks_total, 1ull);
    REQUIRE_EQ(stats.uncached_reads_total, 0ull);
    REQUIRE_EQ(stats.hard_limit_rejections, 0ull);
    REQUIRE_EQ(stats.synchronous_pressure_passes_total, 1ull);
    REQUIRE_EQ(stats.synchronous_pressure_no_progress_total, 0ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_hard_pressure_evicts_clean_data_before_new_commit_write) {
    auto backing = myrd::test::FakeBackingStoreBuilder(41).build();
    auto store = require_store(backing_args(backing));
    const auto resident = block_data(std::byte{0x48});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, resident, 3));
    for (std::uint64_t block = 1; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentDirty,
            resident,
            3));
    }
    const auto replacement = block_data(std::byte{0xC5});

    REQUIRE(store->write(
        myrd::OperationContext{}, 4 * kBlock, replacement).ok());

    myrd::BlockEntry victim{};
    myrd::BlockEntry written{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, victim));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 4, written));
    REQUIRE_EQ(victim.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(written.state, myrd::BlockState::ResidentDirty);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 4 * kBlock);
    REQUIRE_EQ(stats.evicted_blocks_total, 1ull);
    REQUIRE_EQ(stats.hard_limit_rejections, 0ull);
    REQUIRE_EQ(stats.synchronous_pressure_passes_total, 1ull);
    REQUIRE_EQ(stats.synchronous_pressure_no_progress_total, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrent_commit_reservations_never_cross_exact_hard_limit) {
    constexpr std::uint32_t writer_count = 4;
    auto backing = myrd::test::FakeBackingStoreBuilder(42).build();
    auto memory_api = std::make_shared<GatedRealVirtualMemoryApi>();
    auto store = require_store(
        limited_backing_args(backing, writer_count, 1, 1, 2),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    const auto bytes = block_data(std::byte{0xD6});
    std::array<std::uint32_t, writer_count> codes{};
    std::array<std::thread, writer_count> writers;
    for (std::uint32_t index = 0; index < writer_count; ++index) {
        writers[index] = std::thread([&, index] {
            const auto result = store->write(
                myrd::OperationContext{}, index * kBlock, bytes);
            codes[index] = result.ok() ? 0u : result_code(result);
        });
    }

    const bool commit_entered = memory_api->wait_until_commit_entered(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    const bool exact_budget_reserved =
        wait_for_commit_reservation(*store, 2 * kBlock);
    const auto committed_while_blocked = store->stats().committed_bytes;
    const auto reserved_while_blocked =
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store);
    memory_api->release_commits();
    for (auto& writer : writers) {
        writer.join();
    }

    REQUIRE(commit_entered);
    REQUIRE(exact_budget_reserved);
    REQUIRE_EQ(committed_while_blocked, 0ull);
    REQUIRE_EQ(reserved_while_blocked, 2 * kBlock);
    std::uint32_t successes = 0;
    std::uint32_t hard_limit_failures = 0;
    for (const auto code : codes) {
        if (code == 0) {
            ++successes;
        } else if (code == static_cast<std::uint32_t>(
                               myrd::OperationErrorCode::memory_hard_limit)) {
            ++hard_limit_failures;
        }
    }
    REQUIRE_EQ(successes, 2u);
    REQUIRE_EQ(hard_limit_failures, 2u);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 2 * kBlock);
    REQUIRE_EQ(stats.hard_limit_rejections, 2ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_background_and_synchronous_eviction_share_one_victim_owner) {
    auto backing = myrd::test::FakeBackingStoreBuilder(44).build();
    auto memory_api = std::make_shared<GatedRealVirtualMemoryApi>(false);
    auto store = require_store(
        limited_backing_args(backing, 6, 1, 2, 4),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    const auto clean = block_data(std::byte{0x61});
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store,
            block,
            myrd::BlockState::ResidentClean,
            clean,
            3));
    }

    memory_api->block_next_decommit();
    const auto worker_passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(
        *store));
    const bool background_entered = memory_api->wait_until_decommit_entered(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    std::atomic<std::uint32_t> writer_code{0};
    const auto replacement = block_data(std::byte{0xF2});
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 4 * kBlock, replacement);
        writer_code.store(
            result.ok() ? 0u : result_code(result),
            std::memory_order_release);
    });
    const bool synchronous_waited =
        wait_for_synchronous_eviction_waiters(*store, 1);
    memory_api->release_decommit();
    writer.join();
    const bool worker_idle =
        myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
            *store,
            worker_passes_before + 1,
            std::chrono::steady_clock::now() + std::chrono::seconds(2));

    REQUIRE(background_entered);
    REQUIRE(synchronous_waited);
    REQUIRE(worker_idle);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(memory_api->decommit_calls(), 3ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.committed_bytes, 2 * kBlock);
    REQUIRE_EQ(stats.evicted_blocks_total, 3ull);
    REQUIRE_EQ(stats.synchronous_pressure_passes_total, 1ull);
    REQUIRE_EQ(stats.synchronous_pressure_waits_total, 1ull);
    REQUIRE_EQ(stats.synchronous_pressure_no_progress_total, 0ull);
    REQUIRE_EQ(stats.hard_limit_rejections, 0ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_pressure_scan_wait_observes_cancellation_and_deadline) {
    for (const bool cancel : {true, false}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(
                           cancel ? 45 : 46)
                           .build();
        auto memory_api =
            std::make_shared<GatedRealVirtualMemoryApi>(false);
        auto store = require_store(
            limited_backing_args(backing, 6, 1, 2, 4),
            myrd::DynamicBlockStoreTestDependencies{memory_api});
        const auto clean = block_data(std::byte{0x72});
        for (std::uint64_t block = 0; block < 4; ++block) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
                *store,
                block,
                myrd::BlockState::ResidentClean,
                clean,
                3));
        }

        memory_api->block_next_decommit();
        const auto worker_passes_before =
            myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
        REQUIRE(myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(
            *store));
        const bool background_entered =
            memory_api->wait_until_decommit_entered(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(2));
        std::stop_source stop;
        const auto deadline = cancel
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(50);
        std::atomic<std::uint32_t> writer_code{0};
        std::atomic<bool> writer_done{false};
        const auto replacement = block_data(std::byte{0x83});
        std::thread writer([&] {
            const auto result = store->write(
                myrd::OperationContext{stop.get_token(), deadline},
                4 * kBlock,
                replacement);
            writer_code.store(
                result.ok() ? 0u : result_code(result),
                std::memory_order_release);
            writer_done.store(true, std::memory_order_release);
        });
        const bool synchronous_waited =
            wait_for_synchronous_eviction_waiters(*store, 1);
        if (cancel) {
            stop.request_stop();
        }
        const bool interrupted_before_release = wait_for_true(writer_done);
        memory_api->release_decommit();
        writer.join();
        const bool worker_idle =
            myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
                *store,
                worker_passes_before + 1,
                std::chrono::steady_clock::now() + std::chrono::seconds(2));

        REQUIRE(background_entered);
        REQUIRE(synchronous_waited);
        REQUIRE(interrupted_before_release);
        REQUIRE(worker_idle);
        REQUIRE_EQ(
            writer_code.load(std::memory_order_acquire),
            static_cast<std::uint32_t>(
                cancel ? myrd::OperationErrorCode::cancelled
                       : myrd::OperationErrorCode::timed_out));
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 4, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
        REQUIRE_EQ(entry.version, 0ull);
        const auto stats = store->stats();
        REQUIRE_EQ(stats.hard_limit_rejections, 0ull);
        REQUIRE_EQ(stats.synchronous_pressure_passes_total, 1ull);
        REQUIRE_EQ(stats.synchronous_pressure_waits_total, 1ull);
        REQUIRE_EQ(stats.synchronous_pressure_cancellations_total,
                   cancel ? 1ull : 0ull);
        REQUIRE_EQ(stats.synchronous_pressure_deadlines_total,
                   cancel ? 0ull : 1ull);
        REQUIRE_EQ(
            myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(
                *store),
            0ull);
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_partial_zero_and_unmap_fail_before_logical_change_at_hard_limit) {
    const auto expected = block_data(std::byte{0xE7});
    for (const bool discard : {false, true}) {
        auto backing = myrd::test::FakeBackingStoreBuilder(43)
                           .data_block(4, expected)
                           .build();
        auto store = require_store(backing_args(backing));
        const auto resident = block_data(std::byte{0x59});
        for (std::uint64_t block = 0; block < 4; ++block) {
            REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
                *store,
                block,
                myrd::BlockState::ResidentDirty,
                resident,
                3));
        }
        const myrd::DataRange range{4 * kBlock + 512, 512};
        const auto result = discard
            ? store->unmap(myrd::OperationContext{},
                  std::span<const myrd::DataRange>(&range, 1))
            : store->zero(myrd::OperationContext{},
                  std::span<const myrd::DataRange>(&range, 1));

        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::OperationErrorCode::memory_hard_limit));
        myrd::BlockEntry entry{};
        REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 4, entry));
        REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
        REQUIRE_EQ(entry.version, 0ull);
        REQUIRE_EQ(backing->block_load_count(4), 1ull);
        REQUIRE_EQ(store->stats().committed_bytes, 4 * kBlock);
        REQUIRE_EQ(
            myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(
                *store),
            0ull);
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}
