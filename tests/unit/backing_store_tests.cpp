#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/backing_store.h"
#include "src/storage/block_constants.h"
#include "src/storage/zero_backing_store.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using BlockBuffer =
    std::array<std::byte, static_cast<std::size_t>(myrd::kBlockSizeBytes)>;

std::uint32_t error_code(const myrd::Result<myrd::BlockLoadResult>& result) {
    REQUIRE(result.has_error());
    return result.error().code;
}

} // namespace

TEST_CASE(m2_backing_store_contract_has_exact_const_signature) {
    using Expected = myrd::Result<myrd::BlockLoadResult> (
        myrd::IBackingStore::*)(const myrd::OperationContext&, std::uint64_t,
        std::span<std::byte>) const;
    static_assert(std::is_same_v<decltype(&myrd::IBackingStore::load_block),
        Expected>);
    static_assert(std::has_virtual_destructor_v<myrd::IBackingStore>);
    REQUIRE(true);
}

TEST_CASE(m2_zero_backing_store_rejects_non_block_destinations) {
    const myrd::ZeroBackingStore store;
    std::vector<std::byte> short_destination(
        static_cast<std::size_t>(myrd::kBlockSizeBytes - 1));
    std::vector<std::byte> long_destination(
        static_cast<std::size_t>(myrd::kBlockSizeBytes + 1));

    const auto short_result = store.load_block(
        myrd::OperationContext{}, 0, short_destination);
    const auto long_result = store.load_block(
        myrd::OperationContext{}, 0, long_destination);

    REQUIRE_EQ(error_code(short_result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(error_code(long_result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::invalid_range));
}

TEST_CASE(m2_zero_backing_store_returns_all_zero_without_writing_destination) {
    const myrd::ZeroBackingStore store;
    BlockBuffer destination{};
    destination.fill(std::byte{0xA5});

    const auto result =
        store.load_block(myrd::OperationContext{}, 123, destination);

    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, myrd::BlockLoadKind::AllZero);
    for (const auto value : destination) {
        REQUIRE_EQ(value, std::byte{0xA5});
    }
}

TEST_CASE(m2_zero_backing_store_preserves_context_errors) {
    const myrd::ZeroBackingStore store;
    BlockBuffer destination{};
    std::stop_source stop_source;
    stop_source.request_stop();

    const auto cancelled = store.load_block(
        myrd::OperationContext{stop_source.get_token()}, 0, destination);
    const auto timed_out = store.load_block(
        myrd::OperationContext{{}, std::chrono::steady_clock::now() -
                                       std::chrono::milliseconds(1)},
        0, destination);

    REQUIRE_EQ(error_code(cancelled),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(error_code(timed_out),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out));
}

TEST_CASE(m2_fake_backing_store_loads_deterministic_block_data) {
    constexpr std::uint64_t seed = 0x1234'5678'9ABC'DEF0ull;
    constexpr std::uint64_t block_index = 37;
    const auto concrete =
        myrd::test::FakeBackingStoreBuilder(seed).build();
    std::shared_ptr<const myrd::IBackingStore> store = concrete;
    BlockBuffer first{};
    BlockBuffer second{};

    const auto first_result =
        store->load_block(myrd::OperationContext{}, block_index, first);
    const auto second_result =
        store->load_block(myrd::OperationContext{}, block_index, second);

    REQUIRE(first_result.ok());
    REQUIRE(second_result.ok());
    REQUIRE_EQ(first_result.value().kind, myrd::BlockLoadKind::Data);
    REQUIRE(first == second);
    REQUIRE_EQ(first[0], std::byte{0xA3});
    REQUIRE_EQ(first[1], std::byte{0x4F});
    REQUIRE_EQ(first[2], std::byte{0xDF});
    REQUIRE_EQ(first[127], std::byte{0x02});
    REQUIRE_EQ(first[4096], std::byte{0x65});
    REQUIRE_EQ(first[65535], std::byte{0x4E});

    BlockBuffer different_seed{};
    BlockBuffer different_block{};
    const auto seed_store =
        myrd::test::FakeBackingStoreBuilder(seed + 1).build();
    REQUIRE(seed_store->load_block(
                          myrd::OperationContext{}, block_index, different_seed)
                .ok());
    REQUIRE(store->load_block(
                     myrd::OperationContext{}, block_index + 2, different_block)
                .ok());
    REQUIRE(first != different_seed);
    REQUIRE(first != different_block);
}

TEST_CASE(m2_fake_backing_store_rejects_non_block_destinations) {
    const auto store = myrd::test::FakeBackingStoreBuilder(17).build();
    std::vector<std::byte> short_destination(
        static_cast<std::size_t>(myrd::kBlockSizeBytes - 1));
    std::vector<std::byte> long_destination(
        static_cast<std::size_t>(myrd::kBlockSizeBytes + 1));

    const auto short_result = store->load_block(
        myrd::OperationContext{}, 0, short_destination);
    const auto long_result = store->load_block(
        myrd::OperationContext{}, 0, long_destination);

    REQUIRE_EQ(error_code(short_result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(error_code(long_result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(store->total_load_count(), 0ull);
}

TEST_CASE(m2_fake_backing_store_returns_configured_all_zero_without_writing) {
    const auto store = myrd::test::FakeBackingStoreBuilder(99)
                           .all_zero_block(7)
                           .build();
    BlockBuffer destination{};
    destination.fill(std::byte{0x6D});

    const auto result =
        store->load_block(myrd::OperationContext{}, 7, destination);

    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, myrd::BlockLoadKind::AllZero);
    for (const auto value : destination) {
        REQUIRE_EQ(value, std::byte{0x6D});
    }
}

TEST_CASE(m2_fake_backing_store_supports_concurrent_const_loads) {
    constexpr std::uint64_t seed = 0xCAFE'BABEull;
    constexpr std::size_t thread_count = 12;
    constexpr std::array<std::byte, thread_count> expected_first_bytes{
        std::byte{0x46}, std::byte{0x86}, std::byte{0xA2}, std::byte{0x96},
        std::byte{0xBA}, std::byte{0xDF}, std::byte{0x24}, std::byte{0xB1},
        std::byte{0x3F}, std::byte{0x7A}, std::byte{0x1D}, std::byte{0x7A}};
    const auto concrete =
        myrd::test::FakeBackingStoreBuilder(seed).build();
    std::shared_ptr<const myrd::IBackingStore> store = concrete;
    std::array<BlockBuffer, thread_count> destinations{};
    std::array<std::thread, thread_count> threads;
    std::atomic<bool> valid{true};

    for (std::size_t i = 0; i < thread_count; ++i) {
        threads[i] = std::thread([&, i] {
            const auto result = store->load_block(
                myrd::OperationContext{}, i, destinations[i]);
            if (!result.ok() || result.value().kind !=
                                    myrd::BlockLoadKind::Data) {
                valid.store(false, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    REQUIRE(valid.load(std::memory_order_relaxed));
    for (std::size_t i = 0; i < thread_count; ++i) {
        REQUIRE_EQ(destinations[i][0], expected_first_bytes[i]);
        REQUIRE_EQ(concrete->block_load_count(i), 1ull);
    }
    REQUIRE_EQ(concrete->total_load_count(),
        static_cast<std::uint64_t>(thread_count));
}

TEST_CASE(m2_fake_backing_store_gate_blocks_and_releases_without_sleep) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto store = myrd::test::FakeBackingStoreBuilder(1)
                           .gate_block(4, gate)
                           .build();
    BlockBuffer destination{};
    auto future = std::async(std::launch::async, [&] {
        return store->load_block(
            myrd::OperationContext{}, 4, destination);
    });

    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::timeout);
    gate->open();

    const auto result = future.get();
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, myrd::BlockLoadKind::Data);
}

TEST_CASE(m2_fake_backing_store_gate_observes_cancellation) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto store = myrd::test::FakeBackingStoreBuilder(2)
                           .gate_block(5, gate)
                           .build();
    BlockBuffer destination{};
    std::stop_source stop_source;
    auto future = std::async(std::launch::async, [&] {
        return store->load_block(
            myrd::OperationContext{stop_source.get_token()}, 5, destination);
    });

    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    stop_source.request_stop();

    const auto result = future.get();
    REQUIRE_EQ(error_code(result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::cancelled));
}

TEST_CASE(m2_fake_backing_store_gate_observes_deadline) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    const auto store = myrd::test::FakeBackingStoreBuilder(3)
                           .gate_block(6, gate)
                           .build();
    BlockBuffer destination{};
    const myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(1)};
    auto future = std::async(std::launch::async, [&] {
        return store->load_block(context, 6, destination);
    });

    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));

    const auto result = future.get();
    REQUIRE_EQ(error_code(result),
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out));
}

