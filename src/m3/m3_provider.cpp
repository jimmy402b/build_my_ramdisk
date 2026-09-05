#include "src/m3/m3_provider.h"

#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/retention_manager.h"
#include "src/persistence/vhdx_reader.h"
#include "src/proxy/request_processor.h"
#include "src/proxy/shared_memory_channel.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

std::string compact_base_name(const std::uint64_t generation) {
    return "base-g0000-c" + std::to_string(generation) + ".vhdx";
}

std::string compact_base_temporary_name(const std::uint64_t generation) {
    return "base-g0000-c" + std::to_string(generation) + ".tmp";
}

std::string compact_journal_name(const std::uint64_t generation) {
    return "journal-e0002-c" + std::to_string(generation) + ".rdj";
}

VhdxWriterIdentity make_compact_base_identity(
    const RecoveredImage& image,
    const std::uint64_t generation) noexcept {
    VhdxWriterIdentity result{};
    result.virtual_disk_id = image.backing_identity.virtual_disk_id;
    for (std::size_t index = 0; index < sizeof(generation); ++index) {
        const auto value = static_cast<std::byte>(
            (generation >> (index * 8U)) & 0xffU);
        result.file_identity.file_write_guid[index] = value;
        result.file_identity.data_write_guid[index] = value;
    }
    result.file_identity.file_write_guid[8] = std::byte{0xb1};
    result.file_identity.data_write_guid[8] = std::byte{0xc1};
    return result;
}

JournalFileIdentity make_compact_journal_identity(
    const std::uint64_t generation) noexcept {
    JournalFileIdentity result{};
    for (std::size_t index = 0; index < sizeof(generation); ++index) {
        result.journal_uuid[index] = static_cast<std::byte>(
            (generation >> (index * 8U)) & 0xffU);
    }
    result.journal_uuid[8] = std::byte{0xa1};
    return result;
}

VhdxWriterLimits provider_vhdx_limits(
    const PersistenceLimits& limits) noexcept {
    const auto payloads =
        limits.max_virtual_bytes / kVhdxDataBlockBytes + 1;
    return {
        payloads,
        payloads,
        (std::max)(limits.max_io_scratch_bytes,
                   static_cast<std::uint64_t>(kVhdxDataBlockBytes))};
}

Result<CompletePersistenceErrorContext> compaction_correlation(
    const RecoveredImage& image,
    const std::uint64_t operation_id) {
    PersistenceErrorContext value{};
    value.operation = "compaction.prepare";
    value.code = PersistenceErrorCode::InvalidFormat;
    value.phase = SessionPhase::Ready;
    value.operation_id = operation_id;
    value.image_uuid = image.image_uuid;
    value.generation = image.checkpoint_generation + 1;
    value.disposition = CommitDisposition::NotCommitted;
    value.safe_context = "M3 provider compaction";
    auto complete = CompletePersistenceErrorContext::create(std::move(value));
    if (!complete.has_value()) {
        return Result<CompletePersistenceErrorContext>::failure(
            provider_error(L"M3Provider::compact",
                           L"could not create compaction correlation"));
    }
    return Result<CompletePersistenceErrorContext>::success(
        std::move(*complete));
}

bool checked_add(
    std::uint64_t& total,
    const std::uint64_t value) noexcept {
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
        return false;
    }
    total += value;
    return true;
}

