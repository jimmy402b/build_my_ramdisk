#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
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
constexpr std::uint64_t kSector = 512;
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
    std::uint64_t logical_blocks = 128,
    myrd::BackingGeneration generation = 1) {
    return myrd::DynamicBlockStoreCreateArgs{
        logical_blocks * kBlock,
        static_cast<std::uint32_t>(kSector),
        myrd::DynamicBlockStoreOptions{
            3 * kBlock,
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

myrd::DynamicBlockStoreCreateArgs clean_zero_args(
    std::uint64_t logical_blocks = 4,
    myrd::BackingGeneration generation = 3) {
    auto args = backing_args({}, logical_blocks, generation);
    args.initial_state = myrd::InitialBlockState::CleanZero;
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

std::uint32_t result_code(const myrd::Result<void>& result) {
    return result.has_error()
        ? result.error().code
        : (std::numeric_limits<std::uint32_t>::max)();
}

bool wait_for_stripe_waiters(const myrd::DynamicBlockStore& store,
                             std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (store.stripe_waiter_count_for_test() != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return store.stripe_waiter_count_for_test() == expected;
}

bool wait_for_entry_state(myrd::DynamicBlockStore& store,
                          std::uint64_t block_index,
                          myrd::BlockState expected) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
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

bool wait_for_true(const std::atomic<bool>& value) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}

bool wait_for_count(const std::atomic<std::uint64_t>& value,
                    std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (value.load(std::memory_order_acquire) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire) == expected;
}

bool wait_for_synchronous_eviction_waiters(
    myrd::DynamicBlockStore& store,
    std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (myrd::DynamicBlockStoreTestAccess::synchronous_eviction_waiters(
               store) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return myrd::DynamicBlockStoreTestAccess::synchronous_eviction_waiters(
               store) == expected;
}

myrd::DynamicBlockStoreCreateArgs pressure_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t logical_blocks = 8) {
    auto args = backing_args(std::move(backing), logical_blocks, 3);
    args.options.eviction_low_watermark_bytes = kBlock;
    args.options.soft_limit_bytes = 2 * kBlock;
    args.options.hard_limit_bytes = 4 * kBlock;
    return args;
}

class GatedCountingVirtualMemoryApi final : public myrd::IVirtualMemoryApi {
public:
    void block_next_decommit() {
        std::scoped_lock lock(gate_mutex_);
        block_decommit_ = true;
        decommit_entered_ = false;
        decommit_released_ = false;
    }

    bool wait_until_decommit_entered() {
        std::unique_lock lock(gate_mutex_);
        return gate_condition_.wait_until(
            lock,
            std::chrono::steady_clock::now() + kWaitBound,
            [this] { return decommit_entered_; });
    }

    void release_decommit() noexcept {
        {
            std::scoped_lock lock(gate_mutex_);
            decommit_released_ = true;
        }
        gate_condition_.notify_all();
    }

    std::uint64_t decommit_calls() const noexcept {
        return decommit_calls_.load(std::memory_order_acquire);
    }

    std::uint64_t release_calls() const noexcept {
        return release_calls_.load(std::memory_order_acquire);
    }

    void* allocate(void* address,
                   std::size_t size_bytes,
                   std::uint32_t allocation_type,
                   std::uint32_t protection) noexcept override {
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
            decommit_calls_.fetch_add(1, std::memory_order_acq_rel);
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
        } else if (free_type == MEM_RELEASE) {
            release_calls_.fetch_add(1, std::memory_order_acq_rel);
        }
        const auto ok = ::VirtualFree(address, size_bytes, free_type) != FALSE;
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
    std::atomic<std::uint64_t> decommit_calls_{};
    std::atomic<std::uint64_t> release_calls_{};
    std::atomic<std::uint32_t> last_error_{ERROR_SUCCESS};
    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    bool block_decommit_{};
    bool decommit_entered_{};
    bool decommit_released_{};
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

    bool wait_until_entered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(
            lock,
            std::chrono::steady_clock::now() + kWaitBound,
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

void run_parallel_load_pair(std::uint64_t second_block) {
    auto first_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto second_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto first_data = block_data(0x27);
    const auto second_data = block_data(0xB4);
    auto backing = myrd::test::FakeBackingStoreBuilder(0x4411)
                       .data_block(0, first_data)
                       .data_block(second_block, second_data)
                       .gate_block(0, first_gate)
                       .gate_block(second_block, second_gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::array<std::vector<std::byte>, 2> outputs{
        std::vector<std::byte>(static_cast<std::size_t>(kBlock)),
        std::vector<std::byte>(static_cast<std::size_t>(kBlock)),
    };
    std::array<std::uint32_t, 2> codes{};
    std::barrier start(3);
    std::thread first([&] {
        start.arrive_and_wait();
        const auto result = store->read(
            myrd::OperationContext{}, 0, outputs[0]);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    std::thread second([&] {
        start.arrive_and_wait();
        const auto result = store->read(
            myrd::OperationContext{}, second_block * kBlock, outputs[1]);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    start.arrive_and_wait();

    const bool first_entered = first_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    const bool second_entered = second_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    first_gate->open();
    second_gate->open();
    first.join();
    second.join();

    REQUIRE(first_entered);
    REQUIRE(second_entered);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE(std::equal(
        outputs[0].begin(), outputs[0].end(), first_data.begin()));
    REQUIRE(std::equal(
        outputs[1].begin(), outputs[1].end(), second_data.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    REQUIRE_EQ(backing->block_load_count(second_block), 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

enum class MutationKind : std::uint8_t {
    FullWrite,
    PartialWrite,
    FullZero,
    FullUnmap,
};

void run_read_mutation_serialization(MutationKind kind) {
    const auto old_data = block_data(0x18);
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(clean_zero_args(), dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, old_data, 3));

    auto expected_new = old_data;
    const auto full_replacement = block_data(0xE7);
    std::vector<std::byte> patch(
        static_cast<std::size_t>(kSector), std::byte{0xE7});
    if (kind == MutationKind::FullWrite) {
        expected_new = full_replacement;
    } else if (kind == MutationKind::PartialWrite) {
        std::copy(patch.begin(), patch.end(),
                  expected_new.begin() + static_cast<std::ptrdiff_t>(kSector));
    } else {
        expected_new.fill(std::byte{0});
    }

    std::vector<std::byte> old_output(static_cast<std::size_t>(kBlock));
    std::vector<std::byte> new_output(static_cast<std::size_t>(kBlock));
    std::array<std::uint32_t, 3> codes{};
    std::thread old_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, old_output);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool old_reader_pinned = observer->wait_until_entered();

    std::thread mutation([&] {
        myrd::Result<void> result = myrd::Result<void>::failure({
            myrd::ErrorDomain::storage,
            static_cast<std::uint32_t>(
                myrd::OperationErrorCode::internal_invariant),
            L"uninitialized concurrency mutation",
            L"test mutation branch was not selected",
        });
        if (kind == MutationKind::FullWrite) {
            result = store->write(
                myrd::OperationContext{}, 0, full_replacement);
        } else if (kind == MutationKind::PartialWrite) {
            result = store->write(
                myrd::OperationContext{}, kSector, patch);
        } else {
            const myrd::DataRange range{0, kBlock};
            const auto ranges = std::span<const myrd::DataRange>(&range, 1);
            result = kind == MutationKind::FullZero
                ? store->zero(myrd::OperationContext{}, ranges)
                : store->unmap(myrd::OperationContext{}, ranges);
        }
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    const bool mutation_waiting = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);

    std::thread new_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, new_output);
        codes[2] = result.ok() ? 0 : result_code(result);
    });
    const bool both_waiting = wait_for_stripe_waiters(*store, 2);
    observer->release();
    old_reader.join();
    mutation.join();
    new_reader.join();

    REQUIRE(old_reader_pinned);
    REQUIRE(mutation_waiting);
    REQUIRE(both_waiting);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE_EQ(codes[2], 0u);
    REQUIRE(std::equal(
        old_output.begin(), old_output.end(), old_data.begin()));
    REQUIRE(std::equal(
        new_output.begin(), new_output.end(), expected_new.begin()));

    std::vector<std::byte> final_output(static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, final_output).ok());
    REQUIRE(std::equal(
        final_output.begin(), final_output.end(), expected_new.begin()));
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.version, 1ull);
    REQUIRE_EQ(entry.state,
               kind == MutationKind::FullWrite ||
                       kind == MutationKind::PartialWrite
                   ? myrd::BlockState::ResidentDirty
                   : myrd::BlockState::ZeroDirty);
    REQUIRE(store->validate_invariants_for_test().ok());
}

myrd::BlockEntry entry_at(myrd::DynamicBlockStore& store,
                          std::uint64_t block_index) {
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        store, block_index, entry));
    return entry;
}

void run_loading_wait_interruption(bool cancellation) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(0xB101)
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing, 4, 3));
    std::vector<std::byte> owner_output(
        static_cast<std::size_t>(kBlock), std::byte{0x11});
    std::vector<std::byte> waiter_output(
        static_cast<std::size_t>(kBlock), std::byte{0xA5});
    std::atomic<std::uint32_t> owner_code{};
    std::atomic<std::uint32_t> waiter_code{};
    std::atomic<bool> waiter_done{};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, owner_output);
        owner_code.store(result.ok() ? 0 : result_code(result),
                         std::memory_order_release);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    std::stop_source stop;
    std::thread waiter([&] {
        const auto deadline = cancellation
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(100);
        const auto result = store->read(
            myrd::OperationContext{stop.get_token(), deadline},
            0,
            waiter_output);
        waiter_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
        waiter_done.store(true, std::memory_order_release);
    });
    const bool waiter_registered = wait_for_stripe_waiters(*store, 1);
    if (cancellation) {
        stop.request_stop();
    }
    const bool waiter_interrupted = wait_for_true(waiter_done);
    gate->open();
    owner.join();
    waiter.join();

    REQUIRE(owner_entered);
    REQUIRE(waiter_registered);
    REQUIRE(waiter_interrupted);
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(waiter_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   cancellation ? myrd::OperationErrorCode::cancelled
                                : myrd::OperationErrorCode::timed_out));
    REQUIRE(std::all_of(
        waiter_output.begin(), waiter_output.end(),
        [](std::byte value) { return value == std::byte{0xA5}; }));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

void run_pin_wait_interruption(bool cancellation) {
    const auto old_data = block_data(0xC1);
    const auto replacement = block_data(0xD1);
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(clean_zero_args(4, 3), dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentDirty, old_data, 3));
    std::vector<std::byte> old_output(static_cast<std::size_t>(kBlock));
    std::atomic<std::uint32_t> reader_code{};
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, old_output);
        reader_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool pin_entered = observer->wait_until_entered();
    std::stop_source stop;
    std::atomic<std::uint32_t> writer_code{};
    std::atomic<bool> writer_done{};
    std::thread writer([&] {
        const auto deadline = cancellation
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(100);
        const auto result = store->write(
            myrd::OperationContext{stop.get_token(), deadline},
            0,
            replacement);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
        writer_done.store(true, std::memory_order_release);
    });
    const bool committing = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    const bool writer_waiting = wait_for_stripe_waiters(*store, 1);
    if (cancellation) {
        stop.request_stop();
    }
    const bool writer_interrupted = wait_for_true(writer_done);
    observer->release();
    reader.join();
    writer.join();

    REQUIRE(pin_entered);
    REQUIRE(committing);
    REQUIRE(writer_waiting);
    REQUIRE(writer_interrupted);
    REQUIRE_EQ(reader_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   cancellation ? myrd::OperationErrorCode::cancelled
                                : myrd::OperationErrorCode::timed_out));
    REQUIRE(std::equal(old_output.begin(), old_output.end(), old_data.begin()));
    const auto entry = entry_at(*store, 0);
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 0ull);
    std::vector<std::byte> final_output(static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, final_output).ok());
    REQUIRE(std::equal(final_output.begin(), final_output.end(), old_data.begin()));
    REQUIRE(store->validate_invariants_for_test().ok());
}