TEST_CASE(m2_fake_backing_store_injects_block_occurrence_failure_and_counts_it) {
    const auto store = myrd::test::FakeBackingStoreBuilder(4)
                           .fail_block_on_load(9, 2)
                           .build();
    BlockBuffer destination{};

    const auto first =
        store->load_block(myrd::OperationContext{}, 9, destination);
    const auto second =
        store->load_block(myrd::OperationContext{}, 9, destination);
    const auto other =
        store->load_block(myrd::OperationContext{}, 10, destination);

    REQUIRE(first.ok());
    REQUIRE_EQ(error_code(second), static_cast<std::uint32_t>(
                                            myrd::OperationErrorCode::backing_read_failed));
    REQUIRE(other.ok());
    REQUIRE_EQ(store->block_load_count(9), 2ull);
    REQUIRE_EQ(store->block_load_count(10), 1ull);
    REQUIRE_EQ(store->total_load_count(), 3ull);
}

TEST_CASE(m2_fake_backing_store_injects_named_block_and_global_nth_failures) {
    const auto store = myrd::test::FakeBackingStoreBuilder(41)
                           .fail_block(12)
                           .fail_on_load(3)
                           .build();
    BlockBuffer destination{};

    const auto first =
        store->load_block(myrd::OperationContext{}, 1, destination);
    const auto named =
        store->load_block(myrd::OperationContext{}, 12, destination);
    const auto global_nth =
        store->load_block(myrd::OperationContext{}, 2, destination);
    const auto fourth =
        store->load_block(myrd::OperationContext{}, 3, destination);

    REQUIRE(first.ok());
    REQUIRE_EQ(error_code(named), static_cast<std::uint32_t>(
                                      myrd::OperationErrorCode::backing_read_failed));
    REQUIRE_EQ(error_code(global_nth), static_cast<std::uint32_t>(
                                           myrd::OperationErrorCode::backing_read_failed));
    REQUIRE(fourth.ok());
    REQUIRE_EQ(store->total_load_count(), 4ull);
    REQUIRE_EQ(store->block_load_count(12), 1ull);
}

