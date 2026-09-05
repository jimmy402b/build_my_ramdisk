#pragma once

#include "src/common/operation_context.h"
#include "src/host/host_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace myrd::m3 {
class M3Provider;
struct M3ProviderConfig;
struct M3CompactionBoundaryEvidence;
struct M3CompactionOutcome;
enum class M3ProviderState : std::uint8_t;
struct M3CheckpointOutcome;
struct M3StopOutcome;
} // namespace myrd::m3

namespace myrd::persistence {
enum class CommitDisposition : std::uint8_t;
enum class VolumeBarrierKind : std::uint8_t;
struct ImageGeometry;
struct PersistenceErrorContext;
} // namespace myrd::persistence

namespace myrd {
struct DynamicBlockStoreStats;
} // namespace myrd

namespace myrd::host {

enum class HostCoreError : std::uint32_t {
    invalid_argument = 0x4D01,
    binding_mismatch,
    mutation_busy,
    provider_unavailable,
    invalid_state,
    adapter_invariant,
    provider_exception,
};

enum class HostProviderState : std::uint8_t {
    constructed = 0,
    starting,
    ready,
    stopping,
    stopped,
    failed,
};

enum class HostCommitDisposition : std::uint8_t {
    not_committed = 0,
    commit_indeterminate,
    committed,
    committed_needs_reconciliation,
};

enum class HostVolumeBarrierKind : std::uint8_t {
    windows_volume_flushed = 0,
    raw_block_test_no_filesystem_cache,
};

enum class HostSnapshotState : std::uint8_t {
    active = 0,
    aborted,
    fully_scanned,
};

struct HostGeometry final {
    std::uint64_t virtual_bytes{};
    std::uint32_t logical_sector_size{};
    std::uint32_t physical_sector_size{};

    bool operator==(const HostGeometry&) const noexcept = default;
};

struct HostPersistenceErrorContext final {
    std::string operation;
    std::uint32_t code{};
    std::uint8_t phase{};
    std::uint32_t native_code{};
    std::uint64_t operation_id{};
    std::array<std::byte, 16> image_uuid{};
    std::uint64_t generation{};
    HostCommitDisposition disposition{HostCommitDisposition::not_committed};
    std::string safe_context;

    bool operator==(const HostPersistenceErrorContext&) const noexcept =
        default;
};

struct HostStorageStats final {
    std::uint64_t logical_size_bytes{};
    std::uint64_t reserved_bytes{};
    std::uint64_t committed_bytes{};
    std::uint64_t resident_clean_blocks{};
    std::uint64_t resident_dirty_blocks{};
    std::uint64_t zero_clean_blocks{};
    std::uint64_t zero_dirty_blocks{};
    std::uint64_t loading_blocks{};
    std::uint64_t evicted_blocks_total{};
    std::uint64_t backing_reads_total{};
    std::uint64_t backing_read_bytes_total{};
    std::uint64_t uncached_reads_total{};
    std::uint64_t unmap_bytes_total{};
    std::uint64_t zero_bytes_total{};
    std::uint64_t hard_limit_rejections{};
    std::uint64_t commit_failures{};
    std::uint64_t decommit_failures{};
    std::uint64_t synchronous_pressure_passes_total{};
    std::uint64_t synchronous_pressure_waits_total{};
    std::uint64_t synchronous_pressure_no_progress_total{};
    std::uint64_t synchronous_pressure_cancellations_total{};
    std::uint64_t synchronous_pressure_deadlines_total{};
    std::uint64_t commit_reservation_rollbacks_total{};
    std::uint64_t snapshot_records_total{};
    std::uint64_t snapshot_version_conflicts{};
    std::uint64_t backing_generation{};
    std::uint64_t backing_view_installs_total{};
    std::uint64_t backing_generation_conflicts{};
    std::uint64_t stale_backing_load_retries{};
    std::uint64_t snapshot_active_id{};
    std::optional<HostSnapshotState> snapshot_active_state;
    std::uint64_t snapshot_cow_current_bytes{};
    std::uint64_t snapshot_cow_peak_bytes{};
    std::uint64_t snapshot_cow_records{};
    std::uint64_t snapshot_blocked_writers{};
    std::uint64_t snapshot_writer_stalls_total{};
    std::uint64_t snapshot_auto_aborts_total{};
    std::uint64_t snapshot_consistent_batches_total{};
    std::uint64_t snapshot_consistent_records_total{};

