#include "test_framework.h"

#include "src/storage/block_directory.h"
#include "src/storage/dynamic_block_store_internal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t k64GiB = 64ull * 1024 * 1024 * 1024;
constexpr std::uint64_t k1TiB = 1024ull * 1024 * 1024 * 1024;

std::unique_ptr<myrd::BlockDirectory> require_directory(
    std::uint64_t block_count,
    myrd::InitialBlockState initial_state = myrd::InitialBlockState::CleanZero,
    myrd::BackingGeneration initial_generation = 0) {
    auto result = myrd::BlockDirectory::create(
        block_count, initial_state, initial_generation);
    REQUIRE(result.ok());
    return result.move_value();
}

std::unique_ptr<myrd::LockStripeSet> require_stripes(std::size_t count) {
    auto result = myrd::LockStripeSet::create(count);
    REQUIRE(result.ok());
    return result.move_value();
}

class ConcurrentPublicationProbe final
    : public myrd::IBlockLeafPublicationObserver {
public:
    void before_publish(std::uint64_t) noexcept override {
        const auto active =
            active_.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto observed = max_active_.load(std::memory_order_relaxed);
        while (observed < active &&
               !max_active_.compare_exchange_weak(
                   observed, active, std::memory_order_relaxed)) {
        }
        if (active >= 2) {
            release_.store(true, std::memory_order_release);
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(500);
        while (!release_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        release_.store(true, std::memory_order_release);
    }

    void after_publish(std::uint64_t) noexcept override {
        active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::size_t max_active() const noexcept {
        return max_active_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> max_active_{0};
    std::atomic<bool> release_{false};
};

} // namespace

TEST_CASE(m2_block_directory_leaf_and_entry_indices_cover_boundaries) {
    static_assert(myrd::block_leaf_index(0) == 0);
    static_assert(myrd::block_entry_index(0) == 0);
    static_assert(myrd::block_leaf_index(myrd::kBlocksPerLeaf - 1) == 0);
    static_assert(myrd::block_entry_index(myrd::kBlocksPerLeaf - 1) == 1023);
    static_assert(myrd::block_leaf_index(myrd::kBlocksPerLeaf) == 1);
    static_assert(myrd::block_entry_index(myrd::kBlocksPerLeaf) == 0);

    constexpr auto last_1t_block = (k1TiB / myrd::kBlockSizeBytes) - 1;
    REQUIRE_EQ(myrd::block_leaf_index(last_1t_block), 16383ull);
    REQUIRE_EQ(myrd::block_entry_index(last_1t_block), 1023ull);
}

TEST_CASE(m2_block_directory_leaf_has_exact_entry_count_and_coverage) {
    static_assert(myrd::BlockLeaf::kEntryCount == 1024);
    static_assert(myrd::BlockLeaf::kCoverageBytes == 64ull * 1024 * 1024);
    REQUIRE_EQ(myrd::BlockLeaf::kEntryCount, myrd::kBlocksPerLeaf);
    REQUIRE_EQ(myrd::BlockLeaf::kCoverageBytes, myrd::kLeafSizeBytes);
}

TEST_CASE(m2_block_directory_empty_directory_allocates_no_leaves) {
    const auto block_count = k1TiB / myrd::kBlockSizeBytes;
    auto directory = require_directory(block_count);

    REQUIRE_EQ(directory->top_level_slot_count(), 16384ull);
    REQUIRE_EQ(directory->allocated_leaf_count(), 0ull);
    REQUIRE(directory->find(0) == nullptr);
    REQUIRE(directory->find(block_count - 1) == nullptr);
    REQUIRE_EQ(directory->allocated_leaf_count(), 0ull);
}

TEST_CASE(m2_block_directory_first_explicit_access_lazily_creates_one_leaf) {
    auto directory = require_directory(4096, myrd::InitialBlockState::CleanZero, 17);

    auto created = directory->get_or_create(37);
    REQUIRE(created.ok());
    REQUIRE(created.value() != nullptr);
    REQUIRE_EQ(directory->allocated_leaf_count(), 1ull);
    REQUIRE(directory->find(37) == created.value());
    REQUIRE_EQ(created.value()->state, myrd::BlockState::ZeroClean);
    REQUIRE_EQ(created.value()->clean_generation, 17ull);

    auto same_leaf = directory->get_or_create(900);
    REQUIRE(same_leaf.ok());
    REQUIRE_EQ(directory->allocated_leaf_count(), 1ull);
    REQUIRE(same_leaf.value() != created.value());
}

TEST_CASE(m2_block_directory_implicit_initial_state_needs_no_entry) {
    auto zero = require_directory(2048, myrd::InitialBlockState::CleanZero, 3);
    auto backing = require_directory(
        2048, myrd::InitialBlockState::BackingUnresident, 9);

    REQUIRE(zero->find(0) == nullptr);
    REQUIRE(backing->find(0) == nullptr);
    REQUIRE_EQ(zero->allocated_leaf_count(), 0ull);
    REQUIRE_EQ(backing->allocated_leaf_count(), 0ull);
    REQUIRE_EQ(zero->initial_state(), myrd::InitialBlockState::CleanZero);
    REQUIRE_EQ(zero->initial_generation(), 3ull);
    REQUIRE_EQ(backing->initial_state(), myrd::InitialBlockState::BackingUnresident);
    REQUIRE_EQ(backing->initial_generation(), 9ull);
}

TEST_CASE(m2_block_directory_same_leaf_concurrent_creation_publishes_once) {
    constexpr std::size_t kThreads = 32;
    auto directory = require_directory(4096);
    std::array<myrd::BlockEntry*, kThreads> entries{};
    std::array<bool, kThreads> successes{};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto result = directory->get_or_create(73);
            successes[i] = result.ok();
            entries[i] = result.ok() ? result.value() : nullptr;
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_EQ(directory->allocated_leaf_count(), 1ull);
    for (std::size_t i = 0; i < kThreads; ++i) {
        REQUIRE(successes[i]);
        REQUIRE(entries[i] == entries[0]);
    }
}

TEST_CASE(m2_block_directory_different_leaves_publish_concurrently) {
    constexpr std::size_t kThreads = 2;
    auto probe = std::make_shared<ConcurrentPublicationProbe>();
    auto created = myrd::BlockDirectory::create(
        kThreads * myrd::kBlocksPerLeaf,
        myrd::InitialBlockState::CleanZero,
        0,
        probe);
    REQUIRE(created.ok());
    auto directory = created.move_value();
    std::array<myrd::BlockEntry*, kThreads> entries{};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto result = directory->get_or_create(i * myrd::kBlocksPerLeaf);
            entries[i] = result.ok() ? result.value() : nullptr;
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_EQ(directory->allocated_leaf_count(), kThreads);
    REQUIRE(probe->max_active() >= 2);
    for (const auto* entry : entries) {
        REQUIRE(entry != nullptr);
    }
}

TEST_CASE(m2_block_directory_published_entry_addresses_are_stable) {
    auto directory = require_directory(512ull * myrd::kBlocksPerLeaf);
    auto first = directory->get_or_create(19);
    REQUIRE(first.ok());
    auto* const stable_address = first.value();

    for (std::uint64_t leaf = 1; leaf < 512; ++leaf) {
        REQUIRE(directory->get_or_create(leaf * myrd::kBlocksPerLeaf).ok());
    }

    REQUIRE(directory->find(19) == stable_address);
    REQUIRE_EQ(directory->allocated_leaf_count(), 512ull);
}

TEST_CASE(m2_block_directory_enumerates_only_allocated_leaves_in_order) {
    auto directory = require_directory(5 * myrd::kBlocksPerLeaf);
    REQUIRE(directory->get_or_create(
        4 * myrd::kBlocksPerLeaf + 7).ok());
    REQUIRE(directory->get_or_create(
        myrd::kBlocksPerLeaf + 9).ok());

    const auto first = directory->next_allocated_leaf_at_or_after(0);
    REQUIRE(first.has_value());
    REQUIRE_EQ(first->leaf_index, 1ull);
    REQUIRE(first->leaf != nullptr);
    const auto second = directory->next_allocated_leaf_at_or_after(2);
    REQUIRE(second.has_value());
    REQUIRE_EQ(second->leaf_index, 4ull);
    REQUIRE(second->leaf != nullptr);
    REQUIRE(!directory->next_allocated_leaf_at_or_after(5).has_value());
    REQUIRE_EQ(directory->allocated_leaf_count(), 2ull);
}

TEST_CASE(m2_block_directory_rejects_invalid_construction_and_range) {
    const auto zero = myrd::BlockDirectory::create(
        0, myrd::InitialBlockState::CleanZero, 0);
    REQUIRE(!zero.ok());
    REQUIRE_EQ(zero.error(), myrd::BlockDirectoryError::InvalidBlockCount);

    auto directory = require_directory(100);
    REQUIRE(!directory->get_or_create(100).ok());
    REQUIRE(directory->find(100) == nullptr);
    REQUIRE_EQ(directory->allocated_leaf_count(), 0ull);

    const auto invalid_state = myrd::BlockDirectory::create(
        1, static_cast<myrd::InitialBlockState>(0xff), 0);
    REQUIRE(!invalid_state.ok());
    REQUIRE_EQ(invalid_state.error(),
               myrd::BlockDirectoryError::InvalidInitialState);
}

TEST_CASE(m2_lock_stripes_use_power_of_two_mask_mapping) {
    auto stripes = require_stripes(64);
    REQUIRE_EQ(stripes->count(), 64ull);
    REQUIRE_EQ(stripes->stripe_index(0), 0ull);
    REQUIRE_EQ(stripes->stripe_index(63), 63ull);
    REQUIRE_EQ(stripes->stripe_index(64), 0ull);
    REQUIRE_EQ(stripes->stripe_index(129), 1ull);
    REQUIRE(&stripes->stripe_for(1) == &stripes->stripe_for(65));
    REQUIRE(&stripes->stripe_for(1) != &stripes->stripe_for(2));

    REQUIRE(!myrd::LockStripeSet::create(0).ok());
    REQUIRE(!myrd::LockStripeSet::create(63).ok());
    REQUIRE(!myrd::LockStripeSet::create(65).ok());

    constexpr auto kLargestPowerOfTwo =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    const auto oversized = myrd::LockStripeSet::create(kLargestPowerOfTwo);
    REQUIRE(!oversized.ok());
    REQUIRE_EQ(oversized.error(), myrd::LockStripeSetError::AllocationFailed);
}

TEST_CASE(m2_block_directory_large_initial_metadata_is_sparse) {
    const auto blocks64 = k64GiB / myrd::kBlockSizeBytes;
    const auto blocks1t = k1TiB / myrd::kBlockSizeBytes;
    auto directory64 = require_directory(blocks64);
    auto directory1t = require_directory(blocks1t);

    const auto expected64 = 1024ull *
        (sizeof(std::atomic<myrd::BlockLeaf*>) + sizeof(std::uint64_t));
    const auto expected1t = 16384ull *
        (sizeof(std::atomic<myrd::BlockLeaf*>) + sizeof(std::uint64_t));
    REQUIRE_EQ(directory64->initial_metadata_bytes(), expected64);
    REQUIRE_EQ(directory1t->initial_metadata_bytes(), expected1t);
    REQUIRE_EQ(directory64->allocated_leaf_count(), 0ull);
    REQUIRE_EQ(directory1t->allocated_leaf_count(), 0ull);
    REQUIRE(directory1t->initial_metadata_bytes() <
            blocks1t * sizeof(myrd::BlockEntry) / 100);

    std::cout << "metadata: 64GiB=" << directory64->initial_metadata_bytes()
              << " bytes, 1TiB=" << directory1t->initial_metadata_bytes()
              << " bytes\n";
}

TEST_CASE(m2_block_entry_invariant_checker_rejects_generation_errors) {
    myrd::BlockEntry entry{};
    entry.state = myrd::BlockState::ResidentClean;
    entry.prior_state = myrd::BlockState::Unresident;
    entry.clean_generation = 7;

    REQUIRE(myrd::validate_block_entry(entry, 7, false, false).ok());
    REQUIRE_EQ(myrd::validate_block_entry(entry, 6, false, false).error,
               myrd::BlockEntryInvariantError::CleanGenerationAhead);
    REQUIRE_EQ(myrd::validate_block_entry(entry, 7, true, false).error,
               myrd::BlockEntryInvariantError::BackingGenerationWrapped);
    REQUIRE_EQ(myrd::validate_block_entry(entry, 7, false, true).error,
               myrd::BlockEntryInvariantError::ContentVersionWrapped);
}

TEST_CASE(m2_block_entry_invariant_checker_rejects_invalid_transient_prior) {
    myrd::BlockEntry entry{};
    entry.state = myrd::BlockState::Loading;
    entry.prior_state = myrd::BlockState::Committing;

    REQUIRE_EQ(myrd::validate_block_entry(entry, 0, false, false).error,
               myrd::BlockEntryInvariantError::TransientPriorNotStable);
}
