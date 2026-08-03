#include "test_framework.h"

#include "fake_virtual_memory_api.h"
#include "src/storage/block_constants.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

constexpr std::uint64_t k64GiB = 64ull * 1024 * 1024 * 1024;
constexpr std::uint64_t k1TiB = 1024ull * 1024 * 1024 * 1024;

template <class T>
std::uint32_t error_code(const myrd::Result<T>& result) {
    REQUIRE(result.has_error());
    return result.error().code;
}

std::unique_ptr<myrd::VirtualMemoryArena> require_arena(
    std::uint64_t size_bytes,
    const std::shared_ptr<myrd::test::FakeVirtualMemoryApi>& api,
    std::shared_ptr<myrd::VirtualMemoryCleanupTracker> cleanup = {}) {
    auto result = myrd::VirtualMemoryArena::create_with_api(
        size_bytes, api, std::move(cleanup));
    REQUIRE(result.ok());
    return result.move_value();
}

} // namespace

TEST_CASE(m2_virtual_memory_reserves_exact_large_ranges_with_win32_flags) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    {
        auto arena64 = require_arena(k64GiB, api);
        REQUIRE_EQ(arena64->reserved_bytes(), k64GiB);
    }
    {
        auto arena1t = require_arena(k1TiB, api);
        REQUIRE_EQ(arena1t->reserved_bytes(), k1TiB);
    }

    const auto calls = api->allocate_calls();
    REQUIRE_EQ(calls.size(), 2ull);
    REQUIRE(calls[0].address == nullptr);
    REQUIRE_EQ(calls[0].size_bytes, static_cast<std::size_t>(k64GiB));
    REQUIRE_EQ(calls[0].allocation_type, static_cast<std::uint32_t>(MEM_RESERVE));
    REQUIRE_EQ(calls[0].protection, static_cast<std::uint32_t>(PAGE_NOACCESS));
    REQUIRE(calls[1].address == nullptr);
    REQUIRE_EQ(calls[1].size_bytes, static_cast<std::size_t>(k1TiB));
    REQUIRE_EQ(calls[1].allocation_type, static_cast<std::uint32_t>(MEM_RESERVE));
    REQUIRE_EQ(calls[1].protection, static_cast<std::uint32_t>(PAGE_NOACCESS));
}

TEST_CASE(m2_virtual_memory_rejects_invalid_reservation_sizes_before_api_call) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    const auto zero = myrd::VirtualMemoryArena::create_with_api(0, api);
    const auto misaligned = myrd::VirtualMemoryArena::create_with_api(
        myrd::kBlockSizeBytes + 1, api);
    const auto null_api = myrd::VirtualMemoryArena::create_with_api(
        myrd::kBlockSizeBytes, nullptr);

    REQUIRE_EQ(error_code(zero), static_cast<std::uint32_t>(
                                    myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(error_code(misaligned), static_cast<std::uint32_t>(
                                          myrd::OperationErrorCode::invalid_range));
    REQUIRE_EQ(error_code(null_api), static_cast<std::uint32_t>(
                                       myrd::OperationErrorCode::internal_invariant));
    REQUIRE(api->allocate_calls().empty());
}

TEST_CASE(m2_virtual_memory_reserve_failure_is_atomic_and_preserves_native_error) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    api->fail(myrd::test::FakeVirtualMemoryOperation::Reserve,
        ERROR_NOT_ENOUGH_MEMORY);

    const auto result = myrd::VirtualMemoryArena::create_with_api(
        myrd::kBlockSizeBytes, api);

    REQUIRE_EQ(error_code(result), static_cast<std::uint32_t>(
                                      myrd::OperationErrorCode::memory_hard_limit));
    REQUIRE_EQ(result.error().native_code,
        static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY));
    REQUIRE(api->free_calls().empty());
}

TEST_CASE(m2_virtual_memory_block_addresses_are_stable_checked_and_exact) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(3 * myrd::kBlockSizeBytes, api);

    const auto first = arena->block_address(0);
    const auto middle = arena->block_address(1);
    const auto last = arena->block_address(2);
    const auto invalid = arena->block_address(3);

    REQUIRE(first.ok());
    REQUIRE(middle.ok());
    REQUIRE(last.ok());
    REQUIRE_EQ(middle.value() - first.value(),
        static_cast<std::ptrdiff_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(last.value() - first.value(),
        static_cast<std::ptrdiff_t>(2 * myrd::kBlockSizeBytes));
    REQUIRE_EQ(error_code(invalid), static_cast<std::uint32_t>(
                                         myrd::OperationErrorCode::invalid_range));
}

