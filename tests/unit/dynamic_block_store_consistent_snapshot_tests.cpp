#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/snapshot_coordinator.h"

#include <algorithm>
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

std::uint64_t payload_hash(std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto value : payload) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::byte> as_vector(
    const myrd::test::FakeBackingStore::BlockData& data) {
    return {data.begin(), data.end()};
}

constexpr auto kNoResult =
    (std::numeric_limits<std::uint32_t>::max)();

class SnapshotCoordinatorBarrier final
    : public myrd::ISnapshotCoordinatorNonReentrantBarrier {
public:
    explicit SnapshotCoordinatorBarrier(
        myrd::SnapshotCoordinatorBarrierEvent target) noexcept
        : target_(target) {}

    void pause_while_coordinator_locked(
        myrd::SnapshotCoordinatorBarrierEvent event) noexcept override {
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
    myrd::SnapshotCoordinatorBarrierEvent target_;
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
    REQUIRE(!future_generation.ok());
    REQUIRE_EQ(error_code(future_generation.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));
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
    REQUIRE(coordinator.validate_invariants_for_test().ok());

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
    REQUIRE(coordinator.validate_invariants_for_test().ok());

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
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_cancel_during_reservation_leaves_no_ghost_record_or_bytes) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ReservationInserted);
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

TEST_CASE(m2_1_pending_reservation_can_outlive_destroyed_coordinator) {
    alignas(myrd::SnapshotCoordinator)
        std::array<std::byte, sizeof(myrd::SnapshotCoordinator)> storage{};
    std::optional<myrd::SnapshotMetadataReservation> pending;
    auto* coordinator = std::construct_at(
        reinterpret_cast<myrd::SnapshotCoordinator*>(storage.data()),
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator->activate(6, 12);
    REQUIRE(activated.ok());
    auto reserved = coordinator->reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(
            7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    pending.emplace(reserved.move_value());
    REQUIRE_EQ(coordinator->snapshot().pending_reservations, 1ull);
    std::destroy_at(coordinator);

    // Reuse the exact address and reservation fields. A dangling raw owner
    // would now erase the replacement coordinator's unrelated reservation.
    auto* replacement = std::construct_at(
        reinterpret_cast<myrd::SnapshotCoordinator*>(storage.data()),
        myrd::kSnapshotCowRecordBudgetBytes);
    auto replacement_token = replacement->activate(6, 12);
    REQUIRE(replacement_token.ok());
    auto replacement_pending = replacement->reserve_metadata_before_mutation(
        bounded_context(),
        replacement_token.value(),
        metadata_preimage(
            7, 9, myrd::DirtyRecordKind::Discard, 6, 12));
    REQUIRE(replacement_pending.ok());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 1ull);

    pending.reset();
    REQUIRE(!pending.has_value());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 1ull);
    replacement->rollback_metadata_preimage(replacement_pending.value());
    REQUIRE_EQ(replacement->snapshot().pending_reservations, 0ull);
    std::destroy_at(replacement);
}