Result<void> compaction_preflight(
    const M3ProviderConfig& config,
    BundleWriterLock& writer,
    const RecoveredImage& image,
    const std::uint64_t operation_id,
    M3CompactionBoundaryEvidence& evidence) {
    auto writer_valid = writer.verify();
    if (!writer_valid.ok()) return writer_valid;
    if (operation_id == 0 ||
        image.checkpoint_generation >= kInvalidGeneration - 1) {
        return Result<void>::failure(
            provider_error(L"M3Provider::compact",
                           L"compaction identity is invalid"));
    }
    std::uint64_t required{};
    const auto& limits = config.recovery.limits;
    if (!checked_add(required, image.geometry.virtual_bytes) ||
        !checked_add(required, limits.max_journal_committed_bytes) ||
        !checked_add(required, limits.max_io_scratch_bytes) ||
        !checked_add(required, config.store.snapshot_cow_max_bytes) ||
        !checked_add(required, 2ULL * kVhdxDataBlockBytes)) {
        return Result<void>::failure(
            provider_error(L"M3Provider::compact",
                           L"compaction peak-space calculation overflowed"));
    }
    const auto proportional_margin = required / 20;
    constexpr std::uint64_t kMinimumSafetyMargin = 64ULL * 1024 * 1024;
    if (!checked_add(
            required,
            (std::max)(proportional_margin, kMinimumSafetyMargin))) {
        return Result<void>::failure(
            provider_error(L"M3Provider::compact",
                           L"compaction safety margin overflowed"));
    }

    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(
            config.bundle_root.c_str(), &available, nullptr, nullptr)) {
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"M3Provider::compact",
            L"querying exact bundle-volume free space failed",
            GetLastError()});
    }
    writer_valid = writer.verify();
    if (!writer_valid.ok()) return writer_valid;
    evidence = {
        operation_id,
        image.image_uuid,
        image.checkpoint_generation,
        image.checkpoint_generation + 1,
        image.selected_manifest.base_expected_length,
        image.backing_identity.journal_committed_length,
        required,
        available.QuadPart};
    if (available.QuadPart < required) {
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::DiskFull),
            L"M3Provider::compact",
            L"bundle volume lacks compaction peak-space headroom"});
    }
    return Result<void>::success();
}

std::array<OwnedImmutableArtifact, 2> retired_artifacts(
    const RecoveredImage& image) {
    OwnedImmutableArtifact base{};
    base.kind = ImmutableArtifactKind::BaseVhdx;
    base.relative_name = image.selected_manifest.base_relative_name;
    base.windows_identity = image.backing_identity.base_windows_identity;
    base.image_uuid = image.image_uuid;
    base.base_identity = image.backing_identity.base_vhdx_identity;
    base.virtual_disk_id = image.backing_identity.virtual_disk_id;

    OwnedImmutableArtifact journal{};
    journal.kind = ImmutableArtifactKind::Journal;
    journal.relative_name = image.selected_manifest.journal_relative_name;
    journal.windows_identity =
        image.backing_identity.journal_windows_identity;
    journal.image_uuid = image.image_uuid;
    journal.journal_identity = image.backing_identity.journal_identity;
    journal.journal_epoch = image.backing_identity.journal_epoch;
    return {std::move(base), std::move(journal)};
}

constexpr std::size_t kMaxProviderRetentionCandidates = 4096;

bool same_owned_artifact(
    const OwnedImmutableArtifact& left,
    const OwnedImmutableArtifact& right) noexcept {
    return left.kind == right.kind
        && left.relative_name == right.relative_name
        && same_file_object(
            left.windows_identity, right.windows_identity);
}

template <std::size_t Count>
void remember_retention_artifacts(
    std::vector<OwnedImmutableArtifact>& retained,
    const std::array<OwnedImmutableArtifact, Count>& candidates) {
    for (const auto& candidate : candidates) {
        const auto duplicate = std::find_if(
            retained.begin(), retained.end(), [&](const auto& existing) {
                return same_owned_artifact(existing, candidate);
            });
        if (duplicate != retained.end()) {
            *duplicate = candidate;
            continue;
        }
        if (retained.size() == kMaxProviderRetentionCandidates) return;
        retained.push_back(candidate);
    }
}

void forget_active_retention_artifacts(
    std::vector<OwnedImmutableArtifact>& retained,
    const RecoveredImage& active) {
    const auto active_artifacts = retired_artifacts(active);
    retained.erase(
        std::remove_if(
            retained.begin(), retained.end(), [&](const auto& candidate) {
                return std::any_of(
                    active_artifacts.begin(), active_artifacts.end(),
                    [&](const auto& active_artifact) {
                        return same_owned_artifact(
                            candidate, active_artifact);
                    });
            }),
        retained.end());
}