void run_pressure_wait_interruption(bool cancellation) {
    auto backing = myrd::test::FakeBackingStoreBuilder(0xD101).build();
    auto memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    auto store = require_store(
        pressure_args(backing, 6),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    const auto clean = block_data(0xE1);
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentClean, clean, 3));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
            *store, block, myrd::BlockState::ResidentClean, 0, false, 3));
    }
    memory_api->block_next_decommit();
    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    const bool signalled =
        myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(*store);
    const bool background_entered = memory_api->wait_until_decommit_entered();
    std::stop_source stop;
    std::atomic<std::uint32_t> writer_code{};
    std::atomic<bool> writer_done{};
    std::thread writer([&] {
        const auto deadline = cancellation
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(100);
        const auto result = store->write(
            myrd::OperationContext{stop.get_token(), deadline},
            4 * kBlock,
            clean);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
        writer_done.store(true, std::memory_order_release);
    });
    const bool pressure_waited =
        wait_for_synchronous_eviction_waiters(*store, 1);
    if (cancellation) {
        stop.request_stop();
    }
    const bool writer_interrupted = wait_for_true(writer_done);
    memory_api->release_decommit();
    writer.join();
    const bool worker_idle = myrd::DynamicBlockStoreTestAccess::
        wait_for_eviction_worker_idle(
            *store,
            passes_before + 1,
            std::chrono::steady_clock::now() + kWaitBound);

    REQUIRE(signalled);
    REQUIRE(background_entered);
    REQUIRE(pressure_waited);
    REQUIRE(writer_interrupted);
    REQUIRE(worker_idle);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   cancellation ? myrd::OperationErrorCode::cancelled
                                : myrd::OperationErrorCode::timed_out));
    REQUIRE_EQ(entry_at(*store, 4).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

} // namespace

