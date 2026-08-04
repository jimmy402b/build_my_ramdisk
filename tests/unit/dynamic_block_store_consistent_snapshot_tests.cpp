#include "test_framework.h"

#include "src/storage/dynamic_block_store.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

myrd::DynamicBlockStoreOptions valid_options() {
    return myrd::DynamicBlockStoreOptions{
        2 * myrd::kBlockSizeBytes,
        3 * myrd::kBlockSizeBytes,
        myrd::kBlockSizeBytes,
        2 * myrd::kBlockSizeBytes,
        8,
        64,
        myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
}

} // namespace

TEST_CASE(m2_1_snapshot_public_contract_has_exact_types_and_signatures) {
    static_assert(std::is_trivially_copyable_v<myrd::ConsistentSnapshotToken>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::snapshot_id),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::source_backing_generation),
                  myrd::BackingGeneration>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotToken::cutoff_dirty_epoch),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::DynamicBlockStoreStats::snapshot_active_state),
                  std::optional<myrd::ConsistentSnapshotState>>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::state),
                  myrd::ConsistentSnapshotState>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::cow_bytes),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::cow_records),
                  std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(myrd::ConsistentSnapshotStatus::blocked_writers),
                  std::uint64_t>);

    using BeginSignature = myrd::Result<myrd::ConsistentSnapshotToken>
        (myrd::DynamicBlockStore::*)(const myrd::OperationContext&);
    using CopySignature =
        myrd::Result<std::vector<myrd::DirtyBlockSnapshot>>
        (myrd::DynamicBlockStore::*)(
            const myrd::OperationContext&,
            const myrd::ConsistentSnapshotToken&,
            myrd::DirtyScanCursor&,
            std::size_t);
    using FinishSignature = myrd::Result<void>
        (myrd::DynamicBlockStore::*)(
            const myrd::ConsistentSnapshotToken&);
    using StatusSignature = myrd::Result<myrd::ConsistentSnapshotStatus>
        (myrd::DynamicBlockStore::*)(
            const myrd::ConsistentSnapshotToken&) const;

    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::begin_consistent_snapshot),
                  BeginSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::copy_consistent_snapshot_batch),
                  CopySignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::finish_consistent_snapshot),
                  FinishSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::abort_consistent_snapshot),
                  FinishSignature>);
    static_assert(std::is_same_v<
                  decltype(&myrd::DynamicBlockStore::consistent_snapshot_status),
                  StatusSignature>);
}

TEST_CASE(m2_1_snapshot_states_and_errors_have_stable_strings) {
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::Active),
               std::string_view("Active"));
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::Aborted),
               std::string_view("Aborted"));
    REQUIRE_EQ(myrd::to_string(myrd::ConsistentSnapshotState::FullyScanned),
               std::string_view("FullyScanned"));

    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotBusy),
               std::string_view("SnapshotBusy"));
    REQUIRE_EQ(
        myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid),
        std::string_view("SnapshotTokenInvalid"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::SnapshotAborted),
               std::string_view("SnapshotAborted"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreErrorCode::SnapshotNotFullyScanned),
        std::string_view("SnapshotNotFullyScanned"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreErrorCode::SnapshotResourceExhausted),
        std::string_view("SnapshotResourceExhausted"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall),
        std::string_view("SnapshotCowBytesTooSmall"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive),
        std::string_view("SnapshotCowStallNotPositive"));
    REQUIRE_EQ(
        myrd::to_string(
            myrd::DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge),
        std::string_view("SnapshotCowStallTooLarge"));
}

TEST_CASE(m2_1_snapshot_options_enforce_exact_unaligned_budget_boundary) {
    auto options = valid_options();
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_cow_max_bytes = myrd::kMinimumSnapshotCowBytes - 1;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall);

    options = valid_options();
    options.snapshot_cow_max_bytes = myrd::kMinimumSnapshotCowBytes + 1;
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());
}

TEST_CASE(m2_1_snapshot_stall_limit_is_strictly_positive_and_at_most_five_seconds) {
    auto options = valid_options();
    options.snapshot_writer_stall_limit = std::chrono::milliseconds{-1};
    REQUIRE_EQ(
        myrd::validate_dynamic_block_store_options(options).error,
        myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive);

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{0};
    REQUIRE_EQ(
        myrd::validate_dynamic_block_store_options(options).error,
        myrd::DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive);

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{1};
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{5'000};
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.snapshot_writer_stall_limit = std::chrono::milliseconds{5'001};
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge);
}