TEST_CASE(m2_virtual_memory_commit_and_decommit_use_exact_block_calls) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(2 * myrd::kBlockSizeBytes, api);
    const auto expected_address = arena->block_address(1);
    REQUIRE(expected_address.ok());

    const auto committed = arena->commit_block(1);
    REQUIRE(committed.ok());
    REQUIRE(committed.value() == expected_address.value());
    REQUIRE_EQ(arena->committed_bytes(), myrd::kBlockSizeBytes);
    REQUIRE(arena->is_block_committed(1));

    const auto allocations = api->allocate_calls();
    REQUIRE_EQ(allocations.size(), 2ull);
    REQUIRE(allocations[1].address == expected_address.value());
    REQUIRE_EQ(allocations[1].size_bytes,
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(allocations[1].allocation_type,
        static_cast<std::uint32_t>(MEM_COMMIT));
    REQUIRE_EQ(allocations[1].protection,
        static_cast<std::uint32_t>(PAGE_READWRITE));

    REQUIRE(arena->decommit_block(1).ok());
    REQUIRE_EQ(arena->committed_bytes(), 0ull);
    REQUIRE(!arena->is_block_committed(1));
    const auto frees = api->free_calls();
    REQUIRE_EQ(frees.size(), 1ull);
    REQUIRE(frees[0].address == expected_address.value());
    REQUIRE_EQ(frees[0].size_bytes,
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(frees[0].free_type, static_cast<std::uint32_t>(MEM_DECOMMIT));
}

TEST_CASE(m2_virtual_memory_rejects_double_and_out_of_range_transitions) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(myrd::kBlockSizeBytes, api);

    REQUIRE(arena->commit_block(0).ok());
    const auto double_commit = arena->commit_block(0);
    const auto invalid_commit = arena->commit_block(1);
    REQUIRE_EQ(error_code(double_commit), static_cast<std::uint32_t>(
                                               myrd::OperationErrorCode::internal_invariant));
    REQUIRE_EQ(error_code(invalid_commit), static_cast<std::uint32_t>(
                                                myrd::OperationErrorCode::invalid_range));
    REQUIRE(arena->decommit_block(0).ok());
    const auto double_decommit = arena->decommit_block(0);
    const auto invalid_decommit = arena->decommit_block(1);
    REQUIRE_EQ(error_code(double_decommit), static_cast<std::uint32_t>(
                                                 myrd::OperationErrorCode::internal_invariant));
    REQUIRE_EQ(error_code(invalid_decommit), static_cast<std::uint32_t>(
                                                  myrd::OperationErrorCode::invalid_range));

    REQUIRE_EQ(api->allocate_calls().size(), 2ull);
    REQUIRE_EQ(api->free_calls().size(), 1ull);
}

TEST_CASE(m2_virtual_memory_commit_failure_keeps_accounting_and_is_retryable) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(myrd::kBlockSizeBytes, api);
    api->fail(myrd::test::FakeVirtualMemoryOperation::Commit,
        ERROR_COMMITMENT_LIMIT);

    const auto failed = arena->commit_block(0);
    REQUIRE_EQ(error_code(failed), static_cast<std::uint32_t>(
                                      myrd::OperationErrorCode::commit_failed));
    REQUIRE_EQ(failed.error().native_code,
        static_cast<std::uint32_t>(ERROR_COMMITMENT_LIMIT));
    REQUIRE_EQ(arena->committed_bytes(), 0ull);
    REQUIRE(!arena->is_block_committed(0));

    api->clear_failure(myrd::test::FakeVirtualMemoryOperation::Commit);
    REQUIRE(arena->commit_block(0).ok());
    REQUIRE_EQ(arena->committed_bytes(), myrd::kBlockSizeBytes);
}