void cleanup_provider_retention(
    const OperationContext& context,
    ImageSession& session,
    BundleWriterLock& writer,
    DynamicBlockStore& store,
    const M3ProviderConfig& config,
    std::vector<OwnedImmutableArtifact>& retained,
    M3CompactionMetrics& metrics) {
    metrics.retained_candidate_count = retained.size();
    if (retained.empty()) return;
    auto cleanup_lease = session.begin_cleanup();
    if (!cleanup_lease.ok()) return;
    RetentionCleanupOptions options{};
    options.max_candidates = kMaxProviderRetentionCandidates;
    options.dry_run = false;
    options.max_live_views = kMaxProviderRetentionCandidates;
    options.recovery = config.recovery;
    auto cleanup = cleanup_retention_artifacts(
        context,
        cleanup_lease.move_value(),
        writer,
        store,
        config.bundle_root,
        config.manifest_name,
        retained,
        options);
    if (!cleanup.ok()) return;
    metrics.deleted_candidate_count += cleanup.value().deleted_count;
    const auto& entries = cleanup.value().entries;
    retained.erase(
        std::remove_if(
            retained.begin(), retained.end(),
            [&](const OwnedImmutableArtifact& candidate) {
                return std::any_of(
                    entries.begin(), entries.end(),
                    [&](const RetentionCleanupEntry& entry) {
                        return entry.action
                                == RetentionCleanupAction::Deleted
                            && entry.kind == candidate.kind
                            && entry.relative_name
                                == candidate.relative_name;
                    });
            }),
        retained.end());
    metrics.retained_candidate_count = retained.size();
}

