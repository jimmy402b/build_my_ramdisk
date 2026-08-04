#pragma once

#include "backing_store.h"
#include "block_device.h"
#include "consistent_snapshot.h"
#include "dynamic_block_types.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd {

class BlockDirectory;
class EvictionManager;
class LockStripeSet;
class VirtualMemoryArena;
struct DynamicBlockStoreTestDependencies;
class IDynamicBlockStoreLifecycleObserver;
class IDynamicBlockStoreReadObserver;
class DynamicBlockStoreTestAccess;

struct BackingView {
    BackingGeneration generation{};
    std::shared_ptr<const IBackingStore> store;
};

class DynamicBlockStore final : public IBlockDevice {
public:
    static Result<std::unique_ptr<DynamicBlockStore>> create(
        DynamicBlockStoreCreateArgs args);

    // A single narrow construction seam for deterministic resource-failure and
    // lifecycle tests. Product callers use create().
    static Result<std::unique_ptr<DynamicBlockStore>> create_for_testing(
        DynamicBlockStoreCreateArgs args,
        DynamicBlockStoreTestDependencies dependencies);

    ~DynamicBlockStore() noexcept override;

    DynamicBlockStore(const DynamicBlockStore&) = delete;
    DynamicBlockStore& operator=(const DynamicBlockStore&) = delete;
    DynamicBlockStore(DynamicBlockStore&&) = delete;
    DynamicBlockStore& operator=(DynamicBlockStore&&) = delete;

    std::uint64_t size_bytes() const noexcept override;
    std::uint32_t sector_size() const noexcept override;
    DeviceFeatures features() const noexcept override;

    Result<void> read(const OperationContext& context,
        std::uint64_t offset, std::span<std::byte> output) override;
    Result<void> write(const OperationContext& context,
        std::uint64_t offset, std::span<const std::byte> input) override;
    Result<void> unmap(const OperationContext& context,
        std::span<const DataRange> ranges) override;
    Result<void> zero(const OperationContext& context,
        std::span<const DataRange> ranges) override;

    BackingGeneration backing_generation() const;
    Result<void> install_backing_view(BackingView next_view);

    Result<std::vector<DirtyBlockSnapshot>> copy_dirty_batch(
        const OperationContext& context,
        DirtyScanCursor& cursor,
        std::size_t max_bytes);
    Result<void> mark_persisted(
        std::uint64_t block_index,
        std::uint64_t expected_version,
        DirtyRecordKind expected_kind,
        BackingGeneration persisted_generation);

    // ---- M2.1 consistent snapshot API --------------------------------------
    Result<ConsistentSnapshotToken> begin_consistent_snapshot(
        const OperationContext& context);

    Result<std::vector<DirtyBlockSnapshot>>
    copy_consistent_snapshot_batch(
        const OperationContext& context,
        const ConsistentSnapshotToken& token,
        DirtyScanCursor& cursor,
        std::size_t max_bytes);

    Result<void> finish_consistent_snapshot(
        const ConsistentSnapshotToken& token);

    Result<void> abort_consistent_snapshot(
        const ConsistentSnapshotToken& token);

    Result<ConsistentSnapshotStatus> consistent_snapshot_status(
        const ConsistentSnapshotToken& token) const;

    const BlockGeometry& geometry() const noexcept;
    const DynamicBlockStoreOptions& options() const noexcept;
    DynamicBlockStoreStats stats() const;
    std::uint64_t reservation_size_bytes() const noexcept;
    std::uint64_t total_blocks() const noexcept;
    std::uint64_t last_block_size_bytes() const noexcept;

    void close();
    bool is_closed() const noexcept;
    Result<void> validate_invariants_for_test();

    // Read-only diagnostics used by deterministic tests and later stress gates.
    BackingView backing_view_for_test() const;
    std::uint64_t allocated_leaf_count_for_test() const noexcept;
    InitialBlockState implicit_initial_state_for_test() const noexcept;
    BackingGeneration implicit_initial_generation_for_test() const noexcept;
    bool worker_running_for_test() const;
    Result<void> wait_for_close_for_test(std::uint64_t block_index);
    std::uint64_t stripe_waiter_count_for_test() const noexcept;
    std::uint64_t backing_install_waiter_count_for_test() const noexcept;

private:
    friend class EvictionManager;
    friend class DynamicBlockStoreTestAccess;

    struct RequestToken {
        DynamicBlockStore* store{};
        RequestToken* previous{};
    };

    enum class LifecycleState : std::uint8_t {
        Open = 0,
        Closing = 1,
        Closed = 2,
    };

    enum class ReadMaterializationPolicy : std::uint8_t {
        AllowReadThrough = 0,
        ApplyMutation = 1,
    };

    enum class CommitBudgetOutcome : std::uint8_t {
        Reserved = 0,
        NoCapacity = 1,
    };

