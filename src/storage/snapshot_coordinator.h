#pragma once

#include "dynamic_block_types.h"
#include "src/common/operation_context.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace myrd {

struct SnapshotPreimage {
    std::uint64_t block_index{};
    std::uint64_t version{};
    DirtyRecordKind kind{DirtyRecordKind::Data};
    BackingGeneration base_generation{};
    std::uint64_t dirty_epoch{};
    std::vector<std::byte> data;
};

enum class SnapshotPreimageDisposition : std::uint8_t {
    NotMember = 0,
    Consumed = 1,
    AlreadyPreserved = 2,
    Reserved = 3,
};

enum class SnapshotCoordinatorBarrierEvent : std::uint8_t {
    ActivateLocked = 0,
    ReservationInserted = 1,
    PreimagePublished = 2,
    PreimagePublishBeforeFinalContextCheck = 3,
};

// Test-only deterministic barrier. It is invoked while the coordinator mutex
// is held, so implementations must only signal/wait and must never call back
// into SnapshotCoordinator or DynamicBlockStore.
class ISnapshotCoordinatorNonReentrantBarrier {
public:
    virtual ~ISnapshotCoordinatorNonReentrantBarrier() = default;
    virtual void pause_while_coordinator_locked(
        SnapshotCoordinatorBarrierEvent event) noexcept = 0;
};

enum class SnapshotPressureTestEvent : std::uint8_t {
    CapacityUnavailable = 0,
    CapacityWaitReady = 1,
    CapacityWaitSleeping = 2,
    PreparingPublished = 3,
    PreparingWaitReady = 4,
    PreparingMarkerWaitReady = 5,
    PreparingFillActive = 6,
    CapacityGrantReady = 7,
};

class ISnapshotPressureClock {
public:
    virtual ~ISnapshotPressureClock() = default;
    virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

// Test-only observer. Unlike the non-reentrant coordinator barrier, callbacks
// on this interface must be made without coordinator/state/lifecycle locks.
// CapacityWaitReady and all Preparing events must additionally be emitted
// after releasing the block stripe so deterministic tests may pause safely.
class ISnapshotPressureTestObserver {
public:
    virtual ~ISnapshotPressureTestObserver() = default;
    virtual void on_snapshot_pressure_event(
        SnapshotPressureTestEvent event,
        std::uint64_t block_index) noexcept = 0;
};

class SnapshotCoordinator;
class SnapshotCoordinatorState;

class SnapshotMetadataReservation final {
public:
    SnapshotMetadataReservation() noexcept = default;
    ~SnapshotMetadataReservation() noexcept;

    SnapshotMetadataReservation(const SnapshotMetadataReservation&) = delete;
    SnapshotMetadataReservation& operator=(
        const SnapshotMetadataReservation&) = delete;
    SnapshotMetadataReservation(SnapshotMetadataReservation&& other) noexcept;
    SnapshotMetadataReservation& operator=(
        SnapshotMetadataReservation&& other) noexcept;

    SnapshotPreimageDisposition disposition() const noexcept {
        return disposition_;
    }

    std::uint64_t snapshot_id() const noexcept { return snapshot_id_; }
    std::uint64_t reservation_id() const noexcept { return reservation_id_; }

private:
    friend class SnapshotCoordinator;
    friend class SnapshotCoordinatorState;

    SnapshotMetadataReservation(
        SnapshotPreimageDisposition disposition,
        std::weak_ptr<SnapshotCoordinatorState> owner_state,
        std::uint64_t snapshot_id,
        std::uint64_t block_index,
        std::uint64_t reservation_id) noexcept;

    void reset() noexcept;

    SnapshotPreimageDisposition disposition_{
        SnapshotPreimageDisposition::NotMember};
    std::weak_ptr<SnapshotCoordinatorState> owner_state_;
    std::uint64_t snapshot_id_{};
    std::uint64_t block_index_{};
    std::uint64_t reservation_id_{};
};

struct SnapshotCoordinatorSnapshot {
    std::optional<ConsistentSnapshotToken> active_token;
    std::optional<ConsistentSnapshotState> active_state;
    std::optional<DynamicBlockStoreErrorCode> abort_reason;
    std::uint64_t next_scan_block{};
    std::uint64_t cow_current_bytes{};
    std::uint64_t cow_peak_bytes{};
    std::uint64_t cow_records{};
    std::uint64_t pending_reservations{};
    std::uint64_t capacity_waiters{};
    std::uint64_t writer_stalls_total{};
    std::uint64_t auto_aborts_total{};
};

// Lock order is lifecycle/admission -> at most one block stripe -> this
// coordinator -> store state. SnapshotCoordinator has no stripe reference and
// no method may acquire a stripe.
class SnapshotCoordinator final {
public:
    explicit SnapshotCoordinator(
        std::uint64_t max_bytes,
        std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
            non_reentrant_barrier = {},
        std::shared_ptr<ISnapshotPressureClock> pressure_clock = {});
    ~SnapshotCoordinator() noexcept;

