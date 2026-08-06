#include "src/m3/m3_provider.h"

#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/proxy/request_processor.h"
#include "src/proxy/shared_memory_channel.h"

#include <algorithm>
#include <array>
#include <utility>

namespace myrd::m3 {
namespace {

using namespace persistence;

Error provider_error(const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
            operation, message};
}

class ShmProviderTransport final : public IM3ProviderTransport {
public:
    ShmProviderTransport(proxy::SharedMemoryChannel channel,
                         IBlockDevice& device,
                         proxy::ShmProxySessionOptions options)
        : processor_(device), session_(std::move(channel), processor_, options) {}

    Result<void> start() override { return session_.start(); }
    Result<void> stop() override { return session_.stop(); }
    proxy::SessionSnapshot snapshot() const override {
        return session_.snapshot();
    }
    proxy::ProxyObjectNames names() const override { return session_.names(); }

private:
    proxy::RequestProcessor processor_;
    proxy::ShmProxySession session_;
};

Result<std::unique_ptr<IM3ProviderTransport>> create_transport(
    IBlockDevice& device,
    const proxy::IProxyNamespacePolicy& namespace_policy,
    proxy::ShmProxySessionOptions options) {
    auto session_id = proxy::generate_session_id();
    if (!session_id.ok()) {
        return Result<std::unique_ptr<IM3ProviderTransport>>::failure(
            session_id.error());
    }
    auto names = namespace_policy.build_names(session_id.value());
    if (!names.ok()) {
        return Result<std::unique_ptr<IM3ProviderTransport>>::failure(
            names.error());
    }
    auto channel = proxy::SharedMemoryChannel::create(names.value());
    if (!channel.ok()) {
        return Result<std::unique_ptr<IM3ProviderTransport>>::failure(
            channel.error());
    }
    std::unique_ptr<IM3ProviderTransport> result =
        std::make_unique<ShmProviderTransport>(
            channel.move_value(), device, options);
    return Result<std::unique_ptr<IM3ProviderTransport>>::success(
        std::move(result));
}

M3ProviderFactories normalize(M3ProviderFactories factories) {
    if (!factories.transport) factories.transport = create_transport;
    return factories;
}

JournalWriterOptions writer_options(const PersistenceLimits& limits) {
    JournalWriterOptions options{};
    options.index_builder.reader_cache = {
        limits.max_index_cache_bytes, limits.max_index_cache_entries};
    options.index_builder.max_new_nodes = (std::min)(
        limits.max_journal_records,
        limits.max_index_cache_bytes / kIndexNodeBytes);
    options.index_builder.max_plan_image_bytes = limits.max_index_cache_bytes;
    options.max_checkpoint_records = limits.max_journal_records;
    options.max_checkpoint_bytes = limits.max_journal_committed_bytes;
    return options;
}

Result<CompletePersistenceErrorContext> checkpoint_correlation(
    const RecoveredImage& image,
    const std::uint64_t operation_id) {
    PersistenceErrorContext value{};
    value.operation = "checkpoint.prepare";
    value.code = PersistenceErrorCode::InvalidFormat;
    value.phase = SessionPhase::Ready;
    value.operation_id = operation_id;
    value.image_uuid = image.image_uuid;
    value.generation = image.checkpoint_generation + 1;
    value.disposition = CommitDisposition::NotCommitted;
    value.safe_context = "M3 provider checkpoint";
    auto complete = CompletePersistenceErrorContext::create(std::move(value));
    if (!complete.has_value()) {
        return Result<CompletePersistenceErrorContext>::failure(
            provider_error(L"M3Provider::checkpoint",
                           L"could not create checkpoint correlation"));
    }
    return Result<CompletePersistenceErrorContext>::success(
        std::move(*complete));
}

Result<std::shared_ptr<IPersistenceFile>> open_journal(
    const OperationContext& context,
    BundleWriterLock& writer,
    const RecoveredImage& image,
    JournalHeader& header) {
    auto capability = open_bundle_file_for_append(
        writer, image.selected_manifest.journal_relative_name,
        image.backing_identity.journal_windows_identity);
    if (!capability.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            capability.error());
    }
    auto opened = open_validated_persistence_file(capability.move_value());
    if (!opened.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            opened.error());
    }
    JournalHeaderBytes bytes{};
    auto read = opened.value()->read_exact(context, 0, bytes);
    if (!read.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(read.error());
    }
    auto decoded = decode_journal_header(bytes);
    if (!decoded.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            decoded.error());
    }
    header = decoded.move_value();
    return Result<std::shared_ptr<IPersistenceFile>>::success(
        std::shared_ptr<IPersistenceFile>(opened.move_value()));
}