    bool operator==(const HostStorageStats&) const noexcept = default;
};

struct HostCheckpointOutcome final {
    HostCommitDisposition disposition{HostCommitDisposition::not_committed};
    std::uint64_t generation{};
    std::optional<Error> error;
    std::optional<HostPersistenceErrorContext> error_context;

    [[nodiscard]] bool ok() const noexcept {
        return disposition == HostCommitDisposition::committed
            && !error.has_value() && !error_context.has_value();
    }
};

struct HostCompactionRequest final {
    HostVolumeBarrierKind barrier{
        static_cast<HostVolumeBarrierKind>(0xff)};
    std::uint64_t native_operation_id{};
};

struct HostCompactionBoundaryEvidence final {
    std::uint64_t native_operation_id{};
    std::array<std::byte, 16> image_uuid{};
    std::uint64_t source_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t source_base_bytes{};
    std::uint64_t source_journal_bytes{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    std::uint64_t cut_dirty_epoch{};
    bool cut_dirty_epoch_known{};

    bool operator==(const HostCompactionBoundaryEvidence&) const noexcept =
        default;
};

struct HostCompactionBoundaryDecision final {
    bool continue_compaction{};
    std::uint64_t durable_operation_revision{};
    std::uint64_t durable_profile_revision{};
    HostCompactionBoundaryEvidence evidence{};
};

using HostCompactionBoundaryHandler = std::function<
    Result<HostCompactionBoundaryDecision>(
        const HostCompactionBoundaryEvidence&)>;
using HostCompactionCutHandler = HostCompactionBoundaryHandler;

struct HostCompactionMetrics final {
    std::uint64_t source_generation{};
    std::uint64_t committed_generation{};
    std::uint64_t source_base_bytes{};
    std::uint64_t source_journal_bytes{};
    std::uint64_t candidate_base_bytes{};
    std::uint64_t candidate_journal_bytes{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    std::uint64_t live_extent_count{};
    std::uint64_t candidate_payloads{};
    std::uint64_t snapshot_records{};
    std::uint64_t data_records{};
    std::uint64_t zero_records{};
    std::uint64_t discard_records{};
    std::uint64_t index_nodes{};
    std::uint64_t newer_dirty_preserved{};
    std::uint64_t retained_candidate_count{};
    std::uint64_t deleted_candidate_count{};

    bool operator==(const HostCompactionMetrics&) const noexcept = default;
};

struct HostCompactionOutcome final {
    HostCommitDisposition disposition{HostCommitDisposition::not_committed};
    std::uint64_t generation{};
    HostCompactionBoundaryEvidence boundary{};
    HostCompactionMetrics metrics{};
    std::optional<Error> error;
    std::optional<HostPersistenceErrorContext> error_context;

    [[nodiscard]] bool ok() const noexcept {
        return disposition == HostCommitDisposition::committed &&
            generation != 0 && !error.has_value() &&
            !error_context.has_value();
    }
};

struct HostStopRequest final {
    bool checkpoint{};
    std::optional<HostVolumeBarrierKind> barrier;
};

struct HostStopOutcome final {
    HostCommitDisposition disposition{HostCommitDisposition::not_committed};
    std::uint64_t generation{};
    bool transport_stopped{};
    bool checkpoint_completed{};
    bool store_closed{};
    bool session_closed{};
    bool writer_closed{};
    std::optional<Error> error;
    std::optional<HostPersistenceErrorContext> error_context;

    [[nodiscard]] bool ok() const noexcept {
        return !error.has_value() && !error_context.has_value()
            && transport_stopped && store_closed && session_closed
            && writer_closed
            && (!checkpoint_completed
                || disposition == HostCommitDisposition::committed);
    }
};

struct HostStartOutcome final {
    bool provider_opened{};
    bool provider_started{};
    std::uint64_t generation{};
    HostGeometry geometry{};
    std::optional<Error> error;
    std::optional<HostStopOutcome> cleanup;