TEST_CASE(m2_1_snapshot_token_id_is_unique_across_equal_coordinators) {
    myrd::SnapshotCoordinator owner(
        myrd::kSnapshotCowRecordBudgetBytes);
    myrd::SnapshotCoordinator stranger(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto owner_token = owner.activate(4, 6);
    auto stranger_token = stranger.activate(4, 6);
    REQUIRE(owner_token.ok());
    REQUIRE(stranger_token.ok());
    REQUIRE(owner_token.value().snapshot_id !=
            stranger_token.value().snapshot_id);

    auto foreign = stranger.reserve_metadata_before_mutation(
        bounded_context(),
        owner_token.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(!foreign.ok());
    REQUIRE_EQ(error_code(foreign.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotTokenInvalid));
}

TEST_CASE(m2_1_pending_duplicate_is_busy_until_rollback) {
    myrd::SnapshotCoordinator coordinator(
        2 * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(4, 6);
    REQUIRE(activated.ok());

    auto first = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 3, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(first.ok());
    auto duplicate = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 4, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(error_code(duplicate.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::SnapshotBusy));

    coordinator.rollback_metadata_preimage(first.value());
    auto retry = coordinator.reserve_metadata_before_mutation(
        bounded_context(),
        activated.value(),
        metadata_preimage(2, 5, myrd::DirtyRecordKind::Zero, 4, 6));
    REQUIRE(retry.ok());
    REQUIRE_EQ(retry.value().disposition(),
               myrd::SnapshotPreimageDisposition::Reserved);
    coordinator.rollback_metadata_preimage(retry.value());
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

TEST_CASE(m2_1_coordinator_rejects_unknown_dirty_record_kind) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(2, 3);
    REQUIRE(activated.ok());
    auto invalid = metadata_preimage(
        0,
        1,
        static_cast<myrd::DirtyRecordKind>(0xFF),
        2,
        3);

    auto reserved = coordinator.reserve_metadata_before_mutation(
        bounded_context(), activated.value(), invalid);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant));
    const auto accounting = coordinator.snapshot();
    REQUIRE_EQ(accounting.cow_current_bytes, 0ull);
    REQUIRE_EQ(accounting.cow_records, 0ull);
    REQUIRE_EQ(accounting.pending_reservations, 0ull);
}

TEST_CASE(m2_1_coordinator_map_has_bounded_logarithmic_probe_growth) {
    constexpr std::uint64_t kRecordCount = 10'000;
    myrd::SnapshotCoordinator coordinator(
        kRecordCount * myrd::kSnapshotCowRecordBudgetBytes);
    auto activated = coordinator.activate(9, 10);
    REQUIRE(activated.ok());

    for (std::uint64_t block = 0; block < kRecordCount; ++block) {
        auto reserved = coordinator.reserve_metadata_before_mutation(
            bounded_context(),
            activated.value(),
            metadata_preimage(
                block,
                block + 1,
                block % 2 == 0 ? myrd::DirtyRecordKind::Zero
                               : myrd::DirtyRecordKind::Discard,
                9,
                10));
        REQUIRE(reserved.ok());
        REQUIRE(coordinator.publish_metadata_preimage(
                    bounded_context(), reserved.value())
                    .ok());
    }

    const auto filled = coordinator.snapshot();
    REQUIRE_EQ(filled.cow_records, kRecordCount);
    REQUIRE_EQ(
        filled.cow_current_bytes,
        kRecordCount * myrd::kSnapshotCowRecordBudgetBytes);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
    const auto probes_after_build =
        coordinator.index_probe_count_for_test();
    REQUIRE(probes_after_build < kRecordCount * 80);

    for (std::uint64_t block = kRecordCount; block-- > 0;) {
        const auto preimage = coordinator.peek_preimage(block);
        REQUIRE(preimage.has_value());
        REQUIRE_EQ(preimage->block_index, block);
        REQUIRE_EQ(preimage->version, block + 1);
    }
    const auto reverse_lookup_probes =
        coordinator.index_probe_count_for_test() - probes_after_build;
    REQUIRE(reverse_lookup_probes < kRecordCount * 32);
    REQUIRE(coordinator.validate_invariants_for_test().ok());

    REQUIRE(coordinator.advance_scan_frontier(
                activated.value(), kRecordCount, kRecordCount)
                .ok());
    const auto consumed = coordinator.snapshot();
    REQUIRE_EQ(consumed.cow_records, 0ull);
    REQUIRE_EQ(consumed.cow_current_bytes, 0ull);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_store_coordinator_lock_path_holds_no_stripe_or_state_lock) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ActivateLocked);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
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
        // snapshot_blocked_writers is cumulative, so unlike the instantaneous
        // waiter gauge it cannot be missed if the 50 ms deadline expires just
        // before this thread is scheduled.
        return store->stats().snapshot_blocked_writers >= 2;
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
    REQUIRE_EQ(myrd::DynamicBlockStoreTestAccess::mutation_waiters(*store),
               0ull);

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
    std::atomic<std::uint32_t> old_writer_code{kNoResult};
    std::atomic<std::uint32_t> cut_code{kNoResult};
    std::atomic<std::uint32_t> queued_code{kNoResult};

    std::thread old_writer([&] {
        const auto result = store->write(bounded_context(), 0, partial);
        old_writer_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
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
        return old_writer_code.load(std::memory_order_acquire) != kNoResult &&
               cut_code.load(std::memory_order_acquire) != kNoResult &&
               queued_code.load(std::memory_order_acquire) != kNoResult;
    }));
    REQUIRE_EQ(old_writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(cut_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    REQUIRE_EQ(queued_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::session_closed));
    // The backing gate observes the close stop token, so the in-flight writer
    // may drain and let close finish before this thread is scheduled again.
    // Assert outcomes, not that racy scheduling window.
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

TEST_CASE(m2_1_data_cow_preserves_resident_dirty_partial_write_preimage) {
    const auto backing_payload = block_data(0x31);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, backing_payload)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));

    auto reference = as_vector(backing_payload);
    const std::vector<std::byte> pre_cut_patch(4096, std::byte{0xA5});
    REQUIRE(store->write(bounded_context(), 1024, pre_cut_patch).ok());
    std::copy(pre_cut_patch.begin(), pre_cut_patch.end(),
              reference.begin() + 1024);

    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));
    REQUIRE_EQ(cut_entry.state, myrd::BlockState::ResidentDirty);
    REQUIRE_EQ(cut_entry.dirty_kind, myrd::DirtyRecordKind::Data);

    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    const std::vector<std::byte> post_cut_patch(2048, std::byte{0x5A});
    REQUIRE(store->write(bounded_context(), 2048, post_cut_patch).ok());

    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, preimage));
    REQUIRE_EQ(preimage.block_index, 0ull);
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.kind, cut_entry.dirty_kind);
    REQUIRE_EQ(preimage.base_generation,
               cut_entry.dirty_base_generation);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(preimage.data.size(),
               static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(reference));
    REQUIRE(preimage.data == reference);
}

