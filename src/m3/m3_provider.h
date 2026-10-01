#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_session.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/retention_manager.h"
#include "src/persistence/writer_lock.h"
#include "src/proxy/object_names.h"
#include "src/proxy/shm_proxy_session.h"
#include "src/storage/dynamic_block_store.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace myrd::m3 {

enum class M3ProviderState : std::uint8_t {
    Constructed = 0,
    Starting,
    Ready,
    Stopping,
    Stopped,
    Failed,
};

struct M3ProviderConfig final {
    std::filesystem::path bundle_root;
    std::string manifest_name;
    persistence::RecoveryOptions recovery;
    DynamicBlockStoreOptions store;
    std::chrono::milliseconds request_timeout{30000};
};

class IM3ProviderTransport {
public:
    virtual ~IM3ProviderTransport() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual proxy::SessionSnapshot snapshot() const = 0;
    virtual proxy::ProxyObjectNames names() const = 0;
};

struct M3ProviderFactories final {
    using TransportFactory = std::function<
        Result<std::unique_ptr<IM3ProviderTransport>>(
            IBlockDevice&,
            const proxy::IProxyNamespacePolicy&,
            proxy::ShmProxySessionOptions)>;
    TransportFactory transport{};
    // Deterministic pre-snapshot observer available only through
    // open_for_test(); product open() never supplies composition seams.
    std::function<Result<void>()> before_checkpoint_prepare{};
    // Deterministic reconciliation observer. The coordinator converts a
    // failure into CommittedNeedsReconciliation so provider retry ownership
    // can be verified without changing persistence publication behavior.
    std::function<Result<void>()> before_checkpoint_reconcile{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::function<Result<void>(
        const persistence::JournalWriterPhaseEvent&)> journal_phase_hook{};
    persistence::ManifestPublishTestPhaseHook manifest_phase_hook{};
    persistence::CheckpointReconcileTestHook reconcile_phase_hook{};
#endif
};

struct M3CheckpointOutcome final {
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
    std::uint64_t generation{};
    std::optional<Error> error{};
    std::optional<persistence::PersistenceErrorContext> error_context{};

    [[nodiscard]] bool ok() const noexcept {
        return disposition == persistence::CommitDisposition::Committed &&
               !error.has_value() && !error_context.has_value();
    }
};

// M5 two-phase checkpoint boundary.
//
// The M4 one-shot checkpoint() keeps its single-call shape and its native id
// (checkpoint_generation + 3902).  These three calls expose the same
// prepare/publish machinery as explicit steps so the M5 service can publish its
// own durable commit boundary between prepare and commit, which is what makes
// the boundary evidence meaningful across a worker restart.
//
// The candidate is frozen during prepare: the manifest slot and the SHA-256 of
// the exact candidate-slot bytes are returned, and commit refuses a request that
// does not match them.  A zero native id is refused before any state changes,
// because zero is the "no operation" encoding and cannot identify a prepare.
struct M3CheckpointPrepareRequest final {
    OperationContext context;
    persistence::VolumeBarrierKind barrier{
        static_cast<persistence::VolumeBarrierKind>(0xff)};
    std::uint64_t native_operation_id{};
};

struct M3CheckpointPrepared final {
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};
};

struct M3CheckpointCommitRequest final {
    // The commit is the same operation as the prepare, so it carries the
    // caller's live operation context as well as the frozen candidate identity.
    OperationContext context;
    std::uint64_t native_operation_id{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};
};

struct M3CompactionRequest final {
    persistence::VolumeBarrierKind barrier{
        static_cast<persistence::VolumeBarrierKind>(0xff)};
    std::uint64_t operation_id{};
};

