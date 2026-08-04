#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;

myrd::DynamicBlockStoreCreateArgs args_for(
    std::uint64_t block_count = 16,
    std::uint64_t low_blocks = 1,
    std::uint64_t soft_blocks = 8,
    std::uint64_t hard_blocks = 12) {
    return myrd::DynamicBlockStoreCreateArgs{
        block_count * kBlock,
        512,
        myrd::DynamicBlockStoreOptions{
            soft_blocks * kBlock,
            hard_blocks * kBlock,
            low_blocks * kBlock,
            kBlock,
            8,
            64,
            myrd::kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250},
        },
        myrd::InitialBlockState::BackingUnresident,
        myrd::test::FakeBackingStoreBuilder(0xE71C7).build(),
        5,
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

std::vector<std::byte> block_data(std::byte value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(kBlock), value);
}

myrd::BlockEntry entry_at(myrd::DynamicBlockStore& store,
                          std::uint64_t block_index) {
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        store, block_index, entry));
    return entry;
}

class FailOnceRealVirtualMemoryApi final : public myrd::IVirtualMemoryApi {
public:
    void fail_next_decommit() noexcept {
        fail_decommit_.store(true, std::memory_order_release);
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

    void* allocate(void* address,
                   std::size_t size_bytes,
                   std::uint32_t allocation_type,
                   std::uint32_t protection) noexcept override {
        return ::VirtualAlloc(address, size_bytes, allocation_type, protection);
    }

    bool free(void* address,
              std::size_t size_bytes,
              std::uint32_t free_type) noexcept override {
        if (free_type == MEM_DECOMMIT) {
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
        if (free_type == MEM_DECOMMIT &&
            fail_decommit_.exchange(false, std::memory_order_acq_rel)) {
            last_error_.store(ERROR_NOT_ENOUGH_MEMORY,
                              std::memory_order_release);
            return false;
        }
        const auto ok = ::VirtualFree(address, size_bytes, free_type) != FALSE;
        last_error_.store(ok ? ERROR_SUCCESS : ::GetLastError(),
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
    std::atomic<bool> fail_decommit_{false};
    std::atomic<std::uint32_t> last_error_{ERROR_SUCCESS};
    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    bool block_decommit_{};
    bool decommit_entered_{};
    bool decommit_released_{};
};

} // namespace

TEST_CASE(m2_eviction_clock_gives_a_referenced_clean_block_one_second_chance) {
    auto store = require_store(args_for());
    const auto bytes = block_data(std::byte{0x41});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, bytes, 5));

    std::vector<std::byte> output(static_cast<std::size_t>(kBlock));
    REQUIRE(store->read(myrd::OperationContext{}, 0, output).ok());
    REQUIRE(entry_at(*store, 0).reference_bit);

    const auto first =
        myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(first.cleared_reference_bits, 1ull);
    REQUIRE_EQ(first.evicted_blocks, 0ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentClean);
    REQUIRE(!entry_at(*store, 0).reference_bit);

    const auto second =
        myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(second.evicted_blocks, 1ull);
    const auto after = entry_at(*store, 0);
    REQUIRE_EQ(after.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(after.clean_generation, 5ull);
    REQUIRE_EQ(store->stats().committed_bytes, 0ull);
    REQUIRE_EQ(store->stats().evicted_blocks_total, 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_eviction_selects_only_reloadable_unpinned_resident_clean_blocks) {
    auto store = require_store(args_for(32, 1, 24, 28));
    const auto bytes = block_data(std::byte{0x52});
    for (std::uint64_t block = 0; block < 7; ++block) {
        const auto state = block == 1
            ? myrd::BlockState::ResidentDirty
            : myrd::BlockState::ResidentClean;
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, state, bytes, block == 3 ? 6 : 5));
    }
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
        *store, 7, myrd::BlockState::ZeroClean, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_zero(
        *store, 8, myrd::BlockState::ZeroDirty, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 2, myrd::BlockState::ResidentClean, 1, false, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 4, myrd::BlockState::Loading, 0, false, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 5, myrd::BlockState::Committing, 0, false, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
        *store, 6, myrd::BlockState::Evicting, 0, false, 5));