TEST_CASE(m2_1_data_cow_preserves_only_first_full_block_preimage) {
    const auto backing_payload = block_data(0x42);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, backing_payload)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));

    std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x17});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    std::vector<std::byte> first_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x28});
    REQUIRE(store->write(bounded_context(), 0, first_replacement).ok());
    const auto bytes_after_first = store->stats().snapshot_cow_current_bytes;
    REQUIRE_EQ(bytes_after_first, myrd::kMinimumSnapshotCowBytes);

    std::vector<std::byte> second_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x39});
    REQUIRE(store->write(bounded_context(), 0, second_replacement).ok());
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               bytes_after_first);

    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, preimage));
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(reference));
    REQUIRE(preimage.data == reference);
}

TEST_CASE(m2_1_data_cow_payload_allocation_failure_rolls_back_record) {
    myrd::SnapshotCoordinator coordinator(
        myrd::kSnapshotCowRecordBudgetBytes);
    const auto begun = coordinator.activate(7, 1);
    REQUIRE(begun.ok());
    const auto payload = block_data(0x53);

    const auto reserved = coordinator.reserve_data_before_mutation(
        bounded_context(),
        begun.value(),
        metadata_preimage(
            0, 1, myrd::DirtyRecordKind::Data, 7, 1),
        payload);
    REQUIRE(!reserved.ok());
    REQUIRE_EQ(error_code(reserved.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    const auto state = coordinator.snapshot();
    REQUIRE_EQ(state.cow_current_bytes, 0ull);
    REQUIRE_EQ(state.cow_records, 0ull);
    REQUIRE_EQ(state.pending_reservations, 0ull);
    REQUIRE(coordinator.validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_data_cow_preserves_each_member_of_cross_block_write) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x61))
                       .data_block(1, block_data(0x72))
                       .build();
    auto args = backing_args(backing, 3, 7);
    args.options.snapshot_cow_max_bytes =
        2 * myrd::kMinimumSnapshotCowBytes;
    auto store = require_store(std::move(args));

    std::vector<std::byte> first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x18});
    std::vector<std::byte> second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x29});
    REQUIRE(store->write(bounded_context(), 0, first).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second).ok());
    myrd::BlockEntry first_cut{};
    myrd::BlockEntry second_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> spanning(1024, std::byte{0x7B});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes - 512, spanning).ok());

    myrd::SnapshotPreimage first_preimage{};
    myrd::SnapshotPreimage second_preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, first_preimage));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, second_preimage));
    REQUIRE_EQ(first_preimage.version, first_cut.version);
    REQUIRE_EQ(second_preimage.version, second_cut.version);
    REQUIRE_EQ(payload_hash(first_preimage.data), payload_hash(first));
    REQUIRE_EQ(payload_hash(second_preimage.data), payload_hash(second));
    REQUIRE(first_preimage.data == first);
    REQUIRE(second_preimage.data == second);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               2 * myrd::kMinimumSnapshotCowBytes);
}

TEST_CASE(m2_1_data_cow_consumed_member_does_not_allocate) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0x83))
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const std::vector<std::byte> pre_cut(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x34});
    REQUIRE(store->write(bounded_context(), 0, pre_cut).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::advance_snapshot_frontier(
        *store, begun.value(), 1));

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x45});
    REQUIRE(store->write(bounded_context(), 0, replacement).ok());
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
}