    SnapshotCoordinator(const SnapshotCoordinator&) = delete;
    SnapshotCoordinator& operator=(const SnapshotCoordinator&) = delete;
    SnapshotCoordinator(SnapshotCoordinator&&) = delete;
    SnapshotCoordinator& operator=(SnapshotCoordinator&&) = delete;

    Result<ConsistentSnapshotToken> activate(
        BackingGeneration source_backing_generation,
        std::uint64_t cutoff_dirty_epoch);

    Result<SnapshotMetadataReservation> reserve_metadata_before_mutation(
        const OperationContext& context,
        const ConsistentSnapshotToken& token,
        const SnapshotPreimage& metadata,
        std::uint64_t capacity_grant_ticket = 0);

    Result<SnapshotMetadataReservation> reserve_data_before_mutation(
        const OperationContext& context,
        const ConsistentSnapshotToken& token,
        const SnapshotPreimage& metadata,
        std::span<const std::byte> payload,
        std::uint64_t capacity_grant_ticket = 0);

    Result<void> publish_metadata_preimage(
        const OperationContext& context,
        SnapshotMetadataReservation& reservation);

    Result<void> fill_data_preimage(
        const OperationContext& context,
        SnapshotMetadataReservation& reservation,
        std::span<const std::byte> payload,
        ISnapshotPressureTestObserver* test_observer);

    Result<void> wait_for_preparing_resolution(
        const OperationContext& context,
        std::uint64_t snapshot_id,
        std::uint64_t block_index,
        std::uint64_t reservation_id,
        ISnapshotPressureTestObserver* test_observer);

    void rollback_metadata_preimage(
        SnapshotMetadataReservation& reservation) noexcept;

    Result<void> wait_for_capacity_change(
        const OperationContext& context,
        const ConsistentSnapshotToken& token,
        std::uint64_t required_bytes,
        std::uint64_t block_index,
        std::chrono::milliseconds stall_limit,
        ISnapshotPressureTestObserver* test_observer,
        std::uint64_t& capacity_grant_ticket);
    void release_capacity_grant(
        std::uint64_t capacity_grant_ticket) noexcept;

    std::optional<SnapshotPreimage> peek_preimage(
        std::uint64_t block_index) const;
    std::optional<SnapshotPreimage> peek_preimage_metadata(
        std::uint64_t block_index) const;

    Result<void> advance_scan_frontier(
        const ConsistentSnapshotToken& token,
        std::uint64_t expected_scan_block,
        std::uint64_t next_scan_block,
        std::uint64_t total_blocks);

    Result<void> finish(
        const ConsistentSnapshotToken& token,
        std::uint64_t total_blocks);
    Result<void> abort(const ConsistentSnapshotToken& token);
    void request_close() noexcept;
    void finalize_close() noexcept;
    Result<ConsistentSnapshotStatus> status(
        const ConsistentSnapshotToken& token) const;
    SnapshotCoordinatorSnapshot snapshot() const;
    bool owns_pending_reservation(
        std::uint64_t snapshot_id,
        std::uint64_t block_index,
        std::uint64_t reservation_id) const;
    Result<void> validate_invariants_for_test() const;
    std::uint64_t index_probe_count_for_test() const;
    bool preimage_lookup_entered_for_test() const noexcept;
    void notify_capacity_waiters_for_test() noexcept;

private:
    Result<SnapshotMetadataReservation> reserve_before_mutation(
        const OperationContext& context,
        const ConsistentSnapshotToken& token,
        const SnapshotPreimage& metadata,
        std::span<const std::byte> payload,
        bool data_preimage,
        std::uint64_t capacity_grant_ticket);

    std::shared_ptr<SnapshotCoordinatorState> state_;
};

} // namespace myrd