TEST_CASE(m2_fake_backing_store_successor_is_complete_and_keeps_old_content) {
    constexpr std::uint64_t seed = 0x55AAull;
    BlockBuffer inherited_data{};
    inherited_data.fill(std::byte{0x3C});
    BlockBuffer patch{};
    patch.fill(std::byte{0xCC});
    const auto old_store = myrd::test::FakeBackingStoreBuilder(seed)
                               .data_block(6, inherited_data)
                               .all_zero_block(8)
                               .build();
    const auto successor =
        myrd::test::FakeBackingStoreBuilder::successor_of(*old_store)
            .data_block(7, patch)
            .build();
    BlockBuffer old_block{};
    BlockBuffer new_block{};
    BlockBuffer successor_inherited_data{};
    BlockBuffer inherited_zero{};
    inherited_zero.fill(std::byte{0xA9});

    const auto old_result =
        old_store->load_block(myrd::OperationContext{}, 7, old_block);
    const auto new_result =
        successor->load_block(myrd::OperationContext{}, 7, new_block);
    const auto inherited_result = successor->load_block(
        myrd::OperationContext{}, 8, inherited_zero);
    const auto inherited_data_result = successor->load_block(
        myrd::OperationContext{}, 6, successor_inherited_data);

    REQUIRE(old_result.ok());
    REQUIRE_EQ(old_result.value().kind, myrd::BlockLoadKind::Data);
    REQUIRE(old_block != patch);
    REQUIRE(new_result.ok());
    REQUIRE_EQ(new_result.value().kind, myrd::BlockLoadKind::Data);
    REQUIRE(new_block == patch);
    REQUIRE(inherited_data_result.ok());
    REQUIRE_EQ(inherited_data_result.value().kind, myrd::BlockLoadKind::Data);
    REQUIRE(successor_inherited_data == inherited_data);
    REQUIRE(inherited_result.ok());
    REQUIRE_EQ(inherited_result.value().kind, myrd::BlockLoadKind::AllZero);
    for (const auto value : inherited_zero) {
        REQUIRE_EQ(value, std::byte{0xA9});
    }

    BlockBuffer old_again{};
    REQUIRE(old_store->load_block(
                         myrd::OperationContext{}, 7, old_again)
                .ok());
    REQUIRE(old_again == old_block);
    BlockBuffer old_inherited_data{};
    REQUIRE(old_store->load_block(
                         myrd::OperationContext{}, 6, old_inherited_data)
                .ok());
    REQUIRE(old_inherited_data == inherited_data);
}

TEST_CASE(m2_fake_backing_store_inflight_shared_owner_controls_lifetime) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto tracker =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    auto concrete = myrd::test::FakeBackingStoreBuilder(5)
                        .gate_block(11, gate)
                        .lifetime_tracker(tracker)
                        .build();
    std::weak_ptr<const myrd::test::FakeBackingStore> weak = concrete;
    std::shared_ptr<const myrd::IBackingStore> published = concrete;
    concrete.reset();
    BlockBuffer destination{};
    std::promise<myrd::Result<myrd::BlockLoadResult>> completion;
    auto result_future = completion.get_future();
    std::thread worker([held = published, &destination,
                           completion = std::move(completion)]() mutable {
        completion.set_value(held->load_block(
            myrd::OperationContext{}, 11, destination));
        held.reset();
    });
    published.reset();

    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    REQUIRE(!weak.expired());
    REQUIRE_EQ(tracker->live_count(), 1ull);
    gate->open();

    REQUIRE(result_future.get().ok());
    worker.join();
    REQUIRE(weak.expired());
    REQUIRE_EQ(tracker->live_count(), 0ull);
    REQUIRE_EQ(tracker->destroyed_count(), 1ull);
}
