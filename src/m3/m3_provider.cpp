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
    const std::uint64_t generation,
    std::optional<PersistenceErrorContext> error_context = std::nullopt) {
    return {disposition, generation, error, std::move(error_context)};
}

PersistenceErrorContext provider_error_context(
    const RecoveredImage& image,
    const std::uint64_t operation_id,
    const Error& source,
    const CommitDisposition disposition,
    const std::uint64_t generation,
    const std::string_view operation,
    const SessionPhase phase,
    const std::string_view safe_context) {
    auto code = checkpoint_persistence_code_for(source);
    PersistenceErrorContext context{};
    context.operation = std::string(operation);
    context.code = code;
    context.phase = phase;
    context.native_code = source.native_code;
    context.operation_id = operation_id;
    context.image_uuid = image.image_uuid;
    context.generation = generation;
    context.disposition = disposition;
    context.safe_context = std::string(safe_context);

    if (!widen_ascii_operation(operation).has_value() ||
        !is_complete_error_context(context)) {
        code = PersistenceErrorCode::FatalInternalInvariant;
        context.operation = "checkpoint.failure";
        context.code = code;
        context.phase = SessionPhase::PersistenceFaulted;
        context.safe_context = "checkpoint error normalization failed";
    }
    return context;
}

M3CheckpointOutcome provider_checkpoint_failure(
    const RecoveredImage& image,
    const std::uint64_t operation_id,
    const Error& source,
    const CommitDisposition disposition,
    const std::uint64_t generation,
    const std::string_view operation,
    const SessionPhase phase,
    const std::string_view safe_context) {
    auto context = provider_error_context(
        image, operation_id, source, disposition, generation,
        operation, phase, safe_context);
    auto wide_operation = widen_ascii_operation(context.operation);
    return {
        disposition,
        generation,
        Error{ErrorDomain::persistence,
              static_cast<std::uint32_t>(context.code),
              std::move(*wide_operation),
              source.message,
              source.native_code},
        std::move(context)};
}

M3CheckpointOutcome preserve_or_normalize_checkpoint_failure(
    const RecoveredImage& image,
    const std::uint64_t operation_id,
    const Error& error,
    const CommitDisposition disposition,
    const std::uint64_t generation,
    std::optional<PersistenceErrorContext> error_context,
    const std::string_view operation,
    const SessionPhase phase,
    const std::string_view safe_context) {
    if (error_context.has_value() &&
        error.domain == ErrorDomain::persistence &&
        is_complete_error_context(*error_context) &&
        error.code == static_cast<std::uint32_t>(error_context->code) &&
        error.native_code == error_context->native_code &&
        operation_labels_match(error.operation, error_context->operation) &&
        error_context->generation == generation &&
        error_context->disposition == disposition) {
        return checkpoint_failure(
            error, disposition, generation, std::move(error_context));
    }
    return provider_checkpoint_failure(
        image, operation_id, error, disposition, generation,
        operation, phase, safe_context);
}

} // namespace

M3Provider::M3Provider(
    M3ProviderConfig config,
    RecoveredImage image,
    BundleWriterLock writer,
    std::unique_ptr<DynamicBlockStore> store,
    std::unique_ptr<IM3ProviderTransport> transport,
    std::function<Result<void>()> before_checkpoint_prepare,
    std::function<Result<void>()> before_checkpoint_reconcile
#if defined(MYRD_ENABLE_TEST_SEAMS)
    , std::function<Result<void>(const JournalWriterPhaseEvent&)>
          journal_phase_hook,
    ManifestPublishTestPhaseHook manifest_phase_hook,
    CheckpointReconcileTestHook reconcile_phase_hook
#endif
    )
    : config_(std::move(config)),
      image_(std::move(image)),
      writer_(std::move(writer)),
      store_(std::move(store)),
      session_(ImageSession::ready()),
      transport_(std::move(transport)),
      names_(transport_->names()),
      before_checkpoint_prepare_(std::move(before_checkpoint_prepare)),
      before_checkpoint_reconcile_(std::move(before_checkpoint_reconcile))
#if defined(MYRD_ENABLE_TEST_SEAMS)
      , journal_phase_hook_(std::move(journal_phase_hook)),
      manifest_phase_hook_(std::move(manifest_phase_hook)),
      reconcile_phase_hook_(std::move(reconcile_phase_hook))