M3CheckpointOutcome checkpoint_failure(
    const Error& error,
    const CommitDisposition disposition,
    const std::uint64_t generation) {
    return {disposition, generation, error};
}

} // namespace

M3Provider::M3Provider(
    M3ProviderConfig config,
    RecoveredImage image,
    BundleWriterLock writer,
    std::unique_ptr<DynamicBlockStore> store,
    std::unique_ptr<IM3ProviderTransport> transport,
    std::function<Result<void>()> before_checkpoint_prepare,
    std::function<Result<void>()> before_checkpoint_reconcile)
    : config_(std::move(config)),
      image_(std::move(image)),
      writer_(std::move(writer)),
      store_(std::move(store)),
      session_(ImageSession::ready()),
      transport_(std::move(transport)),
      names_(transport_->names()),
      before_checkpoint_prepare_(std::move(before_checkpoint_prepare)),
      before_checkpoint_reconcile_(std::move(before_checkpoint_reconcile)) {}

Result<std::unique_ptr<M3Provider>> M3Provider::open(
    const OperationContext& context,
    M3ProviderConfig config) {
    proxy::KernelVisibleNamespacePolicy policy;
    return open_impl(context, policy, std::move(config), {});
}

Result<std::unique_ptr<M3Provider>> M3Provider::open_for_test(
    const OperationContext& context,
    const proxy::IProxyNamespacePolicy& namespace_policy,
    M3ProviderConfig config,
    M3ProviderFactories factories) {
    return open_impl(context, namespace_policy, std::move(config),
                     std::move(factories));
}

Result<std::unique_ptr<M3Provider>> M3Provider::open_impl(
    const OperationContext& context,
    const proxy::IProxyNamespacePolicy& namespace_policy,
    M3ProviderConfig config,
    M3ProviderFactories factories) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::unique_ptr<M3Provider>>::failure(live.error());
    }
    if (validate_limits(config.recovery.limits) != LimitsStatus::Ok ||
        !validate_dynamic_block_store_options(config.store).ok() ||
        config.request_timeout.count() <= 0) {
        return Result<std::unique_ptr<M3Provider>>::failure(
            provider_error(L"M3Provider::open", L"provider limits are invalid"));
    }
    auto writer = acquire_bundle_writer_lock(
        config.bundle_root, config.manifest_name);
    if (!writer.ok()) {
        return Result<std::unique_ptr<M3Provider>>::failure(writer.error());
    }
    auto writer_lock = writer.move_value();
    auto recovered = recover_complete_image(
        context, config.bundle_root, config.manifest_name, config.recovery);
    if (!recovered.ok() || !recovered.image.has_value()) {
        const auto error = recovered.error.value_or(provider_error(
            L"M3Provider::open", L"no complete image was recovered"));
        (void)writer_lock.close();
        return Result<std::unique_ptr<M3Provider>>::failure(error);
    }
    auto store = open_recovered_dynamic_store(*recovered.image, config.store);
    if (!store.ok()) {
        const auto error = store.error();
        (void)writer_lock.close();
        return Result<std::unique_ptr<M3Provider>>::failure(error);
    }
    factories = normalize(std::move(factories));
    auto transport = factories.transport(
        *store.value(), namespace_policy,
        {config.request_timeout, 64});
    if (!transport.ok() || !transport.value()) {
        const auto error = transport.ok()
            ? provider_error(L"M3Provider::open",
                             L"transport factory returned null")
            : transport.error();
        store.value()->close();
        (void)writer_lock.close();
        return Result<std::unique_ptr<M3Provider>>::failure(error);
    }
    auto before_checkpoint_prepare =
        std::move(factories.before_checkpoint_prepare);
    auto before_checkpoint_reconcile =
        std::move(factories.before_checkpoint_reconcile);
    return Result<std::unique_ptr<M3Provider>>::success(
        std::unique_ptr<M3Provider>(new M3Provider(
            std::move(config), std::move(*recovered.image),
            std::move(writer_lock), store.move_value(),
            transport.move_value(), std::move(before_checkpoint_prepare),
            std::move(before_checkpoint_reconcile))));
}

M3Provider::~M3Provider() noexcept {
    try {
        (void)stop(OperationContext{}, {});
    } catch (...) {
    }
}