    [[nodiscard]] bool ok() const noexcept {
        return provider_opened && provider_started && !error.has_value();
    }
};

struct HostCommand final {
    WorkerBinding binding;
    product::Uuid operation_id;
};

class IHostProvider {
public:
    virtual ~IHostProvider() = default;
    virtual Result<void> start(const OperationContext& context) = 0;
    virtual HostCheckpointOutcome checkpoint(
        const OperationContext& context,
        HostVolumeBarrierKind barrier) = 0;
    virtual HostCompactionOutcome compact(
        const OperationContext& context,
        HostCompactionRequest request,
        const HostCompactionBoundaryHandler& boundary,
        const HostCompactionCutHandler& cut);
    virtual HostCompactionOutcome reconcile_compaction(
        const OperationContext& context,
        const HostCompactionBoundaryEvidence& boundary);
    virtual HostStopOutcome stop(
        const OperationContext& context,
        HostStopRequest request) = 0;
    virtual Result<HostProviderState> state() const = 0;
    virtual Result<std::uint64_t> generation() const = 0;
    virtual Result<HostGeometry> geometry() const = 0;
    virtual Result<HostStorageStats> storage_stats() const = 0;
};

class IHostProviderFactory {
public:
    virtual ~IHostProviderFactory() = default;
    virtual Result<std::shared_ptr<IHostProvider>> open(
        const OperationContext& context) = 0;
};

class MutationCompletion;

class HostCore final {
public:
    static Result<std::unique_ptr<HostCore>> create(
        WorkerBinding expected_binding,
        std::shared_ptr<IHostProviderFactory> provider_factory);

    // Adopt the binding proven by an authenticated endpoint rotation for this
    // same live worker. Identity is immutable; only the session generation
    // may move forward, and never while a mutation is active.
    Result<void> adopt_reattached_binding(const WorkerBinding& binding);

    HostCore(const HostCore&) = delete;
    HostCore& operator=(const HostCore&) = delete;

    HostStartOutcome start(
        const HostCommand& command,
        const OperationContext& context);
    HostCheckpointOutcome checkpoint(
        const HostCommand& command,
        const OperationContext& context,
        HostVolumeBarrierKind barrier);
    HostCompactionOutcome compact(
        const HostCommand& command,
        const OperationContext& context,
        HostCompactionRequest request,
        const HostCompactionBoundaryHandler& boundary,
        const HostCompactionCutHandler& cut);
    HostCompactionOutcome reconcile_compaction(
        const HostCommand& command,
        const OperationContext& context,
        const HostCompactionBoundaryEvidence& boundary);
    HostStopOutcome stop(
        const HostCommand& command,
        const OperationContext& context,
        HostStopRequest request);
    Result<HostStorageStats> storage_stats(
        const WorkerBinding& binding) const;
    Result<HostProviderState> provider_state(
        const WorkerBinding& binding) const;

private:
    friend class MutationCompletion;

    HostCore(
        WorkerBinding expected_binding,
        std::shared_ptr<IHostProviderFactory> provider_factory) noexcept;

    Result<void> begin_mutation(const HostCommand& command);
    Result<std::shared_ptr<IHostProvider>> current_provider(
        const WorkerBinding& binding) const;
    void end_mutation() noexcept;

    WorkerBinding expected_binding_;
    std::shared_ptr<IHostProviderFactory> provider_factory_;
    mutable std::mutex mutex_;
    std::shared_ptr<IHostProvider> provider_;
    std::optional<product::Uuid> active_mutation_;
};

std::shared_ptr<IHostProvider> adapt_m3_provider(
    std::unique_ptr<m3::M3Provider> provider);
std::shared_ptr<IHostProviderFactory> make_m3_host_provider_factory(
    m3::M3ProviderConfig config);

namespace m3_adapter {

Result<HostProviderState> map_state(m3::M3ProviderState state);
Result<HostCommitDisposition> map_disposition(
    persistence::CommitDisposition disposition);
Result<HostVolumeBarrierKind> map_barrier(
    persistence::VolumeBarrierKind barrier);
Result<persistence::VolumeBarrierKind> map_barrier(
    HostVolumeBarrierKind barrier);
Result<HostGeometry> map_geometry(
    const persistence::ImageGeometry& geometry);
Result<HostPersistenceErrorContext> map_error_context(
    const persistence::PersistenceErrorContext& context);
Result<HostCheckpointOutcome> map_checkpoint_outcome(
    const m3::M3CheckpointOutcome& outcome);
Result<HostCompactionBoundaryEvidence> map_compaction_boundary(
    const m3::M3CompactionBoundaryEvidence& evidence);
Result<m3::M3CompactionBoundaryEvidence> map_compaction_boundary(
    const HostCompactionBoundaryEvidence& evidence);
Result<HostCompactionOutcome> map_compaction_outcome(
    const m3::M3CompactionOutcome& outcome);
Result<HostStopOutcome> map_stop_outcome(
    const m3::M3StopOutcome& outcome);
Result<HostStorageStats> map_storage_stats(
    const DynamicBlockStoreStats& stats);

} // namespace m3_adapter

} // namespace myrd::host