#endif
      {}

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
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto journal_phase_hook = std::move(factories.journal_phase_hook);
    auto manifest_phase_hook = std::move(factories.manifest_phase_hook);
    auto reconcile_phase_hook = std::move(factories.reconcile_phase_hook);
#endif
    return Result<std::unique_ptr<M3Provider>>::success(
        std::unique_ptr<M3Provider>(new M3Provider(
            std::move(config), std::move(*recovered.image),
            std::move(writer_lock), store.move_value(),
            transport.move_value(), std::move(before_checkpoint_prepare),
            std::move(before_checkpoint_reconcile)
#if defined(MYRD_ENABLE_TEST_SEAMS)
            , std::move(journal_phase_hook), std::move(manifest_phase_hook),
            std::move(reconcile_phase_hook)
#endif
            )));
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
    const auto operation_id = image_.checkpoint_generation + 3902;
    if (state() != M3ProviderState::Ready) {
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"provider is not ready"),
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            "checkpoint.prepare", SessionPhase::Ready,
            "provider is not ready for checkpoint");
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
    last_checkpoint_error_context_ = unresolved
        ? result.error_context : std::nullopt;
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
    const auto operation_id = image_.checkpoint_generation + 3902;
    const auto target_generation = image_.checkpoint_generation + 1;
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return provider_checkpoint_failure(
            image_, operation_id, live.error(),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint operation context rejected");
    }
    if (!is_valid_barrier_for_checkpoint(barrier) || !store_) {
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"checkpoint barrier or store is invalid"),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint barrier or store is invalid");
    }
    if (before_checkpoint_prepare_) {
        auto observed = before_checkpoint_prepare_();
        if (!observed.ok()) {
            return provider_checkpoint_failure(
                image_, operation_id, observed.error(),
                CommitDisposition::NotCommitted, target_generation,
                "checkpoint.prepare", SessionPhase::Ready,
                "checkpoint preparation observer failed");
        }
    }
    auto correlation = checkpoint_correlation(image_, operation_id);
    if (!correlation.ok()) {
        return provider_checkpoint_failure(
            image_, operation_id, correlation.error(),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint correlation creation failed");
    }
    JournalHeader header{};
    auto journal_file = open_journal(context, writer_, image_, header);
    if (!journal_file.ok()) {
        return provider_checkpoint_failure(
            image_, operation_id, journal_file.error(),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint journal open failed");
    }
    auto options = writer_options(config_.recovery.limits);
#if defined(MYRD_ENABLE_TEST_SEAMS)
    options.phase_hook = journal_phase_hook_;
#endif
    auto writer = JournalWriter::create(std::move(options));
    if (!writer.ok()) {
        return provider_checkpoint_failure(
            image_, operation_id, writer.error(),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint journal writer creation failed");
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
        return preserve_or_normalize_checkpoint_failure(
            image_, operation_id,
            outcome.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"checkpoint preparation failed")),
            outcome.disposition, outcome.generation, outcome.error_context,
            "checkpoint.prepare", SessionPhase::Checkpointing,
            "checkpoint preparation failed without complete context");
    }
    auto transaction = prepared.move_transaction();
    auto manifest_capability = open_locked_manifest_for_mutation(writer_);
    if (!manifest_capability.ok()) {
        const auto error = manifest_capability.error();
        (void)transaction.abort();
        return provider_checkpoint_failure(
            image_, operation_id, error, CommitDisposition::NotCommitted,
            target_generation, "checkpoint.publish",
            SessionPhase::Checkpointing,
            "checkpoint manifest capability open failed");
    }
    auto manifest = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest.ok()) {
        const auto error = manifest.error();
        (void)transaction.abort();
        return provider_checkpoint_failure(
            image_, operation_id, error, CommitDisposition::NotCommitted,
            target_generation, "checkpoint.publish",
            SessionPhase::Checkpointing,
            "checkpoint manifest open failed");
    }
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest.move_value();
    publish.active_slot = image_.selected_manifest;
    publish.committed_time_100ns = image_.checkpoint_generation + 2;
    publish.complete_validation = CompleteCandidateValidation{
        config_.bundle_root, config_.manifest_name, config_.recovery};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    publish.test_phase_hook = manifest_phase_hook_;