Result<void> M3Provider::start(const OperationContext& context) {
    std::scoped_lock lock(operation_mutex_);
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
    if (state() != M3ProviderState::Constructed || !transport_ || !store_) {
        return Result<void>::failure(provider_error(
            L"M3Provider::start", L"provider cannot be started"));
    }
    set_state(M3ProviderState::Starting);
    Result<void> started = Result<void>::failure(provider_error(
        L"M3Provider::start", L"transport start did not execute"));
    try {
        started = transport_->start();
    } catch (...) {
        started = Result<void>::failure(provider_error(
            L"M3Provider::start", L"transport start threw"));
    }
    if (!started.ok()) {
        set_state(M3ProviderState::Failed);
        return started;
    }
    set_state(M3ProviderState::Ready);
    return Result<void>::success();
}

M3CheckpointOutcome M3Provider::checkpoint(
    const OperationContext& context,
    const VolumeBarrierKind barrier) {
    std::scoped_lock lock(operation_mutex_);
    if (state() != M3ProviderState::Ready) {
        return checkpoint_failure(
            provider_error(L"M3Provider::checkpoint",
                           L"provider is not ready"),
            CommitDisposition::NotCommitted, image_.checkpoint_generation);
    }
    auto result = pending_checkpoint_.has_value()
        ? resume_pending_checkpoint(context)
        : checkpoint_impl(context, barrier);
    last_checkpoint_disposition_ = result.disposition;
    const bool unresolved =
        result.disposition == CommitDisposition::CommitIndeterminate ||
        result.disposition ==
            CommitDisposition::CommittedNeedsReconciliation;
    last_checkpoint_error_ = unresolved ? result.error : std::nullopt;
    if (unresolved && !pending_checkpoint_.has_value()) {
        // Durable/runtime state could not be retained for an in-process retry.
        // Ordinary device I/O may still drain, but persistence management is
        // fail-closed until restart recovery.
        set_state(M3ProviderState::Failed);
    }
    return result;
}

