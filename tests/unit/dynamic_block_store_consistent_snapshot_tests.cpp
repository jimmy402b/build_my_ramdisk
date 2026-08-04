#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/snapshot_coordinator.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
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
    std::uint64_t logical_blocks = 4,
    myrd::BackingGeneration generation = 7) {
    auto options = valid_options();
    // Keep admission tests independent of eviction pressure: their four-block
    // fixtures can make every logical block resident at once.
    options.soft_limit_bytes = 3 * myrd::kBlockSizeBytes;
    options.hard_limit_bytes = 4 * myrd::kBlockSizeBytes;
    return myrd::DynamicBlockStoreCreateArgs{
        logical_blocks * myrd::kBlockSizeBytes,
        512,
        options,
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

myrd::OperationContext bounded_context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

template <class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

std::uint32_t error_code(const myrd::Error& error) {
    return error.code;
}

constexpr auto kNoResult =
    (std::numeric_limits<std::uint32_t>::max)();

class SnapshotCoordinatorBarrier final
    : public myrd::ISnapshotCoordinatorObserver {
public:
    explicit SnapshotCoordinatorBarrier(
        myrd::SnapshotCoordinatorEvent target) noexcept
        : target_(target) {}

    void on_event(myrd::SnapshotCoordinatorEvent event) noexcept override {
        if (event != target_) {
            return;
        }
        reached_.store(true, std::memory_order_release);
        reached_.notify_all();
        release_.wait(false, std::memory_order_acquire);
    }

    bool reached() const noexcept {
        return reached_.load(std::memory_order_acquire);
    }

    void release() noexcept {
        release_.store(true, std::memory_order_release);
        release_.notify_all();
    }

private:
    myrd::SnapshotCoordinatorEvent target_;
    std::atomic<bool> reached_{};
    std::atomic<bool> release_{};
};

myrd::SnapshotPreimage metadata_preimage(
    std::uint64_t block_index,
    std::uint64_t version,
    myrd::DirtyRecordKind kind,
    myrd::BackingGeneration base_generation,
    std::uint64_t dirty_epoch) {
    return myrd::SnapshotPreimage{
        block_index,
        version,
        kind,
        base_generation,
        dirty_epoch,
        {},
    };
}

} // namespace

TEST_CASE(m2_1_coordinator_owns_one_exact_token_and_cut_membership) {
    myrd::SnapshotCoordinator coordinator(
        4 * myrd::kSnapshotCowRecordBudgetBytes);

    auto activated = coordinator.activate(7, 10);
    REQUIRE(activated.ok());
    const auto token = activated.value();
    REQUIRE(token.snapshot_id != 0);
    REQUIRE_EQ(token.source_backing_generation, 7ull);
    REQUIRE_EQ(token.cutoff_dirty_epoch, 10ull);

    auto duplicate = coordinator.activate(8, 11);
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(error_code(duplicate.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));

    auto wrong_generation = token;
    wrong_generation.source_backing_generation = 8;
    auto foreign = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        wrong_generation,
        metadata_preimage(1, 3, myrd::DirtyRecordKind::Zero, 7, 10));
    REQUIRE(!foreign.ok());
    REQUIRE_EQ(error_code(foreign.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));

    auto member = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(1, 3, myrd::DirtyRecordKind::Zero, 7, 10));
    REQUIRE(member.ok());
    REQUIRE_EQ(member.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    auto published = coordinator.publish_metadata_preimage(
        bounded_context(), member.value());
    REQUIRE(published.ok());

    auto post_cut = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(2, 4, myrd::DirtyRecordKind::Discard, 7, 11));
    REQUIRE(post_cut.ok());
    REQUIRE_EQ(post_cut.value().disposition(),
               myrd::SnapshotPreimageDisposition::NotMember);

    auto future_generation = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(3, 5, myrd::DirtyRecordKind::Zero, 8, 9));
    REQUIRE(future_generation.ok());
    REQUIRE_EQ(future_generation.value().disposition(),
               myrd::SnapshotPreimageDisposition::NotMember);
}