    const auto result =
        myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(result.evicted_blocks, 1ull);
    REQUIRE_EQ(result.skipped_dirty_blocks, 1ull);
    REQUIRE_EQ(result.skipped_pinned_blocks, 1ull);
    REQUIRE_EQ(result.skipped_generation_blocks, 1ull);
    REQUIRE_EQ(result.skipped_transient_blocks, 3ull);
    REQUIRE_EQ(result.skipped_zero_blocks, 2ull);
    REQUIRE_EQ(result.invariant_failures, 1ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry_at(*store, 1).state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(entry_at(*store, 2).state, myrd::BlockState::ResidentClean);
    REQUIRE_EQ(entry_at(*store, 3).state, myrd::BlockState::ResidentClean);
}

TEST_CASE(m2_eviction_decommit_failure_restores_metadata_and_accounting) {
    auto api = std::make_shared<FailOnceRealVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.virtual_memory_api = api;
    auto store = require_store(args_for(), std::move(dependencies));
    const auto bytes = block_data(std::byte{0x63});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, bytes, 5));
    const auto before = store->stats();
    api->fail_next_decommit();

    const auto result =
        myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(result.evicted_blocks, 0ull);
    REQUIRE_EQ(result.decommit_failures, 1ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentClean);
    const auto after = store->stats();
    REQUIRE_EQ(after.committed_bytes, before.committed_bytes);
    REQUIRE_EQ(after.resident_clean_blocks, before.resident_clean_blocks);
    REQUIRE_EQ(after.decommit_failures, before.decommit_failures + 1);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_eviction_transition_has_unique_owner_and_wakes_a_waiting_reader) {
    myrd::test::FakeBackingStore::BlockData backing_bytes{};
    backing_bytes.fill(std::byte{0x6D});
    auto args = args_for();
    args.backing_store = myrd::test::FakeBackingStoreBuilder(77)
                             .data_block(0, backing_bytes)
                             .build();
    auto api = std::make_shared<FailOnceRealVirtualMemoryApi>();
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.virtual_memory_api = api;
    auto store = require_store(std::move(args), std::move(dependencies));
    const std::vector<std::byte> resident_bytes(
        backing_bytes.begin(), backing_bytes.end());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, resident_bytes, 5));

    api->block_next_decommit();
    myrd::EvictionPassResult eviction{};
    std::thread evictor([&] {
        eviction = myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(
            *store, 0);
    });
    REQUIRE(api->wait_until_decommit_entered(
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));

    const auto during = entry_at(*store, 0);
    REQUIRE_EQ(during.state, myrd::BlockState::Evicting);
    REQUIRE_EQ(during.prior_state, myrd::BlockState::ResidentClean);
    REQUIRE(during.transition_owner != 0);
    REQUIRE_EQ(during.transition_owner, during.transition_epoch);

    std::vector<std::byte> output(static_cast<std::size_t>(kBlock));
    std::uint32_t read_code = 0;
    std::thread reader([&] {
        const auto result = store->read(
            myrd::OperationContext{}, 0, output);
        read_code = result.ok() ? 0 : result.error().code;
    });
    const auto waiter_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (store->stripe_waiter_count_for_test() == 0 &&
           std::chrono::steady_clock::now() < waiter_deadline) {
        std::this_thread::yield();
    }
    REQUIRE_EQ(store->stripe_waiter_count_for_test(), 1ull);

    api->release_decommit();
    evictor.join();
    reader.join();
    REQUIRE_EQ(eviction.evicted_blocks, 1ull);
    REQUIRE_EQ(read_code, 0u);
    REQUIRE(std::equal(
        output.begin(), output.end(), backing_bytes.begin()));
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::ResidentClean);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_eviction_persistent_cursor_reaches_far_sparse_leaves_and_wraps) {
    auto store = require_store(args_for(4096, 1, 8, 12));
    const auto bytes = block_data(std::byte{0x74});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, bytes, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 2048, myrd::BlockState::ResidentClean, bytes, 5));

    auto result = myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(
        *store, kBlock);
    REQUIRE_EQ(result.evicted_blocks, 1ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_cursor(*store),
               1ull);

    result = myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(result.evicted_blocks, 1ull);
    REQUIRE_EQ(entry_at(*store, 2048).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_cursor(*store),
               2049ull);

    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentClean, bytes, 5));
    result = myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(*store, 0);
    REQUIRE_EQ(result.evicted_blocks, 1ull);
    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_cursor(*store),
               1ull);
}

