#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"

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
