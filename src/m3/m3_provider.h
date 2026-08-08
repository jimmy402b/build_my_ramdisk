#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/image_session.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/recovery_engine.h"
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
    persistence::CommitDisposition last_checkpoint_disposition_{
        persistence::CommitDisposition::NotCommitted};
    std::optional<Error> last_checkpoint_error_{};
    std::optional<persistence::PersistenceErrorContext>
        last_checkpoint_error_context_{};
};

} // namespace myrd::m3