TEST_CASE(m2_eviction_reclaim_runs_bounded_second_chance_to_low_watermark) {
    auto store = require_store(args_for(16, 1, 8, 12));
    const auto bytes = block_data(std::byte{0x85});
    for (std::uint64_t block = 0; block < 4; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentClean, bytes, 5));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
            *store, block, myrd::BlockState::ResidentClean, 0, true, 5));
    }

    const auto result = myrd::DynamicBlockStoreTestAccess::reclaim_clean_to(
        *store, kBlock);
    REQUIRE(result.target_reached);
    REQUIRE_EQ(result.cleared_reference_bits, 4ull);
    REQUIRE_EQ(result.evicted_blocks, 3ull);
    REQUIRE_EQ(store->stats().committed_bytes, kBlock);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_eviction_worker_coalesces_pressure_and_waits_after_no_progress) {
    auto store = require_store(args_for(16, 1, 2, 8));
    const auto bytes = block_data(std::byte{0x96});
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 0, myrd::BlockState::ResidentDirty, bytes, 5));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 1, myrd::BlockState::ResidentDirty, bytes, 5));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(
        *store));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store, 2, myrd::BlockState::ResidentDirty, bytes, 5));

    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    const auto wakeups_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_wakeups(*store);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::signal_eviction_many_for_test(
        *store, 32));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        *store,
        passes_before + 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(
                   *store),
               passes_before + 1);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_worker_wakeups(
                   *store),
               wakeups_before + 1);
    REQUIRE_EQ(store->stats().committed_bytes, 3 * kBlock);

    REQUIRE(myrd::DynamicBlockStoreTestAccess::signal_eviction_many_for_test(
        *store, 1));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        *store,
        passes_before + 2,
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(
                   *store),
               passes_before + 2);
}

TEST_CASE(m2_eviction_worker_reclaims_after_soft_crossing_and_close_joins_it) {
    auto store = require_store(args_for(16, 1, 2, 8));
    const auto bytes = block_data(std::byte{0xA7});
    for (std::uint64_t block = 0; block < 3; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentClean, bytes, 5));
        REQUIRE(myrd::DynamicBlockStoreTestAccess::configure_eviction_entry(
            *store, block, myrd::BlockState::ResidentClean, 0, true, 5));
    }
    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::signal_eviction_if_needed(
        *store));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        *store,
        passes_before + 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE_EQ(store->stats().committed_bytes, kBlock);
    REQUIRE(store->validate_invariants_for_test().ok());

    store->close();
    REQUIRE(store->is_closed());
    REQUIRE(!store->worker_running_for_test());
}

TEST_CASE(m2_eviction_successful_reads_automatically_signal_on_soft_crossing) {
    auto store = require_store(args_for(16, 1, 2, 8));
    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    std::vector<std::byte> output(static_cast<std::size_t>(kBlock));
    for (std::uint64_t block = 0; block < 3; ++block) {
        REQUIRE(store->read(
            myrd::OperationContext{}, block * kBlock, output).ok());
    }

    REQUIRE(myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        *store,
        passes_before + 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE_EQ(store->stats().committed_bytes, kBlock);
    REQUIRE_EQ(store->stats().evicted_blocks_total, 2ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_eviction_persisted_ack_wakes_pressure_waiting_for_a_clean_victim) {
    auto store = require_store(args_for(16, 1, 2, 8));
    const auto bytes = block_data(std::byte{0xB8});
    for (std::uint64_t block = 0; block < 3; ++block) {
        REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
            *store, block, myrd::BlockState::ResidentDirty, bytes, 5));
    }
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_version(*store, 0, 1));

    myrd::test::FakeBackingStore::BlockData persisted_bytes{};
    persisted_bytes.fill(std::byte{0xB8});
    auto successor = myrd::test::FakeBackingStoreBuilder(0xE71C8)
                         .data_block(0, persisted_bytes)
                         .build();
    REQUIRE(store->install_backing_view({6, successor}).ok());
    const auto passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);
    REQUIRE(store->mark_persisted(
        0, 1, myrd::DirtyRecordKind::Data, 6).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        *store,
        passes_before + 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(2)));

    REQUIRE_EQ(entry_at(*store, 0).state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry_at(*store, 1).state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(store->stats().committed_bytes, 2 * kBlock);
    REQUIRE(store->validate_invariants_for_test().ok());
}