TEST_CASE(m2_1_coordinator_preserves_once_and_uses_only_monotonic_frontier) {
    myrd::SnapshotCoordinator coordinator(
        2 * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(9, 20);
    REQUIRE(activated.ok());
    const auto token = activated.value();

    auto first = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 11, myrd::DirtyRecordKind::Zero, 8, 20));
    REQUIRE(first.ok());
    REQUIRE(coordinator.publish_metadata_preimage(
        bounded_context(), first.value()).ok());

    auto duplicate = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 12, myrd::DirtyRecordKind::Discard, 9, 20));
    REQUIRE(duplicate.ok());
    REQUIRE_EQ(duplicate.value().disposition(),
               myrd::SnapshotPreimageDisposition::AlreadyPreserved);
    const auto saved = coordinator.peek_preimage(4);
    REQUIRE(saved.has_value());
    REQUIRE_EQ(saved->version, 11ull);
    REQUIRE_EQ(saved->kind, myrd::DirtyRecordKind::Zero);
    REQUIRE_EQ(saved->base_generation, 8ull);
    REQUIRE_EQ(saved->dirty_epoch, 20ull);
    REQUIRE(saved->data.empty());

    REQUIRE(coordinator.advance_scan_frontier(token, 5, 1'000'000).ok());
    REQUIRE_EQ(coordinator.snapshot().next_scan_block, 5ull);
    REQUIRE(!coordinator.peek_preimage(4).has_value());

    auto consumed = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(4, 13, myrd::DirtyRecordKind::Discard, 9, 20));
    REQUIRE(consumed.ok());
    REQUIRE_EQ(consumed.value().disposition(),
               myrd::SnapshotPreimageDisposition::Consumed);

    REQUIRE(coordinator.advance_scan_frontier(
        token, 999'999, 1'000'000).ok());
    const auto after_large_advance = coordinator.snapshot();
    REQUIRE_EQ(after_large_advance.next_scan_block, 999'999ull);
    REQUIRE_EQ(after_large_advance.cow_current_bytes, 0ull);
    REQUIRE_EQ(after_large_advance.cow_records, 0ull);
    REQUIRE_EQ(after_large_advance.pending_reservations, 0ull);

    auto rewind = coordinator.advance_scan_frontier(
        token, 999'998, 1'000'000);
    REQUIRE(!rewind.ok());
    REQUIRE_EQ(coordinator.snapshot().next_scan_block, 999'999ull);
}

TEST_CASE(m2_1_coordinator_charges_metadata_and_rolls_back_reservations) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(5, 8);
    REQUIRE(activated.ok());
    const auto token = activated.value();

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(1, 2, myrd::DirtyRecordKind::Discard, 5, 8));
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.cow_peak_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 1ull);
    REQUIRE(!coordinator.peek_preimage(1).has_value());

    auto exhausted = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 5, 8));
    REQUIRE(!exhausted.ok());
    REQUIRE_EQ(error_code(exhausted.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes,
               myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE_EQ(accounting.pending_reservations, 1ull);

    coordinator.rollback_metadata_preimage(reserved.value());
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(1).has_value());

    auto cancelled_reservation = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        token,
        metadata_preimage(3, 4, myrd::DirtyRecordKind::Zero, 5, 8));
    REQUIRE(cancelled_reservation.ok());
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = coordinator.publish_metadata_preimage(
        myrd::OperationContext{
            stop.get_token(),
            std::chrono::steady_clock::time_point::max()},
        cancelled_reservation.value());
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(error_code(cancelled.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(3).has_value());
}