    DynamicBlockStore(
        DynamicBlockStoreCreateArgs args,
        BlockGeometry geometry,
        BackingView backing_view,
        std::unique_ptr<VirtualMemoryArena> arena,
        std::unique_ptr<LockStripeSet> stripes,
        std::unique_ptr<BlockDirectory> directory,
        std::shared_ptr<IDynamicBlockStoreLifecycleObserver>
            lifecycle_observer,
        std::shared_ptr<IDynamicBlockStoreReadObserver>
            read_observer,
        bool fail_snapshot_allocation,
        bool pause_before_snapshot_wait,
        bool pause_after_snapshot_pin,
        bool pause_after_persisted_validation) noexcept;

    static Result<std::unique_ptr<DynamicBlockStore>> create_impl(
        DynamicBlockStoreCreateArgs args,
        DynamicBlockStoreTestDependencies dependencies);

    bool start_worker();
    void worker_main() noexcept;
    bool signal_eviction_worker_if_needed();
    // Must be called without a block stripe. A failed immediate reservation
    // may synchronously run one bounded, cancellation-aware clean reclaim.
    Result<CommitBudgetOutcome> reserve_commit_budget(
        const OperationContext& context);
    void release_commit_reservation(bool rollback) noexcept;
    bool try_enter_request(RequestToken& token);
    void leave_request(RequestToken& token) noexcept;
    bool current_thread_has_active_request() const noexcept;
    void finish_close();
    bool closing_requested() const noexcept;
    void notify_lifecycle_event(std::uint8_t event) noexcept;
    void notify_all_waiters();
    Result<void> read_one_segment(
        const OperationContext& context,
        std::uint64_t block_index,
        std::uint64_t block_offset_bytes,
        std::span<std::byte> destination,
        ReadMaterializationPolicy materialization_policy,
        std::uint64_t mutation_offset_bytes,
        std::span<const std::byte> mutation_source,
        bool* mutation_applied);
    Result<void> write_one_segment(
        const OperationContext& context,
        std::uint64_t block_index,
        std::uint64_t block_offset_bytes,
        std::span<const std::byte> source,
        bool explicit_zero_is_noop);
    Result<void> publish_full_zero_segment(
        const OperationContext& context,
        std::uint64_t block_index,
        DirtyRecordKind kind);
    Result<void> apply_zero_ranges(
        const OperationContext& context,
        std::span<const DataRange> ranges,
        DirtyRecordKind kind);

    BlockGeometry geometry_{};
    DynamicBlockStoreOptions options_{};
    std::uint32_t sector_size_bytes_{};

    mutable std::mutex state_mutex_;
    std::condition_variable backing_view_condition_;
    BackingView backing_view_{};
    DynamicBlockStoreStats stats_{};
    // Correctness-owned commit accounting. stats_.committed_bytes is only
    // its observable mirror and is never consulted for admission decisions.
    std::uint64_t committed_bytes_{};
    std::uint64_t commit_reservation_bytes_{};
    std::uint64_t backing_publications_in_progress_{};

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    LifecycleState lifecycle_state_{LifecycleState::Open};
    std::atomic<bool> closing_requested_{false};
    std::stop_source close_stop_source_{};
    std::uint64_t active_requests_{};
    bool worker_running_{};
    bool eviction_requested_{};
    bool eviction_worker_waiting_{};
    std::uint64_t eviction_worker_passes_{};
    std::uint64_t eviction_worker_wakeups_{};
    bool deferred_close_ready_{};
    std::thread::id close_owner_{};
    std::thread worker_;
    std::atomic<std::uint64_t> stripe_waiter_count_{0};
    std::atomic<std::uint64_t> backing_install_waiter_count_{0};
    std::atomic<std::uint64_t> access_epoch_{0};
    std::atomic<std::uint64_t> dirty_epoch_{0};
    std::atomic<std::uint64_t> transition_epoch_{0};

    std::unique_ptr<VirtualMemoryArena> arena_;
    std::unique_ptr<LockStripeSet> stripes_;
    std::unique_ptr<BlockDirectory> directory_;
    std::unique_ptr<EvictionManager> eviction_manager_;
    std::shared_ptr<IDynamicBlockStoreLifecycleObserver> lifecycle_observer_;
    std::shared_ptr<IDynamicBlockStoreReadObserver> read_observer_;
    bool fail_snapshot_allocation_{};
    std::atomic<bool> snapshot_wait_pause_for_test_{};
    std::atomic<bool> snapshot_wait_gap_reached_for_test_{};
    std::atomic<bool> snapshot_pin_pause_for_test_{};
    std::atomic<bool> snapshot_pin_gap_reached_for_test_{};
    std::atomic<bool> persisted_ack_pause_for_test_{};
    std::atomic<bool> persisted_ack_gap_reached_for_test_{};

    static thread_local RequestToken* current_request_token_;
};

} // namespace myrd