TEST_CASE(m2_concurrency_same_block_is_single_flight_for_2_8_and_32_readers) {
    for (const auto reader_count : {2u, 8u, 32u}) {
        auto gate =
            std::make_shared<myrd::test::FakeBackingStoreGate>();
        const auto expected = block_data(
            static_cast<std::uint8_t>(0x40u + reader_count));
        auto backing = myrd::test::FakeBackingStoreBuilder(0x4040)
                           .data_block(0, expected)
                           .gate_block(0, gate)
                           .build();
        auto store = require_store(backing_args(backing, 4, 1));
        std::vector<std::vector<std::byte>> outputs(
            reader_count,
            std::vector<std::byte>(static_cast<std::size_t>(kBlock)));
        std::vector<std::uint32_t> codes(reader_count);
        std::vector<std::thread> readers;
        readers.reserve(reader_count);
        std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
        for (std::uint32_t index = 0; index < reader_count; ++index) {
            readers.emplace_back([&, index] {
                start.arrive_and_wait();
                const auto result = store->read(
                    myrd::OperationContext{}, 0, outputs[index]);
                codes[index] = result.ok() ? 0 : result_code(result);
            });
        }
        start.arrive_and_wait();
        const bool owner_entered = gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + kWaitBound);
        const bool waiters_registered =
            wait_for_stripe_waiters(*store, reader_count - 1);
        gate->open();
        for (auto& reader : readers) {
            reader.join();
        }

        REQUIRE(owner_entered);
        REQUIRE(waiters_registered);
        for (std::uint32_t index = 0; index < reader_count; ++index) {
            REQUIRE_EQ(codes[index], 0u);
            REQUIRE(std::equal(
                outputs[index].begin(), outputs[index].end(),
                expected.begin()));
        }
        REQUIRE_EQ(backing->block_load_count(0), 1ull);
        REQUIRE(store->validate_invariants_for_test().ok());
    }
}

TEST_CASE(m2_concurrency_cancelled_waiter_does_not_poison_owner_or_peer) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto expected = block_data(0x71);
    auto backing = myrd::test::FakeBackingStoreBuilder(0x5050)
                       .data_block(0, expected)
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing, 4, 1));
    std::array<std::vector<std::byte>, 3> outputs{
        std::vector<std::byte>(static_cast<std::size_t>(kBlock)),
        std::vector<std::byte>(
            static_cast<std::size_t>(kBlock), std::byte{0xA5}),
        std::vector<std::byte>(static_cast<std::size_t>(kBlock)),
    };
    std::array<std::uint32_t, 3> codes{};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, outputs[0]);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);

    std::stop_source cancelled;
    std::thread cancelled_waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{cancelled.get_token()}, 0, outputs[1]);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    std::thread healthy_waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, outputs[2]);
        codes[2] = result.ok() ? 0 : result_code(result);
    });
    const bool waiters_registered = wait_for_stripe_waiters(*store, 2);
    cancelled.request_stop();
    cancelled_waiter.join();
    gate->open();
    owner.join();
    healthy_waiter.join();

    REQUIRE(owner_entered);
    REQUIRE(waiters_registered);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], static_cast<std::uint32_t>(
                             myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(codes[2], 0u);
    REQUIRE(std::equal(
        outputs[0].begin(), outputs[0].end(), expected.begin()));
    REQUIRE(std::all_of(
        outputs[1].begin(), outputs[1].end(),
        [](std::byte value) { return value == std::byte{0xA5}; }));
    REQUIRE(std::equal(
        outputs[2].begin(), outputs[2].end(), expected.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_close_interrupts_blocked_load_within_bound) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(0x6060)
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing, 4, 1));
    std::vector<std::byte> output(
        static_cast<std::size_t>(kBlock), std::byte{0xB6});
    std::uint32_t code{};
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, output);
        code = result.ok() ? 0 : result_code(result);
    });
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    const auto close_started = std::chrono::steady_clock::now();
    store->close();
    const auto close_elapsed =
        std::chrono::steady_clock::now() - close_started;
    reader.join();

    REQUIRE(owner_entered);
    REQUIRE(close_elapsed < kWaitBound);
    REQUIRE_EQ(code, static_cast<std::uint32_t>(
                         myrd::OperationErrorCode::session_closed));
    REQUIRE(std::all_of(
        output.begin(), output.end(),
        [](std::byte value) { return value == std::byte{0xB6}; }));
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(store->stats().loading_blocks, 0ull);
}

TEST_CASE(m2_concurrency_old_generation_retries_once_for_32_waiters) {
    constexpr std::size_t reader_count = 32;
    auto old_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto lifetime =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    const auto old_data = block_data(0x31);
    const auto new_data = block_data(0xD8);
    auto old_backing = myrd::test::FakeBackingStoreBuilder(0x1010)
                           .data_block(0, old_data)
                           .gate_block(0, old_gate)
                           .lifetime_tracker(lifetime)
                           .build();
    std::weak_ptr<const myrd::test::FakeBackingStore> old_weak = old_backing;
    auto successor = myrd::test::FakeBackingStoreBuilder(0x2020)
                         .data_block(0, new_data)
                         .build();
    auto store = require_store(backing_args(old_backing, 4, 1));
    old_backing.reset();

    std::array<std::vector<std::byte>, reader_count> outputs;
    std::array<std::uint32_t, reader_count> codes{};
    std::array<std::thread, reader_count> readers;
    std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
    for (std::size_t index = 0; index < reader_count; ++index) {
        outputs[index].resize(static_cast<std::size_t>(kBlock));
        readers[index] = std::thread([&, index] {
            start.arrive_and_wait();
            const auto result = store->read(
                myrd::OperationContext{}, 0, outputs[index]);
            codes[index] = result.ok() ? 0 : result_code(result);
        });
    }
    start.arrive_and_wait();
    const bool old_entered = old_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    const bool waiters_registered =
        wait_for_stripe_waiters(*store, reader_count - 1);
    auto old_live = old_weak.lock();
    const bool old_loaded_once =
        old_live && old_live->block_load_count(0) == 1;
    old_live.reset();
    const bool installed = store->install_backing_view({2, successor}).ok();
    const bool old_retained_during_load = !old_weak.expired();
    old_gate->open();
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(old_entered);
    REQUIRE(waiters_registered);
    REQUIRE(old_loaded_once);
    REQUIRE(installed);
    REQUIRE(old_retained_during_load);
    REQUIRE(old_weak.expired());
    for (std::size_t index = 0; index < reader_count; ++index) {
        REQUIRE_EQ(codes[index], 0u);
        REQUIRE(std::equal(
            outputs[index].begin(), outputs[index].end(), new_data.begin()));
    }
    REQUIRE_EQ(successor->block_load_count(0), 1ull);
    REQUIRE_EQ(store->stats().stale_backing_load_retries, 1ull);
    REQUIRE_EQ(store->stats().backing_reads_total, 2ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry.clean_generation, 2ull);
    REQUIRE_EQ(entry.load_waiter_count, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_owner_failure_wakes_all_and_retry_uses_new_epoch) {
    constexpr std::size_t reader_count = 8;
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto expected = block_data(0x6C);
    auto backing = myrd::test::FakeBackingStoreBuilder(0x3030)
                       .data_block(0, expected)
                       .gate_block(0, gate)
                       .fail_block_on_load(0, 1)
                       .build();
    auto store = require_store(backing_args(backing, 4, 1));
    std::array<std::vector<std::byte>, reader_count> outputs;
    std::array<std::uint32_t, reader_count> codes{};
    std::array<std::thread, reader_count> readers;
    std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
    for (std::size_t index = 0; index < reader_count; ++index) {
        outputs[index].assign(
            static_cast<std::size_t>(kBlock), std::byte{0xA5});
        readers[index] = std::thread([&, index] {
            start.arrive_and_wait();
            const auto result = store->read(
                myrd::OperationContext{}, 0, outputs[index]);
            codes[index] = result.ok() ? 0 : result_code(result);
        });
    }
    start.arrive_and_wait();
    const bool owner_entered = gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    const bool waiters_registered =
        wait_for_stripe_waiters(*store, reader_count - 1);
    gate->open();
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(owner_entered);
    REQUIRE(waiters_registered);
    for (std::size_t index = 0; index < reader_count; ++index) {
        REQUIRE_EQ(codes[index], static_cast<std::uint32_t>(
                                     myrd::OperationErrorCode::
                                         backing_read_failed));
        REQUIRE(std::all_of(
            outputs[index].begin(), outputs[index].end(),
            [](std::byte value) { return value == std::byte{0xA5}; }));
    }
    REQUIRE_EQ(backing->block_load_count(0), 1ull);
    myrd::BlockEntry failed{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, failed));
    REQUIRE_EQ(failed.state, myrd::BlockState::Unresident);
    REQUIRE(failed.completed_load_epoch != 0);

    std::vector<std::byte> retry_output(static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, retry_output).ok());
    REQUIRE(std::equal(
        retry_output.begin(), retry_output.end(), expected.begin()));
    REQUIRE_EQ(backing->block_load_count(0), 2ull);
    myrd::BlockEntry retried{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, retried));
    REQUIRE_EQ(retried.state, myrd::BlockState::ResidentClean);
    REQUIRE(retried.load_epoch != failed.completed_load_epoch);
    REQUIRE_EQ(retried.completed_load_epoch, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_same_and_different_stripes_load_in_parallel) {
    run_parallel_load_pair(64);
    run_parallel_load_pair(1);
}