TEST_CASE(m2_1_clean_backing_at_cut_load_mutate_is_excluded_from_cow) {
    const auto original = block_data(0x94);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, original)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE_EQ(begun.value().cutoff_dirty_epoch, 0ull);

    const std::vector<std::byte> patch(512, std::byte{0x56});
    REQUIRE(store->write(bounded_context(), 1024, patch).ok());
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);

    auto reference = as_vector(original);
    std::copy(patch.begin(), patch.end(), reference.begin() + 1024);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(actual == reference);
}

TEST_CASE(m2_1_backing_load_failure_publishes_neither_mutation_nor_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xA5))
                       .fail_block(0)
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> patch(512, std::byte{0x67});
    const auto written = store->write(bounded_context(), 1024, patch);
    REQUIRE(!written.ok());
    REQUIRE_EQ(error_code(written.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::backing_read_failed));
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    myrd::BlockEntry entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, entry));
    REQUIRE_EQ(entry.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(entry.dirty_epoch, 0ull);
}

TEST_CASE(m2_1_data_cow_budget_failure_keeps_second_block_unmodified) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xB6))
                       .data_block(1, block_data(0xC7))
                       .build();
    auto store = require_store(backing_args(backing, 3, 7));
    const std::vector<std::byte> first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x78});
    const std::vector<std::byte> second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x89});
    REQUIRE(store->write(bounded_context(), 0, first).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> first_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0x9A});
    REQUIRE(store->write(
        bounded_context(), 0, first_replacement).ok());
    myrd::BlockEntry second_before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_before));
    const std::vector<std::byte> second_replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xAB});
    const auto rejected = store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));
    myrd::BlockEntry second_after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_after));
    REQUIRE_EQ(second_after.state, second_before.state);
    REQUIRE_EQ(second_after.version, second_before.version);
    REQUIRE_EQ(second_after.dirty_epoch, second_before.dirty_epoch);
    REQUIRE_EQ(second_after.dirty_kind, second_before.dirty_kind);
    REQUIRE_EQ(second_after.dirty_base_generation,
               second_before.dirty_base_generation);

    std::vector<std::byte> current_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current_second).ok());
    REQUIRE_EQ(payload_hash(current_second), payload_hash(second));
    REQUIRE(current_second == second);
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);
}

TEST_CASE(m2_1_snapshot_reader_observes_published_data_before_mutator_returns) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::PreimagePublished);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xD8))
                       .build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();

    const std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xBC});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xCD});
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::thread writer([&] {
        const auto result = store->write(
            bounded_context(), 0, replacement);
        writer_code.store(result.ok() ? 0 : error_code(result.error()),
                          std::memory_order_release);
    });
    REQUIRE(wait_until([&] { return barrier->reached(); }));

    myrd::SnapshotPreimage observed{};
    std::atomic<bool> reader_entered{};
    std::atomic<bool> reader_found{};
    std::thread reader([&] {
        reader_entered.store(true, std::memory_order_release);
        reader_found.store(
            myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
                *store, 0, observed),
            std::memory_order_release);
    });
    REQUIRE(wait_until([&] {
        return reader_entered.load(std::memory_order_acquire);
    }));
    barrier->release();
    reader.join();
    writer.join();

    REQUIRE_EQ(writer_code.load(std::memory_order_acquire), 0u);
    REQUIRE(reader_found.load(std::memory_order_acquire));
    REQUIRE_EQ(payload_hash(observed.data), payload_hash(reference));
    REQUIRE(observed.data == reference);
}

TEST_CASE(m2_1_resident_dirty_kind_mismatch_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, block_data(0xE9))
                       .build();
    auto store = require_store(backing_args(backing, 2, 7));
    const std::vector<std::byte> reference(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xDE});
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_kind(
        *store, 0, myrd::DirtyRecordKind::Zero));

    const std::vector<std::byte> replacement(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0xEF});
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(actual == reference);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
}

TEST_CASE(m2_1_resident_dirty_zero_epoch_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto reference = block_data(0x21);
    REQUIRE(myrd::DynamicBlockStoreTestAccess::seed_resident(
        *store,
        0,
        myrd::BlockState::ResidentDirty,
        reference,
        7));
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE_EQ(begun.value().cutoff_dirty_epoch, 0ull);

    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));
    REQUIRE_EQ(before.dirty_epoch, 0ull);
    const auto replacement = block_data(0x32);
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_resident_dirty_future_generation_fails_closed_before_cow) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 2, 7));
    const auto reference = block_data(0x43);
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());
    REQUIRE(myrd::DynamicBlockStoreTestAccess::set_dirty_snapshot_identity(
        *store,
        0,
        begun.value().cutoff_dirty_epoch,
        begun.value().source_backing_generation + 1));

    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));
    REQUIRE(before.dirty_base_generation >
            begun.value().source_backing_generation);
    const auto replacement = block_data(0x54);
    const auto rejected = store->write(
        bounded_context(), 0, replacement);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       FatalInternalInvariant));

    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
}