std::array<OwnedImmutableArtifact, 2> prepared_compaction_artifacts(
    const PreparedCompactionOperation& operation,
    const RecoveredImage& source,
    const CompactionArtifactBuildRequest& request) {
    const auto& published_base = *operation.published_base();
    const auto& journal_header = *operation.new_journal_header();
    const auto& journal_windows_identity =
        *operation.new_journal_windows_identity();

    OwnedImmutableArtifact base{};
    base.kind = ImmutableArtifactKind::BaseVhdx;
    base.relative_name = request.base_relative_name;
    base.windows_identity = published_base.windows_identity();
    base.image_uuid = source.image_uuid;
    base.base_identity = published_base.layout().file_identity();
    base.virtual_disk_id = published_base.layout().virtual_disk_id();

    OwnedImmutableArtifact journal{};
    journal.kind = ImmutableArtifactKind::Journal;
    journal.relative_name = request.journal_relative_name;
    journal.windows_identity = journal_windows_identity;
    journal.image_uuid = source.image_uuid;
    journal.journal_identity = journal_header.journal_identity;
    journal.journal_epoch = journal_header.epoch;
    return {std::move(base), std::move(journal)};
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

M3CompactionOutcome M3Provider::compact(
    const OperationContext& context,
    const M3CompactionRequest request,
    const M3CompactionBoundaryHandler& boundary_handler,
    const M3CompactionCutHandler& cut_handler) {
    std::scoped_lock lock(operation_mutex_);
    const auto target_generation = image_.checkpoint_generation + 1;
    M3CompactionBoundaryEvidence boundary{};
    M3CompactionMetrics metrics{};
    const auto fail = [&](
        const Error& error,
        const CommitDisposition disposition,
        const std::uint64_t generation,
        std::optional<PersistenceErrorContext> error_context,
        const std::string_view operation,
        const SessionPhase phase,
        const std::string_view safe_context) {
        const auto normalized = preserve_or_normalize_checkpoint_failure(
            image_, request.operation_id, error, disposition, generation,
            std::move(error_context), operation, phase, safe_context);
        return M3CompactionOutcome{
            normalized.disposition,
            normalized.generation,
            boundary,
            metrics,
            normalized.error,
            normalized.error_context};
    };

    if (state() != M3ProviderState::Ready || !store_) {
        return fail(
            provider_error(L"M3Provider::compact", L"provider is not ready"),
            CommitDisposition::NotCommitted, target_generation, std::nullopt,
            "compaction.preflight", SessionPhase::Ready,
            "provider is not ready for compaction");
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return fail(
            live.error(), CommitDisposition::NotCommitted, target_generation,
            std::nullopt, "compaction.preflight", SessionPhase::Ready,
            "compaction operation context rejected");
    }
    if (!is_valid_barrier_for_checkpoint(request.barrier) ||
        request.operation_id == 0 || !boundary_handler || !cut_handler ||
        pending_checkpoint_.has_value()) {
        return fail(
            provider_error(
                L"M3Provider::compact",
                L"compaction request, barrier, or provider state is invalid"),
            CommitDisposition::NotCommitted, target_generation, std::nullopt,
            "compaction.preflight", SessionPhase::Ready,
            "compaction request or persistence state is invalid");
    }

    cleanup_provider_retention(
        context, session_, writer_, *store_, config_,
        retention_candidates_, metrics);

    auto preflight = compaction_preflight(
        config_, writer_, image_, request.operation_id, boundary);
    if (!preflight.ok()) {
        metrics.source_generation = boundary.source_generation;
        metrics.source_base_bytes = boundary.source_base_bytes;
        metrics.source_journal_bytes = boundary.source_journal_bytes;
        metrics.required_peak_free_bytes =
            boundary.required_peak_free_bytes;
        metrics.available_free_bytes = boundary.available_free_bytes;
        return fail(
            preflight.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.preflight",
            SessionPhase::Ready, "compaction preflight failed");
    }
    metrics.source_generation = boundary.source_generation;
    metrics.source_base_bytes = boundary.source_base_bytes;
    metrics.source_journal_bytes = boundary.source_journal_bytes;
    metrics.required_peak_free_bytes = boundary.required_peak_free_bytes;
    metrics.available_free_bytes = boundary.available_free_bytes;

    Result<M3CompactionBoundaryDecision> decision =
        Result<M3CompactionBoundaryDecision>::failure(provider_error(
            L"M3Provider::compact",
            L"compaction boundary callback did not execute"));
    try {
        decision = boundary_handler(boundary);
    } catch (...) {
        decision = Result<M3CompactionBoundaryDecision>::failure(
            provider_error(L"M3Provider::compact",
                           L"compaction boundary callback threw"));
    }
    if (!decision.ok()) {
        return fail(
            decision.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.boundary",
            SessionPhase::Ready, "compaction boundary callback failed");
    }
    if (!decision.value().continue_compaction
        || decision.value().durable_operation_revision == 0
        || decision.value().durable_profile_revision == 0
        || decision.value().evidence != boundary) {
        return fail(
            provider_error(
                L"M3Provider::compact",
                L"compaction boundary continue proof was denied or mismatched"),
            CommitDisposition::NotCommitted, target_generation, std::nullopt,
            "compaction.boundary", SessionPhase::Ready,
            "compaction boundary continue proof was invalid");
    }

    auto correlation = compaction_correlation(image_, request.operation_id);
    if (!correlation.ok()) {
        return fail(
            correlation.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.prepare",
            SessionPhase::Ready, "compaction correlation creation failed");
    }
    auto journal_writer = JournalWriter::create(
        writer_options(config_.recovery.limits));
    if (!journal_writer.ok()) {
        return fail(
            journal_writer.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.prepare",
            SessionPhase::Ready,
            "compaction journal writer creation failed");
    }

    CompactionCoordinator coordinator(session_, *store_);
    auto begun = coordinator.begin(context);
    if (!begun.ok()) {
        return fail(
            begun.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.prepare",
            SessionPhase::Compacting, "compaction operation did not begin");
    }
    auto operation = begun.move_value();
    CompactionArtifactBuildRequest artifacts{};
    artifacts.writer_lock = &writer_;
    artifacts.active_slot = image_.selected_manifest;
    artifacts.base_temporary_name =
        compact_base_temporary_name(target_generation);
    artifacts.base_relative_name = compact_base_name(target_generation);
    artifacts.base_identity =
        make_compact_base_identity(image_, target_generation);
    artifacts.journal_relative_name =
        compact_journal_name(target_generation);
    artifacts.journal_identity =
        make_compact_journal_identity(target_generation);
    artifacts.journal_epoch = image_.journal_epoch + 1;
    artifacts.operation_id = request.operation_id;
    artifacts.extent_options.max_extent_bytes = kVhdxDataBlockBytes;
    artifacts.vhdx_limits = provider_vhdx_limits(config_.recovery.limits);
    artifacts.limits.max_live_extents =
        artifacts.vhdx_limits.max_candidate_payloads;
    artifacts.offline_vhdx_validator = [](
        const OperationContext&,
        const std::filesystem::path& path) -> Result<WindowsFileIdentity> {
        auto opened = open_bundle_file_for_inspection(
            path.parent_path(), path.filename().string());
        if (!opened.ok()) {
            return Result<WindowsFileIdentity>::failure(opened.error());
        }
        return Result<WindowsFileIdentity>::success(
            opened.value().identity());
    };
    auto built = operation.build_artifacts(context, artifacts);
    if (!built.ok()) {
        return fail(
            built.error(), CommitDisposition::NotCommitted,
            target_generation, std::nullopt, "compaction.artifacts",
            SessionPhase::Compacting, "compaction artifact build failed");
    }
    metrics.live_extent_count = built.value().extents.extents_emitted;
    metrics.candidate_payloads = built.value().candidate_payloads;
    metrics.candidate_base_bytes = built.value().published_base_bytes;
    auto candidate_artifacts = prepared_compaction_artifacts(
        operation, image_, artifacts);
    remember_retention_artifacts(
        retention_candidates_, candidate_artifacts);
    metrics.retained_candidate_count = retention_candidates_.size();

    CompactionDeltaPrepareRequest delta{correlation.move_value()};
    delta.volume_barrier = request.barrier;
    delta.snapshot_batch_bytes = (std::min)(
        config_.recovery.limits.max_io_scratch_bytes,
        config_.store.max_snapshot_batch_bytes);
    delta.after_snapshot_begin = [&](
        const ConsistentSnapshotToken& token) -> Result<void> {
        if (token.source_backing_generation != boundary.source_generation) {
            return Result<void>::failure(provider_error(
                L"M3Provider::compact",
                L"compaction snapshot returned an invalid consistency cut"));
        }
        auto cut_evidence = boundary;
        cut_evidence.cut_dirty_epoch = token.cutoff_dirty_epoch;
        cut_evidence.cut_dirty_epoch_known = true;
        Result<M3CompactionBoundaryDecision> cut_decision =
            Result<M3CompactionBoundaryDecision>::failure(provider_error(
                L"M3Provider::compact",
                L"compaction cut callback did not execute"));
        try {
            cut_decision = cut_handler(cut_evidence);
        } catch (...) {
            cut_decision =
                Result<M3CompactionBoundaryDecision>::failure(
                    provider_error(
                        L"M3Provider::compact",
                        L"compaction cut callback threw"));
        }
        if (!cut_decision.ok()) {
            return Result<void>::failure(cut_decision.error());
        }
        if (!cut_decision.value().continue_compaction
            || cut_decision.value().durable_operation_revision == 0
            || cut_decision.value().durable_profile_revision == 0
            || cut_decision.value().evidence != cut_evidence) {
            return Result<void>::failure(provider_error(
                L"M3Provider::compact",
                L"compaction cut continue proof was denied or mismatched"));
        }
        boundary = cut_evidence;
        return Result<void>::success();
    };
    auto prepared = coordinator.prepare_delta(
        context, std::move(operation), delta, *journal_writer.value());
    if (!prepared.ok()) {
        const auto& outcome = prepared.failure()->outcome;
        return fail(
            outcome.error.value_or(provider_error(
                L"M3Provider::compact", L"compaction delta prepare failed")),
            outcome.disposition, outcome.generation, outcome.error_context,
            "compaction.prepare", SessionPhase::Compacting,
            "compaction delta preparation failed without complete context");
    }
    auto transaction = prepared.move_transaction();
    auto final_journal_identity = open_bundle_file_for_inspection(
        config_.bundle_root, artifacts.journal_relative_name);
    if (!final_journal_identity.ok()) {
        const auto error = final_journal_identity.error();
        (void)transaction.abort();
        return fail(
            error, CommitDisposition::NotCommitted, target_generation,
            std::nullopt, "compaction.prepare", SessionPhase::Compacting,
            "compaction journal identity refresh failed");
    }
    candidate_artifacts[1].windows_identity =
        final_journal_identity.value().identity();
    remember_retention_artifacts(
        retention_candidates_, candidate_artifacts);
    const auto checkpoint = transaction.checkpoint();
    const auto prepare_metrics = transaction.metrics();
    metrics.snapshot_records = prepare_metrics.snapshot_records;
    metrics.data_records = checkpoint.data_count;
    metrics.zero_records = checkpoint.zero_count;
    metrics.discard_records = checkpoint.discard_count;
    metrics.index_nodes = checkpoint.index_node_count;
    metrics.candidate_journal_bytes = checkpoint.committed_length;

    auto manifest_capability = open_locked_manifest_for_mutation(writer_);
    if (!manifest_capability.ok()) {
        const auto error = manifest_capability.error();
        (void)transaction.abort();
        return fail(
            error, CommitDisposition::NotCommitted, target_generation,
            std::nullopt, "compaction.publish", SessionPhase::Compacting,
            "compaction manifest capability open failed");
    }
    auto manifest = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest.ok()) {
        const auto error = manifest.error();
        (void)transaction.abort();
        return fail(
            error, CommitDisposition::NotCommitted, target_generation,
            std::nullopt, "compaction.publish", SessionPhase::Compacting,
            "compaction manifest open failed");
    }
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest.move_value();
    publish.active_slot = image_.selected_manifest;
    publish.committed_time_100ns = target_generation + 2;
    publish.complete_validation = CompleteCandidateValidation{
        config_.bundle_root, config_.manifest_name, config_.recovery};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    publish.test_phase_hook = manifest_phase_hook_;
#endif
    auto published = transaction.publish_manifest(context, std::move(publish));
    if (published.disposition == CommitDisposition::CommitIndeterminate) {
        published = transaction.resolve_commit(
            context,
            {FilesystemCommitResolution{
                config_.bundle_root,
                config_.manifest_name,
                config_.recovery,
                &writer_}});
    }
    if (published.disposition != CommitDisposition::Committed &&
        published.disposition !=
            CommitDisposition::CommittedNeedsReconciliation) {
        const auto error = published.error.value_or(provider_error(
            L"M3Provider::compact", L"compaction was not committed"));
        if (published.disposition == CommitDisposition::NotCommitted) {
            (void)transaction.abort();
        }
        return fail(
            error, published.disposition, published.generation,
            published.error_context, "compaction.publish",
            published.disposition == CommitDisposition::CommitIndeterminate
                ? SessionPhase::ResolvingCommit
                : SessionPhase::Compacting,
            "compaction publication failed without complete context");
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
    const auto reconciled = transaction.reconcile(context, reconcile_request);
    if (!reconciled.ok()) {
        return fail(
            reconciled.error.value_or(provider_error(
                L"M3Provider::compact", L"compaction reconciliation failed")),
            reconciled.disposition, reconciled.generation,
            reconciled.error_context, "compaction.reconcile",
            SessionPhase::Reconciling,
            "compaction reconciliation failed without complete context");
    }
    metrics.newer_dirty_preserved = reconciled.ack.newer_dirty_preserved;

    auto refreshed = recover_complete_image(
        context, config_.bundle_root, config_.manifest_name, config_.recovery);
    if (!refreshed.ok() || !refreshed.image.has_value()) {
        return fail(
            refreshed.error.value_or(provider_error(
                L"M3Provider::compact", L"committed image did not reopen")),
            CommitDisposition::CommittedNeedsReconciliation,
            reconciled.generation, std::nullopt, "compaction.reconcile",
            SessionPhase::Reconciling,
            "committed compaction did not reopen");
    }
    auto retired = retired_artifacts(image_);
    image_ = std::move(*refreshed.image);
    metrics.committed_generation = image_.checkpoint_generation;
    metrics.candidate_journal_bytes =
        image_.backing_identity.journal_committed_length;

    forget_active_retention_artifacts(retention_candidates_, image_);
    remember_retention_artifacts(retention_candidates_, retired);
    cleanup_provider_retention(
        context, session_, writer_, *store_, config_,
        retention_candidates_, metrics);
    return {
        CommitDisposition::Committed,
        image_.checkpoint_generation,
        boundary,
        metrics,
        std::nullopt,
        std::nullopt};
}

M3CompactionOutcome M3Provider::reconcile_compaction(
    const OperationContext& context,
    const M3CompactionBoundaryEvidence& boundary) {
    std::scoped_lock lock(operation_mutex_);
    M3CompactionMetrics metrics{};
    metrics.source_generation = boundary.source_generation;
    metrics.source_base_bytes = boundary.source_base_bytes;
    metrics.source_journal_bytes = boundary.source_journal_bytes;
    metrics.required_peak_free_bytes = boundary.required_peak_free_bytes;
    metrics.available_free_bytes = boundary.available_free_bytes;
    const auto fail = [&](
        const Error& error,
        const CommitDisposition disposition,
        const std::uint64_t generation,
        const std::string_view safe_context) {
        auto source = error;
        if (disposition == CommitDisposition::CommitIndeterminate) {
            source.domain = ErrorDomain::persistence;
            source.code = static_cast<std::uint32_t>(
                PersistenceErrorCode::CommitIndeterminate);
        }
        const auto normalized = provider_checkpoint_failure(
            image_, boundary.operation_id, source, disposition, generation,
            "compaction.recover", SessionPhase::Reconciling, safe_context);
        return M3CompactionOutcome{
            normalized.disposition,
            normalized.generation,
            boundary,
            metrics,
            normalized.error,
            normalized.error_context};
    };

    if (state() != M3ProviderState::Ready || !store_
        || boundary.operation_id == 0 || boundary.image_uuid.is_nil()
        || (!boundary.cut_dirty_epoch_known
            && boundary.cut_dirty_epoch != 0)
        || boundary.image_uuid != image_.image_uuid
        || boundary.source_generation >= kInvalidGeneration - 1
        || boundary.candidate_generation
            != boundary.source_generation + 1) {
        return fail(
            provider_error(
                L"M3Provider::reconcile_compaction",
                L"compaction recovery correlation is invalid"),
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "compaction recovery correlation rejected");
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return fail(
            live.error(),
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "compaction recovery operation context rejected");
    }
    auto recovered = recover_complete_image(
        context, config_.bundle_root, config_.manifest_name,
        config_.recovery);
    if (!recovered.ok() || !recovered.image.has_value()) {
        return fail(
            recovered.error.value_or(provider_error(
                L"M3Provider::reconcile_compaction",
                L"compaction recovery found no complete image")),
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "compaction recovery found no authoritative generation");
    }
    if (recovered.image->image_uuid != boundary.image_uuid) {
        return fail(
            provider_error(
                L"M3Provider::reconcile_compaction",
                L"compaction recovery image identity changed"),
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "compaction recovery image identity mismatch");
    }
    if (recovered.image->checkpoint_generation
        == boundary.source_generation) {
        if (session_.phase() == SessionPhase::Reconciling) {
            auto reconciliation = session_.begin_reconciliation();
            if (!reconciliation.ok()) {
                return fail(
                    reconciliation.error(),
                    CommitDisposition::CommitIndeterminate,
                    boundary.source_generation,
                    "compaction source recovery could not own reconciliation");
            }
            auto finished = reconciliation.value().finish(
                CommitDisposition::Committed);
            if (!finished.ok()) {
                return fail(
                    finished.error(),
                    CommitDisposition::CommitIndeterminate,
                    boundary.source_generation,
                    "compaction source recovery could not finish reconciliation");
            }
        } else if (session_.phase() != SessionPhase::Ready) {
            return fail(
                provider_error(
                    L"M3Provider::reconcile_compaction",
                    L"source recovery found an invalid session phase"),
                CommitDisposition::CommitIndeterminate,
                boundary.source_generation,
                "compaction source recovery session was contradictory");
        }
        cleanup_provider_retention(
            context, session_, writer_, *store_, config_,
            retention_candidates_, metrics);
        return fail(
            Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::VerificationFailed),
                L"M3Provider::reconcile_compaction",
                L"normal recovery selected the recorded source generation"},
            CommitDisposition::NotCommitted,
            boundary.source_generation,
            "compaction recovery proved source generation authoritative");
    }
    if (recovered.image->checkpoint_generation
        != boundary.candidate_generation) {
        return fail(
            Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::CommitIndeterminate),
                L"M3Provider::reconcile_compaction",
                L"normal recovery selected an unexpected generation"},
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "compaction recovery generation was contradictory");
    }
    if (!boundary.cut_dirty_epoch_known) {
        return fail(
            Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::CommitIndeterminate),
                L"M3Provider::reconcile_compaction",
                L"candidate recovery lacks the exact consistency cut"},
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "committed compaction cut proof was unavailable");
    }

    std::optional<std::array<OwnedImmutableArtifact, 2>> retired_source;
    if (image_.checkpoint_generation == boundary.source_generation) {
        retired_source = retired_artifacts(image_);
    } else if (image_.checkpoint_generation
               != boundary.candidate_generation) {
        return fail(
            provider_error(
                L"M3Provider::reconcile_compaction",
                L"live provider generation contradicts compaction recovery"),
            CommitDisposition::CommitIndeterminate,
            boundary.candidate_generation,
            "live compaction generation was contradictory");
    }

    std::optional<PublishOperationLease> reconciliation;
    if (session_.phase() == SessionPhase::Reconciling) {
        auto started = session_.begin_reconciliation();
        if (!started.ok()) {
            return fail(
                started.error(),
                CommitDisposition::CommittedNeedsReconciliation,
                boundary.candidate_generation,
                "committed compaction could not own reconciliation");
        }
        reconciliation.emplace(started.move_value());
    } else if (session_.phase() != SessionPhase::Ready) {
        return fail(
            provider_error(
                L"M3Provider::reconcile_compaction",
                L"candidate recovery found an invalid session phase"),
            CommitDisposition::CommittedNeedsReconciliation,
            boundary.candidate_generation,
            "committed compaction session was contradictory");
    }

    auto installed = install_recovered_backing_view(
        *store_, *recovered.image);
    if (!installed.ok()) {
        if (reconciliation.has_value()) {
            (void)reconciliation->finish(
                CommitDisposition::CommittedNeedsReconciliation);
        }
        return fail(
            installed.error(),
            CommitDisposition::CommittedNeedsReconciliation,
            boundary.candidate_generation,
            "committed compaction backing installation failed");
    }
    image_ = std::move(*recovered.image);
    metrics.committed_generation = image_.checkpoint_generation;
    metrics.candidate_base_bytes =
        image_.selected_manifest.base_expected_length;
    metrics.candidate_journal_bytes =
        image_.backing_identity.journal_committed_length;
    if (reconciliation.has_value()) {
        auto finished = reconciliation->finish(
            CommitDisposition::Committed);
        if (!finished.ok()) {
            return fail(
                finished.error(),
                CommitDisposition::CommittedNeedsReconciliation,
                boundary.candidate_generation,
                "committed compaction could not finish reconciliation");
        }
    }
    forget_active_retention_artifacts(retention_candidates_, image_);
    if (retired_source.has_value()) {
        remember_retention_artifacts(
            retention_candidates_, retired_source.value());
    }
    cleanup_provider_retention(
        context, session_, writer_, *store_, config_,
        retention_candidates_, metrics);
    return {
        CommitDisposition::Committed,
        image_.checkpoint_generation,
        boundary,
        metrics,
        std::nullopt,
        std::nullopt};
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