struct M3CompactionBoundaryEvidence final {
    std::uint64_t operation_id{};
    persistence::ImageUuid image_uuid{};
    std::uint64_t source_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t source_base_bytes{};
    std::uint64_t source_journal_bytes{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    std::uint64_t cut_dirty_epoch{};
    bool cut_dirty_epoch_known{};

    bool operator==(const M3CompactionBoundaryEvidence&) const noexcept =
        default;
};

struct M3CompactionBoundaryDecision final {
    bool continue_compaction{};
    std::uint64_t durable_operation_revision{};
    std::uint64_t durable_profile_revision{};
    M3CompactionBoundaryEvidence evidence{};
};

using M3CompactionBoundaryHandler = std::function<
    Result<M3CompactionBoundaryDecision>(
        const M3CompactionBoundaryEvidence&)>;
using M3CompactionCutHandler = M3CompactionBoundaryHandler;

struct M3CompactionMetrics final {
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

    bool operator==(const M3CompactionMetrics&) const noexcept = default;
};

struct M3CompactionOutcome final {
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
    std::uint64_t generation{};
    M3CompactionBoundaryEvidence boundary{};
    M3CompactionMetrics metrics{};
    std::optional<Error> error{};
    std::optional<persistence::PersistenceErrorContext> error_context{};

    [[nodiscard]] bool ok() const noexcept {
        return disposition == persistence::CommitDisposition::Committed &&
               generation != persistence::kInvalidGeneration &&
               !error.has_value() && !error_context.has_value();
    }
};

struct M3StopRequest final {
    bool checkpoint{};
    std::optional<persistence::VolumeBarrierKind> barrier{};
};

struct M3StopOutcome final {
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
    std::uint64_t generation{};
    bool transport_stopped{};
    bool checkpoint_completed{};
    bool store_closed{};
    bool session_closed{};
    bool writer_closed{};
    std::optional<Error> error{};
    std::optional<persistence::PersistenceErrorContext> error_context{};

    [[nodiscard]] bool ok() const noexcept {
        return !error.has_value() && !error_context.has_value() &&
               transport_stopped && store_closed &&
               session_closed && writer_closed &&
               (!checkpoint_completed ||
                disposition == persistence::CommitDisposition::Committed);
    }
};

class M3Provider final {
public:
    static Result<std::unique_ptr<M3Provider>> open(
        const OperationContext& context,
        M3ProviderConfig config);
    static Result<std::unique_ptr<M3Provider>> open_for_test(
        const OperationContext& context,
        const proxy::IProxyNamespacePolicy& namespace_policy,
        M3ProviderConfig config,
        M3ProviderFactories factories = {});

    ~M3Provider() noexcept;
    M3Provider(const M3Provider&) = delete;
    M3Provider& operator=(const M3Provider&) = delete;

    Result<void> start(const OperationContext& context);
    M3CheckpointOutcome checkpoint(
        const OperationContext& context,
        persistence::VolumeBarrierKind barrier);
    // M5 two-phase boundary. prepare holds an unpublished transaction and
    // returns the frozen candidate; abort discards it; commit publishes exactly
    // that candidate and then reconciles it. Only one two-phase checkpoint may
    // be outstanding, and it must be resolved before the provider stops.
    Result<M3CheckpointPrepared> prepare_checkpoint(
        const M3CheckpointPrepareRequest& request);
    M3CheckpointOutcome commit_checkpoint(
        const M3CheckpointCommitRequest& request);
    Result<void> abort_checkpoint(std::uint64_t native_operation_id);
    M3CompactionOutcome compact(
        const OperationContext& context,
        M3CompactionRequest request,
        const M3CompactionBoundaryHandler& boundary,
        const M3CompactionCutHandler& cut);
    M3CompactionOutcome reconcile_compaction(
        const OperationContext& context,
        const M3CompactionBoundaryEvidence& boundary);
    M3StopOutcome stop(
        const OperationContext& context,
        M3StopRequest request = {});