TEST_CASE(m2_1_cancel_during_reservation_leaves_no_ghost_record_or_bytes) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorEvent::ReservationInserted);
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes, barrier);
    auto activated = coordinator.activate(6, 12);
    REQUIRE(activated.ok());
    const auto token = activated.value();
    std::stop_source stop;
    std::atomic<std::uint32_t> reserve_code{kNoResult};

    std::thread reserver([&] {
        auto result = coordinator.reserve_metadata_before_mutation(
            myrd::OperationContext{
                stop.get_token(),
                std::chrono::steady_clock::time_point::max()},
            token,
            metadata_preimage(
                7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
        reserve_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] { return barrier->reached(); }));
    stop.request_stop();
    barrier->release();
    reserver.join();

    REQUIRE_EQ(reserve_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
    REQUIRE(!coordinator.peek_preimage(7).has_value());
}

TEST_CASE(m2_1_wrong_coordinator_cannot_orphan_a_pending_reservation) {
    myrd::SnapshotCoordinator owner(
        myrd::kSnapshotCowRecordBudgetBytes);
    myrd::SnapshotCoordinator stranger(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto owner_token = owner.activate(4, 6);
    auto stranger_token = stranger.activate(8, 9);
    REQUIRE(owner_token.ok());
    REQUIRE(stranger_token.ok());

    auto reserved = owner.reserve_metadata_before_mutation(
        bounded_context(),
        owner_token.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(reserved.ok());
    auto wrong_publish = stranger.publish_metadata_preimage(
        bounded_context(), reserved.value());
    REQUIRE(!wrong_publish.ok());
    REQUIRE_EQ(error_code(wrong_publish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
    REQUIRE_EQ(owner.snapshot().pending_reservations, 1ull);

    owner.rollback_metadata_preimage(reserved.value());
    const auto after_rollback = owner.snapshot();
    REQUIRE_EQ(after_rollback.cow_current_bytes, 0ull);
    REQUIRE_EQ(after_rollback.pending_reservations, 0ull);
    REQUIRE(!owner.peek_preimage(2).has_value());
}

TEST_CASE(m2_1_coordinator_token_finish_abort_lifecycle_is_exact) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(3, 4);
    REQUIRE(activated.ok());
    const auto first = activated.value();

    auto early_finish = coordinator.finish(first, 2);
    REQUIRE(!early_finish.ok());
    REQUIRE_EQ(error_code(early_finish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotNotFullyScanned));
    REQUIRE(coordinator.status(first).ok());

    REQUIRE(coordinator.advance_scan_frontier(first, 2, 2).ok());
    auto fully_scanned = coordinator.status(first);
    REQUIRE(fully_scanned.ok());
    REQUIRE_EQ(fully_scanned.value().state,
               myrd::ConsistentSnapshotState::FullyScanned);
    REQUIRE(coordinator.finish(first, 2).ok());

    auto duplicate_finish = coordinator.finish(first, 2);
    REQUIRE(!duplicate_finish.ok());
    REQUIRE_EQ(error_code(duplicate_finish.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));

    auto second_begin = coordinator.activate(4, 5);
    REQUIRE(second_begin.ok());
    REQUIRE(second_begin.value().snapshot_id > first.snapshot_id);
    const auto second = second_begin.value();
    REQUIRE(coordinator.abort(second).ok());
    auto duplicate_abort = coordinator.abort(second);
    REQUIRE(!duplicate_abort.ok());
    REQUIRE_EQ(error_code(duplicate_abort.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_coordinator_never_treats_data_as_metadata_only) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kMinimumSnapshotCowBytes);
    auto activated = coordinator.activate(2, 3);
    REQUIRE(activated.ok());
    auto data = metadata_preimage(
        0, 1, myrd::DirtyRecordKind::Data, 2, 3);

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(), activated.value(), data);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
}

TEST_CASE(m2_1_store_coordinator_lock_path_holds_no_stripe_or_state_lock) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorEvent::ActivateLocked);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_observer = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(13).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 13), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    std::atomic<std::uint32_t> begin_code{kNoResult};

    std::thread begin([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        begin_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] { return barrier->reached(); }));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::all_stripes_available(*store));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::state_mutex_available(*store));
    barrier->release();
    begin.join();
    REQUIRE_EQ(begin_code.load(std::memory_order_acquire), 0u);
}

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

TEST_CASE(m2_1_cut_drains_the_whole_multiblock_mutation_and_does_not_block_reads) {
    auto first_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto second_gate =
        std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x11))
                       .data_block(1, block_data(0x22))
                       .data_block(3, block_data(0x44))
                       .gate_block(0, first_gate)
                       .gate_block(1, second_gate)
                       .build();
    auto store = require_store(backing_args(backing));

    std::vector<std::byte> spanning_write(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x5A});
    std::vector<std::byte> queued_write(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x6B});
    std::atomic<std::uint32_t> first_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<bool> cut_done{};
    std::atomic<bool> queued_done{};
    myrd::ConsistentSnapshotToken token{};

    std::thread first([&] {
        const auto result = store->write(
            bounded_context(), 512, spanning_write);
        first_code.store(result.ok() ? 0 : error_code(result.error()),
                         std::memory_order_release);
    });
    REQUIRE(first_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
               1ull);

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        if (result.ok()) {
            token = result.value();
            cut_code.store(0, std::memory_order_release);
        } else {
            cut_code.store(error_code(result.error()),
                           std::memory_order_release);
        }
        cut_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    // Reaching the gate accessor proves the waiting begin released the
    // lifecycle/admission mutex. Direct probes prove it also holds neither
    // state nor any block stripe while draining the admitted mutation.
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::state_mutex_available(
                   *store) &&
               myrd::DynamicBlockStoreTestAccess::all_stripes_available(
                   *store);
    }));

    std::atomic<std::uint32_t> overlapping_begin_code{kNoResult};
    std::thread overlapping_begin([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        overlapping_begin_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return overlapping_begin_code.load(std::memory_order_acquire) !=
               kNoResult;
    }));
    REQUIRE_EQ(overlapping_begin_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));
    overlapping_begin.join();

    std::thread queued([&] {
        const auto result = store->write(
            bounded_context(), 2 * myrd::kBlockSizeBytes, queued_write);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
        queued_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    std::vector<std::byte> read_output(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    const auto read = store->read(
        bounded_context(), 3 * myrd::kBlockSizeBytes, read_output);
    REQUIRE(read.ok());
    REQUIRE_EQ(read_output.front(), block_data(0x44).front());

    first_gate->open();
    REQUIRE(second_gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    REQUIRE(!cut_done.load(std::memory_order_acquire));
    REQUIRE(!queued_done.load(std::memory_order_acquire));

    second_gate->open();
    first.join();
    cut.join();
    queued.join();

    REQUIRE_EQ(first_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire), 0u);
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire), 0u);
    REQUIRE(token.snapshot_id != 0);
    REQUIRE_EQ(token.source_backing_generation, 7ull);
    REQUIRE_EQ(token.cutoff_dirty_epoch, 2ull);
    const auto stats = store->stats();
    REQUIRE_EQ(stats.snapshot_active_id, token.snapshot_id);
    REQUIRE_EQ(stats.snapshot_active_state,
               std::optional{myrd::ConsistentSnapshotState::Active});
    REQUIRE_EQ(stats.snapshot_blocked_writers, 1ull);
}