#endif
    auto published = transaction.publish_manifest(context, std::move(publish));
    if (published.disposition != CommitDisposition::Committed &&
        published.disposition != CommitDisposition::CommitIndeterminate &&
        published.disposition !=
            CommitDisposition::CommittedNeedsReconciliation) {
        (void)transaction.abort();
        return preserve_or_normalize_checkpoint_failure(
            image_, operation_id,
            published.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"checkpoint was not committed")),
            published.disposition, published.generation,
            published.error_context, "checkpoint.publish",
            SessionPhase::Checkpointing,
            "checkpoint publication failed without complete context");
    }
    pending_checkpoint_.emplace(std::move(transaction));
    return resume_pending_checkpoint(context);
}

M3CheckpointOutcome M3Provider::resume_pending_checkpoint(
    const OperationContext& context) {
    if (!pending_checkpoint_.has_value()) {
        const auto operation_id = image_.checkpoint_generation + 3902;
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"no checkpoint is pending"),
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            "checkpoint.prepare", SessionPhase::Ready,
            "no checkpoint transaction is pending");
    }
    auto& transaction = *pending_checkpoint_;
    const auto operation_id = transaction.checkpoint().ack_stream.operation_id;
    if (transaction.disposition() == CommitDisposition::CommitIndeterminate) {
        const auto resolved = transaction.resolve_commit(
            context, {FilesystemCommitResolution{
                config_.bundle_root, config_.manifest_name,
                config_.recovery, &writer_}});
        if (resolved.disposition == CommitDisposition::NotCommitted) {
            const auto outcome = preserve_or_normalize_checkpoint_failure(
                image_, operation_id,
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"indeterminate checkpoint was not committed")),
                resolved.disposition, resolved.generation,
                resolved.error_context, "checkpoint.resolve",
                SessionPhase::ResolvingCommit,
                "checkpoint resolution failed without complete context");
            pending_checkpoint_.reset();
            return outcome;
        }
        if (resolved.disposition == CommitDisposition::CommitIndeterminate) {
            return preserve_or_normalize_checkpoint_failure(
                image_, operation_id,
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"checkpoint commit remains indeterminate")),
                resolved.disposition, resolved.generation,
                resolved.error_context, "checkpoint.resolve",
                SessionPhase::ResolvingCommit,
                "checkpoint resolution remained indeterminate without complete context");
        }
        if (resolved.disposition != CommitDisposition::Committed &&
            resolved.disposition !=
                CommitDisposition::CommittedNeedsReconciliation) {
            return preserve_or_normalize_checkpoint_failure(
                image_, operation_id,
                resolved.error.value_or(provider_error(
                    L"M3Provider::checkpoint",
                    L"commit resolution returned an invalid disposition")),
                resolved.disposition, resolved.generation,
                resolved.error_context, "checkpoint.resolve",
                SessionPhase::ResolvingCommit,
                "checkpoint resolution returned invalid disposition without complete context");
        }
    }
    CheckpointReconcileRequest reconcile_request{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (before_checkpoint_reconcile_ || reconcile_phase_hook_) {
        reconcile_request.test_phase_hook = [this](
            const CheckpointReconcileTestPhase phase) {
            if (phase == CheckpointReconcileTestPhase::BeforeRecoverView &&
                before_checkpoint_reconcile_) {
                auto observed = before_checkpoint_reconcile_();
                if (!observed.ok()) return observed;
            }
            if (reconcile_phase_hook_) return reconcile_phase_hook_(phase);
            return Result<void>::success();
        };
    }
#endif
    const auto reconciled = transaction.reconcile(
        context, reconcile_request);
    if (!reconciled.ok()) {
        return preserve_or_normalize_checkpoint_failure(
            image_, operation_id,
            reconciled.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"reconciliation failed")),
            reconciled.disposition, reconciled.generation,
            reconciled.error_context, "checkpoint.reconcile",
            SessionPhase::Reconciling,
            "checkpoint reconciliation failed without complete context");
    }
    auto refreshed = recover_complete_image(
        context, config_.bundle_root, config_.manifest_name, config_.recovery);
    if (!refreshed.ok() || !refreshed.image.has_value()) {
        pending_checkpoint_.reset();
        return provider_checkpoint_failure(
            image_, operation_id,
            refreshed.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"committed image did not reopen")),
            CommitDisposition::CommittedNeedsReconciliation,
            reconciled.generation, "checkpoint.reconcile",
            SessionPhase::Reconciling,
            "committed checkpoint did not reopen");
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
    if (pending_checkpoint_.has_value()) {
        result.generation = pending_checkpoint_->checkpoint().generation;
        result.disposition = pending_checkpoint_->disposition();
    } else if (last_checkpoint_error_context_.has_value()) {
        result.generation = last_checkpoint_error_context_->generation;
    }
    set_state(M3ProviderState::Stopping);
    const auto stop_operation_id = result.generation + 4902;
    const auto remember = [this, &result, stop_operation_id](
        const Error& error,
        std::optional<PersistenceErrorContext> error_context,
        const std::string_view operation,
        const SessionPhase phase,
        const std::string_view safe_context) {
        if (result.error.has_value()) return;
        result.error = error;
        if (error_context.has_value() &&
            is_complete_error_context(*error_context)) {
            result.error_context = std::move(error_context);
            return;
        }
        result.error_context = provider_error_context(
            image_, stop_operation_id, error, result.disposition,
            result.generation, operation, phase, safe_context);
    };
    if (transport_) {
        try {
            const auto stopped = transport_->stop();
            result.transport_stopped = stopped.ok();
            if (!stopped.ok()) {
                remember(stopped.error(), std::nullopt, "stop.transport",
                         SessionPhase::Closing,
                         "provider transport stop failed");
            }
        } catch (...) {
            result.transport_stopped = false;
            remember(provider_error(L"M3Provider::stop",
                                    L"transport stop threw"),
                     std::nullopt, "stop.transport", SessionPhase::Closing,
                     "provider transport stop threw");
        }
        try {
            final_snapshot_ = transport_->snapshot();
        } catch (...) {
            remember(provider_error(L"M3Provider::stop",
                                    L"transport snapshot threw"),
                     std::nullopt, "stop.transport", SessionPhase::Closing,
                     "provider transport snapshot threw");
        }
        transport_.reset();
    } else {
        result.transport_stopped = true;
    }
    bool persistence_ready = true;
    if (pending_checkpoint_.has_value()) {
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
            last_checkpoint_error_context_ = unresolved
                ? resumed.error_context : std::nullopt;
            result.generation = resumed.generation;
            persistence_ready = resumed.ok();
            if (resumed.error.has_value()) {
                remember(*resumed.error, resumed.error_context,
                         "checkpoint.reconcile", SessionPhase::Reconciling,
                         "pending checkpoint reconciliation failed");
            }
        }
    } else if (result.transport_stopped &&
               (result.disposition == CommitDisposition::CommitIndeterminate ||
                result.disposition ==
                    CommitDisposition::CommittedNeedsReconciliation)) {
        persistence_ready = false;
        remember(last_checkpoint_error_.value_or(provider_error(
                     L"M3Provider::stop",
                     L"persistence management requires restart recovery")),
                 last_checkpoint_error_context_, "stop.persistence",
                 SessionPhase::PersistenceFaulted,
                 "persistence management requires restart recovery");
    }
    if (request.checkpoint && persistence_ready) {
        if (!result.transport_stopped || !request.barrier.has_value()) {
            remember(provider_error(
                         L"M3Provider::stop",
                         L"checkpoint requires a stopped transport and barrier"),
                     std::nullopt, "stop.checkpoint", SessionPhase::Closing,
                     "stop checkpoint requires transport barrier");
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
            last_checkpoint_error_context_ = unresolved
                ? checkpoint_result.error_context : std::nullopt;
            result.generation = checkpoint_result.generation;
            result.checkpoint_completed = checkpoint_result.ok();
            if (checkpoint_result.error.has_value()) {
                remember(*checkpoint_result.error,
                         checkpoint_result.error_context,
                         "stop.checkpoint", SessionPhase::Checkpointing,
                         "stop checkpoint failed");
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
    if (!session_closed.ok()) {
        remember(session_closed.error(), std::nullopt, "stop.session",
                 SessionPhase::Closing, "image session close failed");
    }
    const auto writer_closed = writer_.close();
    result.writer_closed = writer_closed.ok();
    if (!writer_closed.ok()) {
        remember(writer_closed.error(), std::nullopt, "stop.writer",
                 SessionPhase::Closing, "bundle writer close failed");
    }
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