    [[nodiscard]] M3ProviderState state() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] persistence::ImageGeometry geometry() const;
    [[nodiscard]] DynamicBlockStoreStats storage_stats() const;
    [[nodiscard]] proxy::SessionSnapshot transport_snapshot() const;
    [[nodiscard]] proxy::ProxyObjectNames names() const;

private:
    M3Provider(
        M3ProviderConfig config,
        persistence::RecoveredImage image,
        persistence::BundleWriterLock writer,
        std::unique_ptr<DynamicBlockStore> store,
        std::unique_ptr<IM3ProviderTransport> transport,
        std::function<Result<void>()> before_checkpoint_prepare,
        std::function<Result<void>()> before_checkpoint_reconcile
#if defined(MYRD_ENABLE_TEST_SEAMS)
        , std::function<Result<void>(
              const persistence::JournalWriterPhaseEvent&)>
              journal_phase_hook,
        persistence::ManifestPublishTestPhaseHook manifest_phase_hook,
        persistence::CheckpointReconcileTestHook reconcile_phase_hook
#endif
        );

    static Result<std::unique_ptr<M3Provider>> open_impl(
        const OperationContext& context,
        const proxy::IProxyNamespacePolicy& namespace_policy,
        M3ProviderConfig config,
        M3ProviderFactories factories);
    M3CheckpointOutcome checkpoint_impl(
        const OperationContext& context,
        persistence::VolumeBarrierKind barrier);
    M3CheckpointOutcome resume_pending_checkpoint(
        const OperationContext& context);
    void set_state(M3ProviderState state) noexcept;

    // Correlation frozen at prepare and required again at commit. It never
    // holds the transaction itself: that stays in pending_checkpoint_ so the
    // existing stop/resume path is untouched.
    struct PreparedTwoPhaseCorrelation final {
        std::uint64_t native_operation_id{};
        std::uint64_t prior_generation{};
        std::uint64_t candidate_generation{};
        std::uint8_t manifest_slot{};
        Sha256Digest candidate_bytes_digest{};
        persistence::CheckpointManifestPublishRequest publish{};
    };

    // Runs prepare with an explicit native operation id and leaves the
    // transaction plus its frozen candidate pending.
    //
    // Contract: on failure the returned outcome carries an error and nothing is
    // left pending; on success the returned outcome has no error and
    // `pending_two_phase_` is engaged. No manifest slot is touched either way.
    M3CheckpointOutcome checkpoint_prepare_impl(
        const OperationContext& context,
        persistence::VolumeBarrierKind barrier,
        std::uint64_t operation_id);
    // Publishes the candidate frozen by prepare and then reconciles it.
    M3CheckpointOutcome publish_prepared_checkpoint(
        const OperationContext& context);

    mutable std::mutex operation_mutex_;
    std::atomic<M3ProviderState> state_{M3ProviderState::Constructed};
    M3ProviderConfig config_;
    persistence::RecoveredImage image_;
    persistence::BundleWriterLock writer_;
    std::unique_ptr<DynamicBlockStore> store_;
    persistence::ImageSession session_;
    std::unique_ptr<IM3ProviderTransport> transport_;
    proxy::ProxyObjectNames names_;
    std::optional<proxy::SessionSnapshot> final_snapshot_{};
    std::optional<DynamicBlockStoreStats> final_stats_{};
    std::function<Result<void>()> before_checkpoint_prepare_{};
    std::function<Result<void>()> before_checkpoint_reconcile_{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::function<Result<void>(
        const persistence::JournalWriterPhaseEvent&)> journal_phase_hook_{};
    persistence::ManifestPublishTestPhaseHook manifest_phase_hook_{};
    persistence::CheckpointReconcileTestHook reconcile_phase_hook_{};
#endif
    std::optional<persistence::PreparedCheckpointTransaction>
        pending_checkpoint_{};
    // Engaged exactly while a two-phase prepare is outstanding and unpublished.
    // Cleared by abort, by commit, and by any prepare that fails after the point
    // where the previous correlation was replaced.
    std::optional<PreparedTwoPhaseCorrelation> pending_two_phase_{};
    persistence::CommitDisposition last_checkpoint_disposition_{
        persistence::CommitDisposition::NotCommitted};
    std::optional<Error> last_checkpoint_error_{};
    std::optional<persistence::PersistenceErrorContext>
        last_checkpoint_error_context_{};
    std::vector<persistence::OwnedImmutableArtifact>
        retention_candidates_{};
};

} // namespace myrd::m3