TEST_CASE(m2_concurrency_read_vs_full_and_partial_write_is_not_torn) {
    run_read_mutation_serialization(MutationKind::FullWrite);
    run_read_mutation_serialization(MutationKind::PartialWrite);
}

TEST_CASE(m2_concurrency_full_zero_and_unmap_do_not_resurrect_old_data) {
    run_read_mutation_serialization(MutationKind::FullZero);
    run_read_mutation_serialization(MutationKind::FullUnmap);
}

TEST_CASE(m2_concurrency_two_full_writes_publish_one_complete_final_version) {
    const auto old_data = block_data(0x11);
    const auto first_data = block_data(0x55);
    const auto second_data = block_data(0xC0);
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto store = require_store(clean_zero_args(), dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, old_data, 3));

    std::vector<std::byte> old_output(static_cast<std::size_t>(kBlock));
    std::array<std::uint32_t, 3> codes{};
    std::thread old_reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, old_output);
        codes[0] = result.ok() ? 0 : result_code(result);
    });
    const bool old_reader_pinned = observer->wait_until_entered();

    std::barrier writer_start(3);
    std::thread first_writer([&] {
        writer_start.arrive_and_wait();
        const auto result = store->write(
            myrd::OperationContext{}, 0, first_data);
        codes[1] = result.ok() ? 0 : result_code(result);
    });
    std::thread second_writer([&] {
        writer_start.arrive_and_wait();
        const auto result = store->write(
            myrd::OperationContext{}, 0, second_data);
        codes[2] = result.ok() ? 0 : result_code(result);
    });
    writer_start.arrive_and_wait();
    const bool one_writer_committing = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    const bool both_writers_waiting = wait_for_stripe_waiters(*store, 2);
    observer->release();
    old_reader.join();
    first_writer.join();
    second_writer.join();

    REQUIRE(old_reader_pinned);
    REQUIRE(one_writer_committing);
    REQUIRE(both_writers_waiting);
    REQUIRE_EQ(codes[0], 0u);
    REQUIRE_EQ(codes[1], 0u);
    REQUIRE_EQ(codes[2], 0u);
    REQUIRE(std::equal(
        old_output.begin(), old_output.end(), old_data.begin()));
    std::vector<std::byte> final_output(static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, final_output).ok());
    const bool final_is_first = std::equal(
        final_output.begin(), final_output.end(), first_data.begin());
    const bool final_is_second = std::equal(
        final_output.begin(), final_output.end(), second_data.begin());
    REQUIRE(final_is_first || final_is_second);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry.version, 2ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_snapshot_copy_and_write_preserve_version_byte_pairs) {
    const auto old_data = block_data(0x23);
    const auto new_data = block_data(0xD3);
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_after_snapshot_pin = true;
    auto store = require_store(clean_zero_args(4, 3), dependencies);
    REQUIRE(store->write(myrd::OperationContext{}, 0, old_data).ok());

    myrd::DirtyScanCursor cursor{};
    std::vector<myrd::DirtyBlockSnapshot> snapshot_records;
    std::atomic<std::uint32_t> snapshot_code{};
    std::thread snapshot([&] {
        auto result = store->copy_dirty_batch(
            myrd::OperationContext{}, cursor, kBlock);
        if (result.ok()) {
            snapshot_records = result.move_value();
            snapshot_code.store(0, std::memory_order_release);
        } else {
            snapshot_code.store(result.error().code,
                                std::memory_order_release);
        }
    });
    const auto pin_deadline =
        std::chrono::steady_clock::now() + kWaitBound;
    while (!myrd::DynamicBlockStoreTestAccess::snapshot_pin_gap_reached(
               *store) &&
           std::chrono::steady_clock::now() < pin_deadline) {
        std::this_thread::yield();
    }
    const bool pin_reached =
        myrd::DynamicBlockStoreTestAccess::snapshot_pin_gap_reached(*store);

    std::atomic<std::uint32_t> writer_code{};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, new_data);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool writer_waiting = wait_for_entry_state(
        *store, 0, myrd::BlockState::Committing);
    const bool pin_released =
        myrd::DynamicBlockStoreTestAccess::release_snapshot_pin_gap(*store);
    snapshot.join();
    writer.join();

    REQUIRE(pin_reached);
    REQUIRE(writer_waiting);
    REQUIRE(pin_released);
    REQUIRE_EQ(snapshot_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(snapshot_records.size(), 1u);
    REQUIRE_EQ(snapshot_records[0].version, 1ull);
    REQUIRE(std::equal(
        snapshot_records[0].data.begin(),
        snapshot_records[0].data.end(),
        old_data.begin()));

    myrd::DirtyScanCursor new_cursor{};
    const auto later = store->copy_dirty_batch(
        myrd::OperationContext{}, new_cursor, kBlock);
    REQUIRE(later.ok());
    REQUIRE_EQ(later.value().size(), 1u);
    REQUIRE_EQ(later.value()[0].version, 2ull);
    REQUIRE(std::equal(
        later.value()[0].data.begin(),
        later.value()[0].data.end(),
        new_data.begin()));
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentDirty);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_generation_bound_ack_never_cleans_a_newer_write) {
    const auto old_data = block_data(0x34);
    const auto new_data = block_data(0xE4);
    auto initial = myrd::test::FakeBackingStoreBuilder(0x7101)
                       .data_block(0, old_data)
                       .build();
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(*initial)
                         .data_block(0, old_data)
                         .build();
    myrd::DynamicBlockStoreTestDependencies ack_first_dependencies{};
    ack_first_dependencies.pause_after_persisted_validation = true;
    auto store = require_store(
        backing_args(initial, 4, 3), ack_first_dependencies);
    REQUIRE(store->write(myrd::OperationContext{}, 0, old_data).ok());

    myrd::DirtyScanCursor cursor{};
    const auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    const auto record = snapshot.value()[0];

    const auto before_install = store->mark_persisted(
        0, record.version, record.kind, 4);
    REQUIRE(!before_install.ok());
    REQUIRE_EQ(before_install.error().code,
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       BackingGenerationConflict));
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentDirty);
    REQUIRE(store->install_backing_view({4, successor}).ok());

    // Ack-first race: the test-only gate pauses after all exact validation,
    // publishes a unique Committing owner, and releases the stripe so the
    // writer can become a real waiter before the ack is allowed to publish.
    std::atomic<std::uint32_t> ack_first_code{};
    std::thread ack_first([&] {
        const auto result = store->mark_persisted(
            0, record.version, record.kind, 4);
        ack_first_code.store(result.ok() ? 0 : result.error().code,
                             std::memory_order_release);
    });
    const auto ack_gap_deadline =
        std::chrono::steady_clock::now() + kWaitBound;
    while (!myrd::DynamicBlockStoreTestAccess::persisted_ack_gap_reached(
               *store) &&
           std::chrono::steady_clock::now() < ack_gap_deadline) {
        std::this_thread::yield();
    }
    const bool ack_validation_reached =
        myrd::DynamicBlockStoreTestAccess::persisted_ack_gap_reached(*store);
    myrd::BlockEntry ack_transition{};
    const bool ack_transition_captured =
        myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, ack_transition);
    const auto ack_transition_stats = store->stats();
    std::atomic<std::uint32_t> ack_first_writer_code{};
    std::thread ack_first_writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 0, new_data);
        ack_first_writer_code.store(
            result.ok() ? 0 : result_code(result),
            std::memory_order_release);
    });
    const bool ack_first_writer_waiting =
        wait_for_stripe_waiters(*store, 1);
    const bool ack_gate_released =
        myrd::DynamicBlockStoreTestAccess::release_persisted_ack_gap(*store);
    ack_first.join();
    ack_first_writer.join();

    REQUIRE(ack_validation_reached);
    REQUIRE(ack_transition_captured);
    REQUIRE_EQ(ack_transition.state, myrd::BlockState::Committing);
    REQUIRE_EQ(ack_transition.prior_state,
               myrd::BlockState::ResidentDirty);
    REQUIRE(ack_transition.transition_owner != 0);
    REQUIRE_EQ(ack_transition.transition_owner,
               ack_transition.transition_epoch);
    REQUIRE_EQ(ack_transition_stats.resident_dirty_blocks, 0ull);
    REQUIRE(ack_first_writer_waiting);
    REQUIRE(ack_gate_released);
    REQUIRE_EQ(ack_first_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(ack_first_writer_code.load(std::memory_order_acquire), 0u);
    const auto ack_first_final = entry_at(*store, 0);
    REQUIRE_EQ(ack_first_final.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(ack_first_final.version, 2ull);
    REQUIRE_EQ(ack_first_final.dirty_base_generation, 4ull);
    REQUIRE_EQ(store->stats().resident_dirty_blocks, 1ull);
    std::vector<std::byte> ack_first_output(
        static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(
        myrd::OperationContext{}, 0, ack_first_output).ok());
    REQUIRE(std::equal(
        ack_first_output.begin(), ack_first_output.end(), new_data.begin()));
    const auto stale_after_ack_first = store->mark_persisted(
        0, record.version, record.kind, 4);
    REQUIRE(!stale_after_ack_first.ok());
    REQUIRE_EQ(stale_after_ack_first.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::version_changed));

    // Writer-first order: pin v1, force the writer into Committing, then queue
    // the old ack behind that exact owner. The old ack has one legal result.
    auto observer = std::make_shared<BlockingPinObserver>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.read_observer = observer;
    auto race_store = require_store(
        backing_args(initial, 4, 3), dependencies);
    REQUIRE(race_store->write(
        myrd::OperationContext{}, 0, old_data).ok());
    myrd::DirtyScanCursor race_cursor{};
    const auto race_snapshot = race_store->copy_dirty_batch(
        myrd::OperationContext{}, race_cursor, kBlock);
    REQUIRE(race_snapshot.ok());
    REQUIRE_EQ(race_snapshot.value().size(), 1u);
    const auto race_record = race_snapshot.value()[0];
    REQUIRE(race_store->install_backing_view({4, successor}).ok());

    std::vector<std::byte> old_output(static_cast<std::size_t>(kBlock));
    std::atomic<std::uint32_t> reader_code{};
    std::thread reader([&] {
        const auto result = race_store->read(
            myrd::OperationContext{}, 0, old_output);
        reader_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool pin_entered = observer->wait_until_entered();
    std::atomic<std::uint32_t> writer_code{};
    std::thread writer([&] {
        const auto result = race_store->write(
            myrd::OperationContext{}, 0, new_data);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool writer_committing = wait_for_entry_state(
        *race_store, 0, myrd::BlockState::Committing);
    std::atomic<std::uint32_t> ack_code{};
    std::thread acknowledger([&] {
        const auto result = race_store->mark_persisted(
            0, race_record.version, race_record.kind, 4);
        ack_code.store(result.ok() ? 0 : result.error().code,
                       std::memory_order_release);
    });
    const bool writer_and_ack_waiting =
        wait_for_stripe_waiters(*race_store, 2);
    observer->release();
    reader.join();
    writer.join();
    acknowledger.join();

    REQUIRE(pin_entered);
    REQUIRE(writer_committing);
    REQUIRE(writer_and_ack_waiting);
    REQUIRE_EQ(reader_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(ack_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::version_changed));
    REQUIRE(std::equal(old_output.begin(), old_output.end(), old_data.begin()));
    const auto writer_first_final = entry_at(*race_store, 0);
    REQUIRE_EQ(writer_first_final.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(writer_first_final.version, 2ull);
    REQUIRE_EQ(writer_first_final.dirty_base_generation, 4ull);

    // Persist the actual v2 into a complete generation-5 view, then evict and
    // reload it. This closes the resurrection loop after the rejected old ack.
    myrd::DirtyScanCursor latest_cursor{};
    const auto latest_snapshot = race_store->copy_dirty_batch(
        myrd::OperationContext{}, latest_cursor, kBlock);
    REQUIRE(latest_snapshot.ok());
    REQUIRE_EQ(latest_snapshot.value().size(), 1u);
    REQUIRE_EQ(latest_snapshot.value()[0].version, 2ull);
    auto latest_view =
        myrd::test::FakeBackingStoreBuilder::successor_of(*successor)
            .data_block(0, new_data)
            .build();
    REQUIRE(race_store->install_backing_view({5, latest_view}).ok());
    REQUIRE(race_store->mark_persisted(
        0,
        latest_snapshot.value()[0].version,
        latest_snapshot.value()[0].kind,
        5).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *race_store, 0, myrd::BlockState::ResidentClean, 0, false, 5));
    const auto eviction = myrd::DynamicBlockStoreTestAccess::
        run_eviction_sweep(*race_store, 0);
    REQUIRE_EQ(eviction.evicted_blocks, 1ull);
    std::vector<std::byte> output(static_cast<std::size_t>(kBlock));
    REQUIRE(race_store->read(
        myrd::OperationContext{}, 0, output).ok());
    REQUIRE(std::equal(output.begin(), output.end(), new_data.begin()));
    REQUIRE_EQ(latest_view->block_load_count(0), 1ull);
    REQUIRE_EQ(entry_at(*race_store, 0).clean_generation, 5ull);
    REQUIRE(race_store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_close_releases_ack_gate_and_restores_dirty_state) {
    const auto data = block_data(0x3A);
    auto initial = myrd::test::FakeBackingStoreBuilder(0x7181).build();
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(*initial)
                         .data_block(0, data)
                         .build();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_after_persisted_validation = true;
    auto store = require_store(
        backing_args(initial, 4, 3), dependencies);
    REQUIRE(store->write(myrd::OperationContext{}, 0, data).ok());
    myrd::DirtyScanCursor cursor{};
    const auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    REQUIRE(store->install_backing_view({4, successor}).ok());
    const auto before_entry = entry_at(*store, 0);
    const auto before_stats = store->stats();

    std::atomic<std::uint32_t> ack_code{};
    std::atomic<std::uint64_t> completed{};
    std::thread acknowledger([&] {
        const auto result = store->mark_persisted(
            0,
            snapshot.value()[0].version,
            snapshot.value()[0].kind,
            4);
        ack_code.store(result.ok() ? 0 : result.error().code,
                       std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    const auto gap_deadline =
        std::chrono::steady_clock::now() + kWaitBound;
    while (!myrd::DynamicBlockStoreTestAccess::persisted_ack_gap_reached(
               *store) &&
           std::chrono::steady_clock::now() < gap_deadline) {
        std::this_thread::yield();
    }
    const bool ack_paused =
        myrd::DynamicBlockStoreTestAccess::persisted_ack_gap_reached(*store);
    myrd::BlockEntry paused_entry{};
    const bool paused_entry_captured =
        myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, paused_entry);
    const auto paused_stats = store->stats();

    std::thread closer([&] {
        store->close();
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    const bool close_and_ack_bounded = wait_for_count(completed, 2);
    if (!close_and_ack_bounded) {
        myrd::DynamicBlockStoreTestAccess::release_persisted_ack_gap(*store);
    }
    acknowledger.join();
    closer.join();

    REQUIRE(ack_paused);
    REQUIRE(paused_entry_captured);
    REQUIRE_EQ(paused_entry.state, myrd::BlockState::Committing);
    REQUIRE_EQ(paused_entry.prior_state, myrd::BlockState::ResidentDirty);
    REQUIRE(paused_entry.transition_owner != 0);
    REQUIRE_EQ(paused_entry.transition_owner, paused_entry.transition_epoch);
    REQUIRE_EQ(paused_stats.resident_dirty_blocks, 0ull);
    REQUIRE(close_and_ack_bounded);
    REQUIRE_EQ(ack_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE(store->is_closed());
    const auto restored = entry_at(*store, 0);
    REQUIRE_EQ(restored.state, before_entry.state);
    REQUIRE_EQ(restored.prior_state, before_entry.prior_state);
    REQUIRE_EQ(restored.version, before_entry.version);
    REQUIRE_EQ(restored.dirty_kind, before_entry.dirty_kind);
    REQUIRE_EQ(restored.dirty_base_generation,
               before_entry.dirty_base_generation);
    REQUIRE_EQ(restored.transition_owner, 0ull);
    REQUIRE_EQ(restored.transition_epoch, 0ull);
    REQUIRE(store->stats() == before_stats);
}

TEST_CASE(m2_concurrency_ack_evict_reload_never_exposes_predecessor_bytes) {
    const auto predecessor = block_data(0x45);
    const auto current = block_data(0xF5);
    auto initial = myrd::test::FakeBackingStoreBuilder(0x8101)
                       .data_block(0, predecessor)
                       .build();
    auto successor = myrd::test::FakeBackingStoreBuilder::successor_of(*initial)
                         .data_block(0, current)
                         .build();
    auto memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    auto store = require_store(
        backing_args(initial, 4, 3),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    REQUIRE(store->write(myrd::OperationContext{}, 0, current).ok());
    myrd::DirtyScanCursor cursor{};
    const auto snapshot = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, kBlock);
    REQUIRE(snapshot.ok());
    REQUIRE_EQ(snapshot.value().size(), 1u);
    REQUIRE(store->install_backing_view({4, successor}).ok());
    REQUIRE(store->mark_persisted(
        0,
        snapshot.value()[0].version,
        snapshot.value()[0].kind,
        4).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 0, myrd::BlockState::ResidentClean, 0, false, 4));

    memory_api->block_next_decommit();
    myrd::EvictionPassResult eviction{};
    std::thread evictor([&] {
        eviction = myrd::DynamicBlockStoreTestAccess::
            run_eviction_sweep(*store, 0);
    });
    const bool decommit_entered = memory_api->wait_until_decommit_entered();
    std::vector<std::byte> output(static_cast<std::size_t>(kBlock));
    std::atomic<std::uint32_t> read_code{};
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, output);
        read_code.store(result.ok() ? 0 : result_code(result),
                        std::memory_order_release);
    });
    const bool reader_waiting = wait_for_stripe_waiters(*store, 1);
    memory_api->release_decommit();
    evictor.join();
    reader.join();

    REQUIRE(decommit_entered);
    REQUIRE(reader_waiting);
    REQUIRE_EQ(eviction.evicted_blocks, 1ull);
    REQUIRE_EQ(read_code.load(std::memory_order_acquire), 0u);
    REQUIRE(std::equal(output.begin(), output.end(), current.begin()));
    REQUIRE_EQ(initial->block_load_count(0), 0ull);
    REQUIRE_EQ(successor->block_load_count(0), 1ull);
    REQUIRE_EQ(entry_at(*store, 0).clean_generation, 4ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_eviction_respects_pin_generation_and_writer_owner) {
    const auto data = block_data(0x56);
    auto initial = myrd::test::FakeBackingStoreBuilder(0x9101)
                       .data_block(0, data)
                       .build();
    auto observer = std::make_shared<BlockingPinObserver>();
    auto pin_memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies pin_dependencies{};
    pin_dependencies.virtual_memory_api = pin_memory_api;
    pin_dependencies.read_observer = observer;
    auto store = require_store(
        backing_args(initial, 4, 3), pin_dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, data, 3));

    std::vector<std::byte> pinned_output(static_cast<std::size_t>(kBlock));
    std::atomic<std::uint32_t> reader_code{};
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, pinned_output);
        reader_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool pin_entered = observer->wait_until_entered();
    const auto pinned_pass = myrd::DynamicBlockStoreTestAccess::
        run_eviction_sweep(*store, 0);
    const auto decommits_while_pinned = pin_memory_api->decommit_calls();
    observer->release();
    reader.join();

    REQUIRE(pin_entered);
    REQUIRE_EQ(reader_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(pinned_pass.evicted_blocks, 0ull);
    REQUIRE_EQ(pinned_pass.skipped_pinned_blocks, 1ull);
    REQUIRE_EQ(decommits_while_pinned, 0ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentClean);
    REQUIRE(std::equal(
        pinned_output.begin(), pinned_output.end(), data.begin()));

    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 0, myrd::BlockState::ResidentClean, 0, false, 3));
    const auto released_pass = myrd::DynamicBlockStoreTestAccess::
        run_eviction_sweep(*store, 0);
    REQUIRE_EQ(released_pass.evicted_blocks, 1ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::Unresident);

    // Writer-first owner order: a pinned reader holds the old bytes while the
    // writer publishes Committing. Eviction must skip that transient owner.
    auto writer_first_observer = std::make_shared<BlockingPinObserver>();
    auto writer_first_memory =
        std::make_shared<GatedCountingVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies writer_first_dependencies{};
    writer_first_dependencies.virtual_memory_api = writer_first_memory;
    writer_first_dependencies.read_observer = writer_first_observer;
    auto writer_first_store = require_store(
        backing_args(initial, 4, 3), writer_first_dependencies);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *writer_first_store,
        0,
        myrd::BlockState::ResidentClean,
        data,
        3));
    std::vector<std::byte> writer_first_old_output(
        static_cast<std::size_t>(kBlock));
    std::atomic<std::uint32_t> writer_first_reader_code{};
    std::thread writer_first_reader([&] {
        const auto result = writer_first_store->read(
            myrd::OperationContext{}, 0, writer_first_old_output);
        writer_first_reader_code.store(
            result.ok() ? 0 : result_code(result),
            std::memory_order_release);
    });
    const bool writer_first_pin = writer_first_observer->wait_until_entered();
    const auto writer_first_data = block_data(0x96);
    std::atomic<std::uint32_t> writer_first_code{};
    std::thread writer_first_writer([&] {
        const auto result = writer_first_store->write(
            myrd::OperationContext{}, 0, writer_first_data);
        writer_first_code.store(
            result.ok() ? 0 : result_code(result),
            std::memory_order_release);
    });
    const bool writer_first_committing = wait_for_entry_state(
        *writer_first_store, 0, myrd::BlockState::Committing);
    const bool writer_first_waiting =
        wait_for_stripe_waiters(*writer_first_store, 1);
    const auto writer_first_eviction = myrd::DynamicBlockStoreTestAccess::
        run_eviction_sweep(*writer_first_store, 0);
    const auto writer_first_decommits =
        writer_first_memory->decommit_calls();
    writer_first_observer->release();
    writer_first_reader.join();
    writer_first_writer.join();

    REQUIRE(writer_first_pin);
    REQUIRE(writer_first_committing);
    REQUIRE(writer_first_waiting);
    REQUIRE_EQ(writer_first_eviction.evicted_blocks, 0ull);
    REQUIRE_EQ(writer_first_eviction.skipped_transient_blocks, 1ull);
    REQUIRE_EQ(writer_first_decommits, 0ull);
    REQUIRE_EQ(writer_first_reader_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(writer_first_code.load(std::memory_order_acquire), 0u);
    REQUIRE(std::equal(
        writer_first_old_output.begin(),
        writer_first_old_output.end(),
        data.begin()));
    REQUIRE_EQ(entry_at(*writer_first_store, 0).state,
               myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry_at(*writer_first_store, 0).version, 1ull);
    std::vector<std::byte> writer_first_output(
        static_cast<std::size_t>(kBlock));
    REQUIRE(writer_first_store->read(
        myrd::OperationContext{}, 0, writer_first_output).ok());
    REQUIRE(std::equal(
        writer_first_output.begin(),
        writer_first_output.end(),
        writer_first_data.begin()));
    REQUIRE(writer_first_store->validate_invariants_for_test().ok());

    auto impossible = require_store(backing_args(initial, 4, 3));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *impossible, 0, myrd::BlockState::ResidentClean, data, 4));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *impossible, 0, myrd::BlockState::ResidentClean, 0, false, 4));
    const auto impossible_pass = myrd::DynamicBlockStoreTestAccess::
        run_eviction_sweep(*impossible, 0);
    REQUIRE_EQ(impossible_pass.evicted_blocks, 0ull);
    REQUIRE_EQ(impossible_pass.skipped_generation_blocks, 1ull);
    REQUIRE_EQ(impossible_pass.invariant_failures, 1ull);
    REQUIRE_EQ(entry_at(*impossible, 0).state,
               myrd::BlockState::ResidentClean);

    const auto replacement = block_data(0xA6);
    auto memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    auto writer_store = require_store(
        backing_args(initial, 4, 3),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *writer_store, 0, myrd::BlockState::ResidentClean, data, 3));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *writer_store, 0, myrd::BlockState::ResidentClean, 0, false, 3));
    memory_api->block_next_decommit();
    myrd::EvictionPassResult eviction{};
    std::thread evictor([&] {
        eviction = myrd::DynamicBlockStoreTestAccess::
            run_eviction_sweep(*writer_store, 0);
    });
    const bool decommit_entered = memory_api->wait_until_decommit_entered();
    std::atomic<std::uint32_t> writer_code{};
    std::thread writer([&] {
        const auto result = writer_store->write(
            myrd::OperationContext{}, 0, replacement);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool writer_waiting = wait_for_stripe_waiters(*writer_store, 1);
    memory_api->release_decommit();
    evictor.join();
    writer.join();

    REQUIRE(decommit_entered);
    REQUIRE(writer_waiting);
    REQUIRE_EQ(eviction.evicted_blocks, 1ull);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(memory_api->decommit_calls(), 1ull);
    REQUIRE_EQ(entry_at(*writer_store, 0).state,
               myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry_at(*writer_store, 0).version, 1ull);
    std::vector<std::byte> writer_output(static_cast<std::size_t>(kBlock));
    REQUIRE(writer_store->read(
        myrd::OperationContext{}, 0, writer_output).ok());
    REQUIRE(std::equal(
        writer_output.begin(), writer_output.end(), replacement.begin()));
    REQUIRE(writer_store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_background_and_synchronous_eviction_have_one_owner) {
    auto backing = myrd::test::FakeBackingStoreBuilder(0xA101).build();
    auto memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    auto store = require_store(
        pressure_args(backing, 6),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    const auto clean = block_data(0x67);
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentClean, clean, 3));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
            *store, block, myrd::BlockState::ResidentClean, 0, false, 3));
    }

    memory_api->block_next_decommit();
    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    const bool signalled =
        myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(*store);
    const bool background_entered = memory_api->wait_until_decommit_entered();
    std::atomic<std::uint32_t> writer_code{};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 4 * kBlock, clean);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
    });
    const bool synchronous_waited =
        wait_for_synchronous_eviction_waiters(*store, 1);
    memory_api->release_decommit();
    writer.join();
    const bool worker_idle = myrd::DynamicBlockStoreTestAccess::
        wait_for_eviction_worker_idle(
            *store,
            passes_before + 1,
            std::chrono::steady_clock::now() + kWaitBound);

    REQUIRE(signalled);
    REQUIRE(background_entered);
    REQUIRE(synchronous_waited);
    REQUIRE(worker_idle);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(memory_api->decommit_calls(), 3ull);
    REQUIRE_EQ(store->stats().committed_bytes, 2 * kBlock);
    REQUIRE_EQ(store->stats().evicted_blocks_total, 3ull);
    REQUIRE_EQ(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(*store),
        0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_concurrency_loading_wait_observes_cancellation) {
    run_loading_wait_interruption(true);
}