TEST_CASE(m2_1_cross_block_late_cow_failure_keeps_precise_prefix) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto store = require_store(backing_args(backing, 3, 7));
    const auto first_reference = block_data(0x65);
    const auto second_reference = block_data(0x76);
    REQUIRE(store->write(bounded_context(), 0, first_reference).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_reference).ok());
    myrd::BlockEntry first_cut{};
    myrd::BlockEntry second_cut{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_cut));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_cut));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> spanning(1024, std::byte{0x87});
    const auto rejected = store->write(
        bounded_context(), myrd::kBlockSizeBytes - 512, spanning);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::DynamicBlockStoreErrorCode::
                       SnapshotResourceExhausted));

    auto expected_first = as_vector(first_reference);
    std::copy_n(spanning.begin(), 512, expected_first.end() - 512);
    std::vector<std::byte> actual_first(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    std::vector<std::byte> actual_second(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual_first).ok());
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, actual_second).ok());
    REQUIRE_EQ(payload_hash(actual_first), payload_hash(expected_first));
    REQUIRE(actual_first == expected_first);
    REQUIRE_EQ(payload_hash(actual_second),
               payload_hash(second_reference));
    REQUIRE(std::equal(actual_second.begin(), actual_second.end(),
                       second_reference.begin()));

    myrd::BlockEntry first_after{};
    myrd::BlockEntry second_after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, first_after));
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, second_after));
    REQUIRE(first_after.version > first_cut.version);
    REQUIRE(first_after.dirty_epoch > first_cut.dirty_epoch);
    REQUIRE_EQ(second_after.state, second_cut.state);
    REQUIRE_EQ(second_after.version, second_cut.version);
    REQUIRE_EQ(second_after.dirty_epoch, second_cut.dirty_epoch);
    REQUIRE_EQ(second_after.dirty_base_generation,
               second_cut.dirty_base_generation);

    myrd::SnapshotPreimage first_preimage{};
    myrd::SnapshotPreimage unexpected_second{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, first_preimage));
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, unexpected_second));
    REQUIRE_EQ(first_preimage.version, first_cut.version);
    REQUIRE_EQ(first_preimage.dirty_epoch, first_cut.dirty_epoch);
    REQUIRE_EQ(first_preimage.base_generation,
               first_cut.dirty_base_generation);
    REQUIRE_EQ(payload_hash(first_preimage.data),
               payload_hash(first_reference));
    REQUIRE(std::equal(first_preimage.data.begin(), first_preimage.data.end(),
                       first_reference.begin()));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_active_snapshot_hard_limit_load_mutate_excludes_clean_backing) {
    const auto first_backing = block_data(0x98);
    const auto second_backing = block_data(0xA9);
    const auto third_backing = block_data(0xBA);
    auto backing = myrd::test::FakeBackingStoreBuilder(7)
                       .data_block(0, first_backing)
                       .data_block(1, second_backing)
                       .data_block(2, third_backing)
                       .build();
    auto args = backing_args(backing, 3, 7);
    args.options.soft_limit_bytes = myrd::kBlockSizeBytes;
    args.options.hard_limit_bytes = 2 * myrd::kBlockSizeBytes;
    args.options.eviction_low_watermark_bytes = myrd::kBlockSizeBytes;
    auto store = require_store(std::move(args));
    const auto first_resident = block_data(0xCB);
    const auto second_resident = block_data(0xDC);
    REQUIRE(store->write(bounded_context(), 0, first_resident).ok());
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, second_resident).ok());
    const auto begun = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(begun.ok());

    const std::vector<std::byte> patch(512, std::byte{0xED});
    const auto rejected = store->write(
        bounded_context(), 2 * myrd::kBlockSizeBytes + 512, patch);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(error_code(rejected.error()),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::memory_hard_limit));
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 2, after));
    REQUIRE_EQ(after.state, myrd::BlockState::Unresident);
    REQUIRE_EQ(after.version, 0ull);
    REQUIRE_EQ(after.dirty_epoch, 0ull);
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 2, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);

    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(
        bounded_context(), 2 * myrd::kBlockSizeBytes, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(third_backing));
    REQUIRE(std::equal(actual.begin(), actual.end(), third_backing.begin()));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_cancel_during_data_reservation_rolls_back_payload_and_write) {
    auto barrier = std::make_shared<SnapshotCoordinatorBarrier>(
        myrd::SnapshotCoordinatorBarrierEvent::ReservationInserted);
    myrd::DynamicBlockStoreTestDependencies dependencies;
    dependencies.snapshot_coordinator_non_reentrant_barrier = barrier;
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto created = myrd::DynamicBlockStore::create_for_testing(
        backing_args(backing, 2, 7), dependencies);
    REQUIRE(created.ok());
    auto store = created.move_value();
    const auto reference = block_data(0xDC);
    REQUIRE(store->write(bounded_context(), 0, reference).ok());
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());
    myrd::BlockEntry before{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, before));

    const auto replacement = block_data(0xED);
    std::stop_source stop;
    std::atomic<std::uint32_t> writer_code{kNoResult};
    std::thread writer([&] {
        const auto result = store->write(
            myrd::OperationContext{
                stop.get_token(),
                std::chrono::steady_clock::time_point::max()},
            0,
            replacement);
        writer_code.store(
            result.ok() ? 0 : error_code(result.error()),
            std::memory_order_release);
    });
    const bool reservation_reached =
        wait_until([&] { return barrier->reached(); });
    stop.request_stop();
    barrier->release();
    writer.join();

    REQUIRE(reservation_reached);
    REQUIRE_EQ(writer_code.load(std::memory_order_acquire),
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));
    myrd::BlockEntry after{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, after));
    REQUIRE_EQ(after.state, before.state);
    REQUIRE_EQ(after.version, before.version);
    REQUIRE_EQ(after.dirty_epoch, before.dirty_epoch);
    REQUIRE_EQ(after.dirty_base_generation,
               before.dirty_base_generation);
    std::vector<std::byte> actual(
        static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE(store->read(bounded_context(), 0, actual).ok());
    REQUIRE_EQ(payload_hash(actual), payload_hash(reference));
    REQUIRE(std::equal(actual.begin(), actual.end(), reference.begin()));
    myrd::SnapshotPreimage unexpected{};
    REQUIRE(!myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 0, unexpected));
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes, 0ull);
    REQUIRE_EQ(store->stats().snapshot_cow_records, 0ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_partial_final_block_cow_owns_zero_padded_full_payload) {
    auto backing = myrd::test::FakeBackingStoreBuilder(7).build();
    auto args = backing_args(backing, 2, 7);
    args.logical_size_bytes = myrd::kBlockSizeBytes + 512;
    auto store = require_store(std::move(args));
    const std::vector<std::byte> reference(512, std::byte{0x3E});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, reference).ok());
    myrd::BlockEntry cut_entry{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 1, cut_entry));
    REQUIRE(store->begin_consistent_snapshot(bounded_context()).ok());

    const std::vector<std::byte> replacement(512, std::byte{0x4F});
    REQUIRE(store->write(
        bounded_context(), myrd::kBlockSizeBytes, replacement).ok());
    myrd::SnapshotPreimage preimage{};
    REQUIRE(myrd::DynamicBlockStoreTestAccess::snapshot_preimage(
        *store, 1, preimage));
    REQUIRE_EQ(preimage.data.size(),
               static_cast<std::size_t>(myrd::kBlockSizeBytes));
    std::vector<std::byte> expected(
        static_cast<std::size_t>(myrd::kBlockSizeBytes), std::byte{0});
    std::copy(reference.begin(), reference.end(), expected.begin());
    REQUIRE_EQ(payload_hash(preimage.data), payload_hash(expected));
    REQUIRE(preimage.data == expected);
    REQUIRE(std::all_of(
        preimage.data.begin() + 512,
        preimage.data.end(),
        [](std::byte value) { return value == std::byte{0}; }));
    REQUIRE_EQ(preimage.version, cut_entry.version);
    REQUIRE_EQ(preimage.dirty_epoch, cut_entry.dirty_epoch);
    REQUIRE_EQ(preimage.base_generation,
               cut_entry.dirty_base_generation);
    std::vector<std::byte> current(512);
    REQUIRE(store->read(
        bounded_context(), myrd::kBlockSizeBytes, current).ok());
    REQUIRE(current == replacement);
    REQUIRE_EQ(store->stats().snapshot_cow_current_bytes,
               myrd::kMinimumSnapshotCowBytes);
}