TEST_CASE(m2_virtual_memory_decommit_failure_remains_conservatively_committed) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(myrd::kBlockSizeBytes, api);
    REQUIRE(arena->commit_block(0).ok());
    api->fail(myrd::test::FakeVirtualMemoryOperation::Decommit,
        ERROR_NOT_ENOUGH_MEMORY);

    const auto failed = arena->decommit_block(0);
    REQUIRE_EQ(error_code(failed), static_cast<std::uint32_t>(
                                      myrd::OperationErrorCode::decommit_failed));
    REQUIRE_EQ(failed.error().native_code,
        static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY));
    REQUIRE_EQ(arena->committed_bytes(), myrd::kBlockSizeBytes);
    REQUIRE(arena->is_block_committed(0));

    api->clear_failure(myrd::test::FakeVirtualMemoryOperation::Decommit);
    REQUIRE(arena->decommit_block(0).ok());
    REQUIRE_EQ(arena->committed_bytes(), 0ull);
}

TEST_CASE(m2_virtual_memory_query_cross_checks_reserved_and_committed_state) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(myrd::kBlockSizeBytes, api);

    const auto reserved = arena->query_block(0);
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value().state,
        myrd::VirtualMemoryRegionState::Reserved);
    REQUIRE(arena->commit_block(0).ok());
    const auto committed = arena->query_block(0);
    REQUIRE(committed.ok());
    REQUIRE_EQ(committed.value().state,
        myrd::VirtualMemoryRegionState::Committed);
    REQUIRE(arena->decommit_block(0).ok());
    const auto decommitted = arena->query_block(0);
    REQUIRE(decommitted.ok());
    REQUIRE_EQ(decommitted.value().state,
        myrd::VirtualMemoryRegionState::Reserved);
    REQUIRE_EQ(api->query_count(), 3ull);
}

TEST_CASE(m2_virtual_memory_query_failure_preserves_native_error) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto arena = require_arena(myrd::kBlockSizeBytes, api);
    api->fail(myrd::test::FakeVirtualMemoryOperation::Query,
        ERROR_INVALID_ADDRESS);

    const auto failed = arena->query_block(0);
    REQUIRE_EQ(error_code(failed), static_cast<std::uint32_t>(
                                      myrd::OperationErrorCode::internal_invariant));
    REQUIRE_EQ(failed.error().native_code,
        static_cast<std::uint32_t>(ERROR_INVALID_ADDRESS));
}

TEST_CASE(m2_virtual_memory_destruction_releases_only_base_once) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    void* base = nullptr;
    {
        auto arena = require_arena(2 * myrd::kBlockSizeBytes, api);
        base = arena->base_address();
        REQUIRE(arena->commit_block(1).ok());
    }

    const auto frees = api->free_calls();
    REQUIRE_EQ(frees.size(), 1ull);
    REQUIRE(frees[0].address == base);
    REQUIRE_EQ(frees[0].size_bytes, 0ull);
    REQUIRE_EQ(frees[0].free_type, static_cast<std::uint32_t>(MEM_RELEASE));
}

TEST_CASE(m2_virtual_memory_move_construction_does_not_double_release) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    void* base = nullptr;
    {
        auto source = require_arena(myrd::kBlockSizeBytes, api);
        base = source->base_address();
        myrd::VirtualMemoryArena moved(std::move(*source));
        REQUIRE(moved.base_address() == base);
        REQUIRE(source->base_address() == nullptr);
    }

    const auto frees = api->free_calls();
    REQUIRE_EQ(frees.size(), 1ull);
    REQUIRE(frees[0].address == base);
}

TEST_CASE(m2_virtual_memory_release_failure_is_observable_without_throwing) {
    auto api = std::make_shared<myrd::test::FakeVirtualMemoryApi>();
    auto cleanup = std::make_shared<myrd::VirtualMemoryCleanupTracker>();
    api->fail(myrd::test::FakeVirtualMemoryOperation::Release,
        ERROR_INVALID_ADDRESS);
    {
        auto arena = require_arena(
            myrd::kBlockSizeBytes, api, cleanup);
        REQUIRE_EQ(cleanup->release_failures(), 0ull);
    }

    REQUIRE_EQ(api->free_calls().size(), 1ull);
    REQUIRE_EQ(cleanup->release_failures(), 1ull);
    REQUIRE_EQ(cleanup->last_native_error(),
        static_cast<std::uint32_t>(ERROR_INVALID_ADDRESS));
}