M3CheckpointOutcome M3Provider::checkpoint_impl(
    const OperationContext& context,
    const VolumeBarrierKind barrier) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return checkpoint_failure(live.error(), CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    if (!is_valid_barrier_for_checkpoint(barrier) || !store_) {
        return checkpoint_failure(
            provider_error(L"M3Provider::checkpoint",
                           L"checkpoint barrier or store is invalid"),
            CommitDisposition::NotCommitted, image_.checkpoint_generation);
    }
    if (before_checkpoint_prepare_) {
        auto observed = before_checkpoint_prepare_();
        if (!observed.ok()) {
            return checkpoint_failure(
                observed.error(), CommitDisposition::NotCommitted,
                image_.checkpoint_generation);
        }
    }
    const auto operation_id = image_.checkpoint_generation + 3902;
    auto correlation = checkpoint_correlation(image_, operation_id);
    if (!correlation.ok()) {
        return checkpoint_failure(correlation.error(),
                                  CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    JournalHeader header{};
    auto journal_file = open_journal(context, writer_, image_, header);
    if (!journal_file.ok()) {
        return checkpoint_failure(journal_file.error(),
                                  CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    auto writer = JournalWriter::create(writer_options(config_.recovery.limits));
    if (!writer.ok()) {
        return checkpoint_failure(writer.error(),
                                  CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    JournalCheckpointRequest journal{};
    journal.file = journal_file.value();
    journal.file_identity = image_.backing_identity.journal_windows_identity;
    journal.header = header;
    journal.previous_generation = image_.checkpoint_generation;
    journal.previous_digest = image_.backing_identity.commit.chain_digest;
    journal.committed_length = image_.backing_identity.journal_committed_length;
    journal.root = image_.backing_identity.root;
    journal.operation_id = operation_id;
    CheckpointPrepareRequest request{correlation.move_value()};
    request.volume_barrier = barrier;
    request.journal = std::move(journal);
    request.snapshot_batch_bytes = (std::min)(
        config_.recovery.limits.max_io_scratch_bytes,
        config_.store.max_snapshot_batch_bytes);
    CheckpointCoordinator coordinator(session_, *store_, *writer.value());
    auto prepared = coordinator.prepare(context, request);
    if (!prepared.ok()) {
        const auto& outcome = prepared.failure()->outcome;
        return checkpoint_failure(
            outcome.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"checkpoint preparation failed")),
            outcome.disposition, outcome.generation);
    }
    auto transaction = prepared.move_transaction();
    auto manifest_capability = open_locked_manifest_for_mutation(writer_);
    if (!manifest_capability.ok()) {
        const auto error = manifest_capability.error();
        (void)transaction.abort();
        return checkpoint_failure(error, CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    auto manifest = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest.ok()) {
        const auto error = manifest.error();
        (void)transaction.abort();
        return checkpoint_failure(error, CommitDisposition::NotCommitted,
                                  image_.checkpoint_generation);
    }
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest.move_value();
    publish.active_slot = image_.selected_manifest;
    publish.committed_time_100ns = image_.checkpoint_generation + 2;
    publish.complete_validation = CompleteCandidateValidation{
        config_.bundle_root, config_.manifest_name, config_.recovery};
    auto published = transaction.publish_manifest(context, std::move(publish));
    if (published.disposition != CommitDisposition::Committed &&
        published.disposition != CommitDisposition::CommitIndeterminate &&
        published.disposition !=
            CommitDisposition::CommittedNeedsReconciliation) {
        (void)transaction.abort();
        return checkpoint_failure(
            published.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"checkpoint was not committed")),
            published.disposition, published.generation);
    }
    pending_checkpoint_.emplace(std::move(transaction));
    return resume_pending_checkpoint(context);
}

M3CheckpointOutcome M3Provider::resume_pending_checkpoint(
    const OperationContext& context) {
    if (!pending_checkpoint_.has_value()) {
        return checkpoint_failure(
            provider_error(L"M3Provider::checkpoint",
                           L"no checkpoint is pending"),
            CommitDisposition::NotCommitted, image_.checkpoint_generation);
    }
    auto& transaction = *pending_checkpoint_;
    if (transaction.disposition() == CommitDisposition::CommitIndeterminate) {
        const auto resolved = transaction.resolve_commit(
            context, {FilesystemCommitResolution{
                config_.bundle_root, config_.manifest_name,
                config_.recovery, &writer_}});
        if (resolved.disposition == CommitDisposition::NotCommitted) {
            const auto outcome = checkpoint_failure(
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"indeterminate checkpoint was not committed")),
                resolved.disposition, resolved.generation);
            pending_checkpoint_.reset();
            return outcome;
        }
        if (resolved.disposition == CommitDisposition::CommitIndeterminate) {
            return checkpoint_failure(
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"checkpoint commit remains indeterminate")),
                resolved.disposition, resolved.generation);
        }
        if (resolved.disposition != CommitDisposition::Committed &&
            resolved.disposition !=
                CommitDisposition::CommittedNeedsReconciliation) {
            return checkpoint_failure(
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"commit resolution returned an invalid disposition")),
                resolved.disposition, resolved.generation);
        }
    }
    CheckpointReconcileRequest reconcile_request{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (before_checkpoint_reconcile_) {
        reconcile_request.test_phase_hook = [this](
            const CheckpointReconcileTestPhase phase) {
            if (phase == CheckpointReconcileTestPhase::BeforeRecoverView) {
                return before_checkpoint_reconcile_();
            }
            return Result<void>::success();
        };
    }
#endif
    const auto reconciled = transaction.reconcile(
        context, reconcile_request);
    if (!reconciled.ok()) {
        return checkpoint_failure(
            reconciled.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"reconciliation failed")),
            reconciled.disposition, reconciled.generation);
    }
    auto refreshed = recover_complete_image(
        context, config_.bundle_root, config_.manifest_name, config_.recovery);
    if (!refreshed.ok() || !refreshed.image.has_value()) {
        pending_checkpoint_.reset();
        return checkpoint_failure(
            refreshed.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"committed image did not reopen")),
            CommitDisposition::CommittedNeedsReconciliation,
            reconciled.generation);
    }
    image_ = std::move(*refreshed.image);
    pending_checkpoint_.reset();
    return {CommitDisposition::Committed, image_.checkpoint_generation,
            std::nullopt};
}