TEST_CASE(m2_1_zero_and_unmap_hold_one_token_across_all_ranges) {
    for (const bool discard : std::array{false, true}) {
        auto first_gate =
            std::make_shared<myrd::test::FakeBackingStoreGate>();
        auto second_gate =
            std::make_shared<myrd::test::FakeBackingStoreGate>();
        auto backing = myrd::test::FakeBackingStoreBuilder(7)
                           .data_block(0, block_data(0x21))
                           .data_block(1, block_data(0x22))
                           .gate_block(0, first_gate)
                           .gate_block(1, second_gate)
                           .build();
        auto store = require_store(backing_args(backing));
        const std::array ranges{
            myrd::DataRange{0, 512},
            myrd::DataRange{myrd::kBlockSizeBytes, 512},
        };
        std::atomic<std::uint32_t> mutation_code{kNoResult};
        std::atomic<std::uint32_t> cut_code{kNoResult};
        std::atomic<bool> cut_done{};
        myrd::ConsistentSnapshotToken token{};

        std::thread mutation([&] {
            const auto result = discard
                ? store->unmap(bounded_context(), ranges)
                : store->zero(bounded_context(), ranges);
            mutation_code.store(
                result.ok() ? 0 : error_code(result.error()),
                std::memory_order_release);
        });
        REQUIRE(first_gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE_EQ(
            myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
            1ull);

        std::thread cut([&] {
            const auto result = store->begin_consistent_snapshot(
                bounded_context());
            if (result.ok()) {
                token = result.value();
                cut_code.store(0, std::memory_order_release);
            } else {
                cut_code.store(error_code(result.error()),
                               std::memory_order_release);
            }
            cut_done.store(true, std::memory_order_release);
        });
        REQUIRE(wait_until([&] {
            return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
                *store);
        }));

        first_gate->open();
        REQUIRE(second_gate->wait_until_entered(
            1, std::chrono::steady_clock::now() + kWaitBound));
        REQUIRE(!cut_done.load(std::memory_order_acquire));
        second_gate->open();

        mutation.join();
        cut.join();
        REQUIRE_EQ(mutation_code.load(std::memory_order_acquire), 0u);
        REQUIRE_EQ(cut_code.load(std::memory_order_acquire), 0u);
        REQUIRE_EQ(token.cutoff_dirty_epoch, 2ull);
    }
}