TEST_CASE(m2_concurrency_loading_wait_observes_deadline) {
    run_loading_wait_interruption(false);
}

TEST_CASE(m2_concurrency_pin_wait_observes_cancellation) {
    run_pin_wait_interruption(true);
}

TEST_CASE(m2_concurrency_pin_wait_observes_deadline) {
    run_pin_wait_interruption(false);
}

TEST_CASE(m2_concurrency_pressure_wait_observes_cancellation) {
    run_pressure_wait_interruption(true);
}

TEST_CASE(m2_concurrency_pressure_wait_observes_deadline) {
    run_pressure_wait_interruption(false);
}

TEST_CASE(m2_concurrency_close_drains_blocked_io_worker_and_arena_once) {
    const auto old_data = block_data(0x68);
    const auto new_data = block_data(0xF8);
    auto backing_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto lifetime =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    myrd::test::FakeBackingStoreBuilder old_builder(0xE101);
    for (std::uint64_t block = 0; block <= 4; ++block) {
        old_builder.data_block(block, old_data);
    }
    auto old_backing = old_builder
                           .gate_block(4, backing_gate)
                           .lifetime_tracker(lifetime)
                           .build();
    auto successor =
        myrd::test::FakeBackingStoreBuilder::successor_of(*old_backing)
            .data_block(4, new_data)
            .lifetime_tracker(lifetime)
            .build();
    std::weak_ptr<const myrd::test::FakeBackingStore> old_weak = old_backing;
    std::weak_ptr<const myrd::test::FakeBackingStore> successor_weak =
        successor;
    auto memory_api = std::make_shared<GatedCountingVirtualMemoryApi>();
    auto store = require_store(
        backing_args(old_backing, 8, 1),
        myrd::DynamicBlockStoreTestDependencies{memory_api});
    old_backing.reset();

    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentClean, old_data, 1));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
            *store, block, myrd::BlockState::ResidentClean, 0, false, 1));
    }
    memory_api->block_next_decommit();
    const bool worker_signalled =
        myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(*store);
    const bool worker_blocked = memory_api->wait_until_decommit_entered();

    std::vector<std::byte> owner_output(
        static_cast<std::size_t>(kBlock), std::byte{0xA5});
    std::vector<std::byte> waiter_output(
        static_cast<std::size_t>(kBlock), std::byte{0xB6});
    std::atomic<std::uint32_t> owner_code{};
    std::atomic<std::uint32_t> waiter_code{};
    std::atomic<std::uint32_t> writer_code{};
    std::atomic<std::uint32_t> pressure_writer_code{};
    std::atomic<std::uint32_t> snapshot_code{};
    std::atomic<std::uint64_t> completed{};
    std::thread owner([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 4 * kBlock, owner_output);
        owner_code.store(result.ok() ? 0 : result_code(result),
                         std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    const bool backing_blocked = backing_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound);
    const bool successor_installed =
        store->install_backing_view({2, successor}).ok();
    successor.reset();

    std::thread waiter([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 4 * kBlock, waiter_output);
        waiter_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 4 * kBlock, new_data);
        writer_code.store(result.ok() ? 0 : result_code(result),
                          std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    myrd::DirtyScanCursor cursor{4};
    std::thread snapshot([&] {
        const auto result = store->copy_dirty_batch(
            myrd::OperationContext{}, cursor, kBlock);
        snapshot_code.store(result.ok() ? 0 : result.error().code,
                            std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    const bool waiters_queued = wait_for_stripe_waiters(*store, 3);
    std::thread pressure_writer([&] {
        const auto result = store->write(
            myrd::OperationContext{}, 5 * kBlock, new_data);
        pressure_writer_code.store(
            result.ok() ? 0 : result_code(result),
            std::memory_order_release);
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    const bool pressure_waiter_queued =
        wait_for_synchronous_eviction_waiters(*store, 1);

    std::atomic<bool> close_started{};
    std::atomic<bool> close_done{};
    std::thread closer([&] {
        close_started.store(true, std::memory_order_release);
        store->close();
        close_done.store(true, std::memory_order_release);
    });
    const bool close_entered = wait_for_true(close_started);
    const bool requests_drained_while_gates_closed = wait_for_count(completed, 5);
    if (!requests_drained_while_gates_closed) {
        backing_gate->open();
    }
    memory_api->release_decommit();
    const bool close_completed_within_bound = wait_for_true(close_done);
    owner.join();
    waiter.join();
    writer.join();
    pressure_writer.join();
    snapshot.join();
    closer.join();

    REQUIRE(worker_signalled);
    REQUIRE(worker_blocked);
    REQUIRE(backing_blocked);
    REQUIRE(successor_installed);
    REQUIRE(waiters_queued);
    REQUIRE(pressure_waiter_queued);
    REQUIRE(close_entered);
    REQUIRE(requests_drained_while_gates_closed);
    REQUIRE(close_completed_within_bound);
    REQUIRE(close_done.load(std::memory_order_acquire));
    const auto closed_code = static_cast<std::uint32_t>(
        myrd::OperationErrorCode::session_closed);
    REQUIRE_EQ(owner_code.load(std::memory_order_acquire), closed_code);
    REQUIRE_EQ(waiter_code.load(std::memory_order_acquire), closed_code);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), closed_code);
    REQUIRE_EQ(pressure_writer_code.load(std::memory_order_acquire),
               closed_code);
    REQUIRE_EQ(snapshot_code.load(std::memory_order_acquire), closed_code);
    REQUIRE_EQ(cursor.next_block_index, 4ull);
    REQUIRE(std::all_of(
        owner_output.begin(), owner_output.end(),
        [](std::byte value) { return value == std::byte{0xA5}; }));
    REQUIRE(std::all_of(
        waiter_output.begin(), waiter_output.end(),
        [](std::byte value) { return value == std::byte{0xB6}; }));
    REQUIRE(store->is_closed());
    REQUIRE(!store->worker_running_for_test());
    REQUIRE_EQ(entry_at(*store, 4).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry_at(*store, 5).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(store->stats().loading_blocks, 0ull);
    REQUIRE(old_weak.expired());
    REQUIRE(!successor_weak.expired());
    REQUIRE_EQ(memory_api->release_calls(), 0ull);

    store.reset();
    REQUIRE(successor_weak.expired());
    REQUIRE_EQ(lifetime->live_count(), 0ull);
    REQUIRE_EQ(lifetime->destroyed_count(), 2ull);
    REQUIRE_EQ(memory_api->release_calls(), 1ull);
}