M3StopOutcome M3Provider::stop(
    const OperationContext& context,
    const M3StopRequest request) {
    std::scoped_lock lock(operation_mutex_);
    M3StopOutcome result{};
    result.generation = image_.checkpoint_generation;
    result.disposition = last_checkpoint_disposition_;
    if (state() == M3ProviderState::Stopped) {
        result.transport_stopped = true;
        result.store_closed = true;
        result.session_closed = true;
        result.writer_closed = true;
        return result;
    }
    set_state(M3ProviderState::Stopping);
    const auto remember = [&result](const Error& error) {
        if (!result.error.has_value()) result.error = error;
    };
    if (transport_) {
        try {
            const auto stopped = transport_->stop();
            result.transport_stopped = stopped.ok();
            if (!stopped.ok()) remember(stopped.error());
        } catch (...) {
            result.transport_stopped = false;
            remember(provider_error(L"M3Provider::stop",
                                    L"transport stop threw"));
        }
        try {
            final_snapshot_ = transport_->snapshot();
        } catch (...) {
            remember(provider_error(L"M3Provider::stop",
                                    L"transport snapshot threw"));
        }
        transport_.reset();
    } else {
        result.transport_stopped = true;
    }
    bool persistence_ready = true;
    if (pending_checkpoint_.has_value()) {
        result.generation = pending_checkpoint_->checkpoint().generation;
        result.disposition = pending_checkpoint_->disposition();
        if (!result.transport_stopped) {
            persistence_ready = false;
        } else {
            const auto resumed = resume_pending_checkpoint(context);
            result.disposition = resumed.disposition;
            last_checkpoint_disposition_ = resumed.disposition;
            const bool unresolved =
                resumed.disposition == CommitDisposition::CommitIndeterminate ||
                resumed.disposition ==
                    CommitDisposition::CommittedNeedsReconciliation;
            last_checkpoint_error_ = unresolved
                ? resumed.error : std::nullopt;
            result.generation = resumed.generation;
            persistence_ready = resumed.ok();
            if (resumed.error.has_value()) remember(*resumed.error);
        }
    } else if (result.transport_stopped &&
               (result.disposition == CommitDisposition::CommitIndeterminate ||
                result.disposition ==
                    CommitDisposition::CommittedNeedsReconciliation)) {
        persistence_ready = false;
        remember(last_checkpoint_error_.value_or(provider_error(
            L"M3Provider::stop",
            L"persistence management requires restart recovery")));
    }
    if (request.checkpoint && persistence_ready) {
        if (!result.transport_stopped || !request.barrier.has_value()) {
            remember(provider_error(
                L"M3Provider::stop",
                L"checkpoint requires a stopped transport and barrier"));
        } else {
            const auto checkpoint_result = checkpoint_impl(
                context, *request.barrier);
            result.disposition = checkpoint_result.disposition;
            last_checkpoint_disposition_ = checkpoint_result.disposition;
            const bool unresolved =
                checkpoint_result.disposition ==
                    CommitDisposition::CommitIndeterminate ||
                checkpoint_result.disposition ==
                    CommitDisposition::CommittedNeedsReconciliation;
            last_checkpoint_error_ = unresolved
                ? checkpoint_result.error : std::nullopt;
            result.generation = checkpoint_result.generation;
            result.checkpoint_completed = checkpoint_result.ok();
            if (checkpoint_result.error.has_value()) {
                remember(*checkpoint_result.error);
            }
        }
    }
    // An unresolved transaction must release its snapshot/session lease before
    // the store and ImageSession are drained. The durable disposition and
    // first error above remain authoritative; restart recovery is the fallback.
    pending_checkpoint_.reset();
    if (store_) {
        final_stats_ = store_->stats();
        store_->close();
        store_.reset();
    }
    result.store_closed = true;
    const auto session_closed = session_.close(context);
    result.session_closed = session_closed.ok();
    if (!session_closed.ok()) remember(session_closed.error());
    const auto writer_closed = writer_.close();
    result.writer_closed = writer_closed.ok();
    if (!writer_closed.ok()) remember(writer_closed.error());
    set_state(result.error.has_value() ? M3ProviderState::Failed
                                      : M3ProviderState::Stopped);
    return result;
}

M3ProviderState M3Provider::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool M3Provider::ready() const noexcept {
    return state() == M3ProviderState::Ready;
}

std::uint64_t M3Provider::generation() const {
    std::scoped_lock lock(operation_mutex_);
    return image_.checkpoint_generation;
}

persistence::ImageGeometry M3Provider::geometry() const {
    std::scoped_lock lock(operation_mutex_);
    return image_.geometry;
}

DynamicBlockStoreStats M3Provider::storage_stats() const {
    std::scoped_lock lock(operation_mutex_);
    if (store_) return store_->stats();
    return final_stats_.value_or(DynamicBlockStoreStats{});
}

proxy::SessionSnapshot M3Provider::transport_snapshot() const {
    std::scoped_lock lock(operation_mutex_);
    if (transport_) return transport_->snapshot();
    return final_snapshot_.value_or(proxy::SessionSnapshot{});
}

proxy::ProxyObjectNames M3Provider::names() const {
    std::scoped_lock lock(operation_mutex_);
    return names_;
}

void M3Provider::set_state(const M3ProviderState state) noexcept {
    state_.store(state, std::memory_order_release);
}

} // namespace myrd::m3