TEST_CASE(m2_1_cancelled_begin_reopens_admission_without_waiting_for_old_writer) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x31))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x71});
    std::vector<std::byte> queued_data(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x72});
    std::stop_source cut_stop;
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};
    std::atomic<bool> queued_done{};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            myrd::OperationContext{
                cut_stop.get_token(),
                std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));

    std::thread queued([&] {
        const auto result = store->write(
            bounded_context(), myrd::kBlockSizeBytes, queued_data);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
        queued_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    cut_stop.request_stop();
    REQUIRE(wait_until([&] {
        return cut_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE(wait_until([&] {
        return queued_done.load(std::memory_order_acquire);
    }));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire), 0u);
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);
    REQUIRE_EQ(store->stats().snapshot_active_id, 0ull);

    gate->open();
    old_writer.join();
    cut.join();
    queued.join();
}

TEST_CASE(m2_1_timed_out_begin_reopens_admission_after_old_writer_drains) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x35))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x75});
    std::atomic<std::uint32_t> cut_code{kNoResult};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));

    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            myrd::OperationContext{
                {}, std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(50)});
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    REQUIRE(wait_until([&] {
        return cut_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(store->stats().snapshot_active_id, 0ull);

    gate->open();
    old_writer.join();
    cut.join();
}

TEST_CASE(m2_1_mutation_admission_honors_cancellation_and_deadline) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x41))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x51});
    std::vector<std::byte> full(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x52});
    std::stop_source mutation_stop;
    std::atomic<std::uint32_t> cancelled_code{kNoResult};
    std::atomic<std::uint32_t> deadline_code{kNoResult};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    std::thread cut([&] {
        (void)store->begin_consistent_snapshot(bounded_context());
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));

    std::thread cancelled([&] {
        const auto result = store->write(
            myrd::OperationContext{
                mutation_stop.get_token(),
                std::chrono::steady_clock::now() + std::chrono::seconds(5)},
            myrd::kBlockSizeBytes,
            full);
        cancelled_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    std::thread deadline([&] {
        const std::array ranges{myrd::DataRange{
            2 * myrd::kBlockSizeBytes, myrd::kBlockSizeBytes}};
        const auto result = store->zero(
            myrd::OperationContext{
                {}, std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(50)},
            ranges);
        deadline_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               2;
    }));
    mutation_stop.request_stop();
    REQUIRE(wait_until([&] {
        return cancelled_code.load(std::memory_order_acquire) != kNoResult &&
               deadline_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(cancelled_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    REQUIRE_EQ(deadline_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));

    gate->open();
    old_writer.join();
    cut.join();
    cancelled.join();
    deadline.join();
}

TEST_CASE(m2_1_close_wakes_cut_and_mutation_admission_waiters) {
    auto gate = std::make_shared<myrd::test::FakeBackingStoreGate>();
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x61))
                       .gate_block(0, gate)
                       .build();
    auto store = require_store(backing_args(backing));
    std::vector<std::byte> partial(512, std::byte{0x62});
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};

    std::thread old_writer([&] {
        (void)store->write(bounded_context(), 0, partial);
    });
    REQUIRE(gate->wait_until_entered(
        1, std::chrono::steady_clock::now() + kWaitBound));
    std::thread cut([&] {
        const auto result = store->begin_consistent_snapshot(
            bounded_context());
        cut_code.store(result.ok() ? 0 : error_code(result.error()),
                       std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(
            *store);
    }));
    std::thread queued([&] {
        const std::array ranges{myrd::DataRange{
            myrd::kBlockSizeBytes, myrd::kBlockSizeBytes}};
        const auto result = store->unmap(
            bounded_context(), ranges);
        queued_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store) ==
               1;
    }));

    std::atomic<bool> close_done{};
    std::thread closer([&] {
        store->close();
        close_done.store(true, std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return cut_code.load(std::memory_order_acquire) != kNoResult &&
               queued_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE(!close_done.load(std::memory_order_acquire));

    gate->open();
    old_writer.join();
    cut.join();
    queued.join();
    closer.join();
    REQUIRE(close_done.load(std::memory_order_acquire));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::mutation_gate_closed(*store));
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::active_mutations(*store),
               0ull);
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);
}

TEST_CASE(m2_1_only_one_concurrent_begin_can_own_the_active_snapshot) {
    auto backing = myrd::test::FakeBackingStoreBuilder(19).build();
    auto store = require_store(backing_args(backing, 2, 19));
    std::atomic<bool> start{};
    std::array<std::uint32_t, 2> codes{kNoResult, kNoResult};
    std::array<myrd::ConsistentSnapshotToken, 2> tokens{};
    std::array<std::thread, 2> begins;
    for (std::size_t index = 0; index < begins.size(); ++index) {
        begins[index] = std::thread([&, index] {
            start.wait(false, std::memory_order_acquire);
            const auto result = store->begin_consistent_snapshot(
                bounded_context());
            if (result.ok()) {
                tokens[index] = result.value();
                codes[index] = 0;
            } else {
                codes[index] = error_code(result.error());
            }
        });
    }
    start.store(true, std::memory_order_release);
    start.notify_all();
    for (auto& begin : begins) {
        begin.join();
    }

    const auto busy = static_cast<std::uint32_t>(
        myrd::DynamicBlockStoreErrorCode::SnapshotBusy);
    REQUIRE((codes[0] == 0 && codes[1] == busy) ||
            (codes[1] == 0 && codes[0] == busy));
    const auto winner = codes[0] == 0 ? 0u : 1u;
    REQUIRE(tokens[winner].snapshot_id != 0);
    REQUIRE_EQ(tokens[winner].source_backing_generation, 19ull);
    REQUIRE_EQ(tokens[winner].cutoff_dirty_epoch, 0ull);
}
