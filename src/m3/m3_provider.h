#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/image_session.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/retention_manager.h"
#include "src/persistence/writer_lock.h"
#include "src/proxy/object_names.h"
#include "src/proxy/shm_proxy_session.h"
#include "src/storage/dynamic_block_store.h"

#include <atomic>
#include <array>
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

// What a prepared-but-unpublished compaction gives the caller before the
// service records its own durable boundary.  `boundary` deliberately has no cut
// epoch: the mutation boundary is published first, then the cut callback fills
// the epoch it was measured at.  `cut_dirty_epoch` is the value that callback
// must receive.
struct M3CompactionPrepared final {
    M3CompactionBoundaryEvidence boundary{};
    std::uint64_t cut_dirty_epoch{};
    M3CompactionMetrics metrics{};
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

    // The M5 recovery proof's read-only entry point, and the only way anything
    // outside persistence may ask what a bundle proves.
    //
    // It is static by design: it is called by a fresh worker deciding whether
    // it is allowed to open this bundle at all, so no provider is open yet and
    // no writer lock is held.  It is a thin pass-through that adds no policy --
    // the verdict and its fail-closed rules live in persistence -- but it does
    // mean the worker, and therefore Task 5 and ServiceCore, never touch a
    // bundle, a manifest or a journal directly.
    [[nodiscard]] static Result<persistence::CheckpointRecoveryInspection>
    inspect_checkpoint(
        const OperationContext& context,
        M3ProviderConfig config,
        const persistence::CheckpointRecoveryCorrelation& expected);

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
    // The M5 worker's two-phase compaction.  `prepare_compaction` runs
    // preflight, folds the durable image into a new base and journal, takes the
    // consistency cut and writes the dirty delta, then **holds** the unpublished
    // transaction; no manifest slot is touched.  `commit_compaction` publishes
    // exactly that prepared transaction.  Only one compaction may be pending,
    // and it must be aborted or committed before the provider stops.
    Result<M3CompactionPrepared> prepare_compaction(
        const OperationContext& context,
        M3CompactionRequest request);
    M3CompactionOutcome commit_compaction(
        const OperationContext& context,
        std::uint64_t native_operation_id,
        std::uint64_t cut_dirty_epoch);
    Result<void> abort_compaction(std::uint64_t native_operation_id);
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
    // The RAM budget this provider's store was opened with.  Paired with
    // `storage_stats().committed_bytes` (the RAM actually committed) it is what
    // "memory is under control" means, and it is the denominator of every
    // percentage a user sees.  Kept beside `storage_stats` rather than inside
    // `DynamicBlockStoreStats` because that struct is the M2 report schema, which is
    // frozen: the budget is a property of the configuration, not of the store's
    // counters.
    [[nodiscard]] std::uint64_t memory_budget_bytes() const;
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

    // The shared body of `compact` and `prepare_compaction`.  When the two
    // handlers are non-null they are invoked exactly where the one-shot compact
    // invoked them (boundary after preflight, cut inside the snapshot begin);
    // the split M5 path passes null and performs those durable writes above this
    // layer between prepare and commit.  On failure `evidence` and `metrics`
    // still carry whatever was established, so the one-shot caller can report
    // the same outcome it always did.
    Result<M3CompactionPrepared> prepare_compaction_impl(
        const OperationContext& context,
        M3CompactionRequest request,
        const M3CompactionBoundaryHandler* boundary,
        const M3CompactionCutHandler* cut,
        bool source_sized_space,
        M3CompactionBoundaryEvidence& evidence,
        M3CompactionMetrics& metrics,
        std::optional<persistence::PersistenceErrorContext>& error_context);
    M3CompactionOutcome commit_compaction_impl(
        const OperationContext& context);
    // Deletes the source pair a just-published compaction retired.
    //
    // The generic retention path keeps one retired pair because the previous
    // manifest slot still names it.  Online compaction's whole purpose is a
    // bounded journal, so the M5 split path asks for that pair to be discarded
    // once the candidate manifest is published and installed.  Every deletion is
    // still identity-anchored: the named file must be the file object the
    // artifact record observed, or it is left alone.  A failure is retention,
    // not a failed compaction.
    Result<void> purge_retired_compaction_artifacts(
        const OperationContext& context);

    // Everything an unpublished compaction holds between prepare and commit.
    // The transaction owns the snapshot and the new journal file; the journal
    // writer must outlive it because the delta was written through that writer.
    // Move-only, like the transaction it carries.
    struct PendingCompaction final {
        PendingCompaction(
            std::uint64_t native_operation_id,
            std::uint64_t source_generation,
            std::uint64_t candidate_generation,
            std::uint64_t cut_dirty_epoch,
            M3CompactionBoundaryEvidence boundary,
            M3CompactionMetrics metrics,
            persistence::PreparedCheckpointTransaction transaction,
            std::unique_ptr<persistence::JournalWriter> journal_writer,
            std::array<persistence::OwnedImmutableArtifact, 2>
                candidate_artifacts) noexcept;

        std::uint64_t native_operation_id{};
        std::uint64_t source_generation{};
        std::uint64_t candidate_generation{};
        std::uint64_t cut_dirty_epoch{};
        M3CompactionBoundaryEvidence boundary{};
        M3CompactionMetrics metrics{};
        persistence::PreparedCheckpointTransaction transaction;
        std::unique_ptr<persistence::JournalWriter> journal_writer;
        std::array<persistence::OwnedImmutableArtifact, 2>
            candidate_artifacts{};
    };

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
    // Engaged exactly between a successful `prepare_compaction` and its commit
    // or abort.  The two pending kinds are mutually exclusive: a compaction
    // folds the durable view and a checkpoint cuts the live one, and allowing
    // both at once would let them publish over one another.
    std::optional<PendingCompaction> pending_compaction_{};
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
