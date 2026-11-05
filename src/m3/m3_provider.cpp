#include "src/m3/m3_provider.h"

#include "src/common/diagnostic_trace.h"
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

// **A failed checkpoint must not leave bytes behind in the journal.**
//
// The journal writer appends records while it cuts the snapshot, so a checkpoint that
// aborts part-way (measured: `SnapshotResourceExhausted`, "snapshot was aborted") has
// already grown the journal file -- 490 MB in the host write/delete/write test -- while no
// manifest references that tail.  Every later open compares the whole Windows file
// identity, size included, against the identity recorded for the last committed
// generation, so the next checkpoint fails with `opened bundle file identity differs from
// the expected identity`; after a restart it gets as far as publish and fails with
// `reopened journal identity changed before publish`.  The disk then reports
// `blocked / needs-attention / CommitIndeterminate` and can never checkpoint again, while
// the writes it accepted are still only in RAM.  Trimming the journal back to the last
// committed end on every non-committing exit keeps the identity/size invariant true.
class JournalTailRollback final {
public:
    JournalTailRollback(std::shared_ptr<IPersistenceFile> file,
                        const std::uint64_t length) noexcept
        : file_(std::move(file)), length_(length) {}
    ~JournalTailRollback() {
        if (!armed_ || !file_) return;
        try {
            (void)file_->truncate(length_);
        } catch (...) {
        }
    }
    void commit() noexcept { armed_ = false; }

    JournalTailRollback(const JournalTailRollback&) = delete;
    JournalTailRollback& operator=(const JournalTailRollback&) = delete;

private:
    std::shared_ptr<IPersistenceFile> file_;
    std::uint64_t length_{};
    bool armed_{true};
};

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
    M3CompactionBoundaryEvidence& evidence,
    const std::optional<std::uint64_t> required_override = std::nullopt) {
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
    if (required_override.has_value()) {
        required = required_override.value();
    } else if (!checked_add(required, image.geometry.virtual_bytes) ||
        !checked_add(required, limits.max_journal_committed_bytes) ||
        !checked_add(required, limits.max_io_scratch_bytes) ||
        !checked_add(required, config.store.snapshot_cow_max_bytes) ||
        !checked_add(required, 2ULL * kVhdxDataBlockBytes)) {
        return Result<void>::failure(
            provider_error(L"M3Provider::compact",
                           L"compaction peak-space calculation overflowed"));
    }
    if (required == 0) {
        return Result<void>::failure(provider_error(
            L"M3Provider::compact",
            L"compaction peak-space requirement is empty"));
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
    const auto recovery_limits = validate_limits(config.recovery.limits);
    const auto store_options = validate_dynamic_block_store_options(config.store);
    if (recovery_limits != LimitsStatus::Ok || !store_options.ok() ||
        config.request_timeout.count() <= 0) {
        // **Which of the three, with the numbers it was decided on.**  The single message
        // "provider limits are invalid" covered a recovery bound, a store bound and a timeout,
        // and the 1.0.0 large-disk report had to treat all three as possible causes at once.
        // The line goes to the trace file; the message the operation carries is unchanged.
        const std::string line =
            "M3Provider::open: invalid limits recovery_status="
            + std::to_string(static_cast<unsigned>(recovery_limits))
            + " store_error=" + std::to_string(static_cast<unsigned>(store_options.error))
            + " timeout_ms=" + std::to_string(config.request_timeout.count())
            + " virtual=" + std::to_string(config.recovery.limits.max_virtual_bytes)
            + " journal_bytes="
            + std::to_string(config.recovery.limits.max_journal_committed_bytes)
            + " journal_records=" + std::to_string(config.recovery.limits.max_journal_records)
            + " index_bytes=" + std::to_string(config.recovery.limits.max_index_cache_bytes)
            + " index_entries=" + std::to_string(config.recovery.limits.max_index_cache_entries)
            + " bat_bytes=" + std::to_string(config.recovery.limits.max_bat_cache_bytes)
            + " bat_entries=" + std::to_string(config.recovery.limits.max_bat_cache_entries)
            + " hard=" + std::to_string(config.store.hard_limit_bytes)
            + " soft=" + std::to_string(config.store.soft_limit_bytes)
            + " low=" + std::to_string(config.store.eviction_low_watermark_bytes)
            + " snapshot_batch=" + std::to_string(config.store.max_snapshot_batch_bytes)
            + " stripes=" + std::to_string(config.store.lock_stripe_count);
        diagnostic_trace(line.c_str());
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

M3Provider::PendingCompaction::PendingCompaction(
    const std::uint64_t native_operation_id,
    const std::uint64_t source_generation,
    const std::uint64_t candidate_generation,
    const std::uint64_t cut_dirty_epoch,
    M3CompactionBoundaryEvidence boundary,
    M3CompactionMetrics metrics,
    persistence::PreparedCheckpointTransaction transaction,
    std::unique_ptr<persistence::JournalWriter> journal_writer,
    std::array<persistence::OwnedImmutableArtifact, 2> candidate_artifacts) noexcept
    : native_operation_id(native_operation_id),
      source_generation(source_generation),
      candidate_generation(candidate_generation),
      cut_dirty_epoch(cut_dirty_epoch),
      boundary(std::move(boundary)),
      metrics(std::move(metrics)),
      transaction(std::move(transaction)),
      journal_writer(std::move(journal_writer)),
      candidate_artifacts(std::move(candidate_artifacts)) {}

Result<M3CompactionPrepared> M3Provider::prepare_compaction_impl(
    const OperationContext& context,
    const M3CompactionRequest request,
    const M3CompactionBoundaryHandler* boundary_handler,
    const M3CompactionCutHandler* cut_handler,
    const bool source_sized_space,
    M3CompactionBoundaryEvidence& boundary,
    M3CompactionMetrics& metrics,
    std::optional<persistence::PersistenceErrorContext>& error_context) {
    const auto target_generation = image_.checkpoint_generation + 1;
    const auto fail = [&](const Error& error) {
        return Result<M3CompactionPrepared>::failure(error);
    };

    if (state() != M3ProviderState::Ready || !store_) {
        return fail(provider_error(L"M3Provider::prepare_compaction",
                                   L"provider is not ready"));
    }
    auto live = check_operation_context(context);
    if (!live.ok()) return fail(live.error());
    if (!is_valid_barrier_for_checkpoint(request.barrier) ||
        request.operation_id == 0 ||
        pending_checkpoint_.has_value() || pending_two_phase_.has_value() ||
        pending_compaction_.has_value()) {
        return fail(provider_error(
            L"M3Provider::prepare_compaction",
            L"compaction request, barrier, or pending state is invalid"));
    }

    cleanup_provider_retention(
        context, session_, writer_, *store_, config_,
        retention_candidates_, metrics);

    // **The M5 split path prices the files it will actually write, not the
    // virtual disk.**  A sparse VHDX's `virtual_bytes` can be sixteen times its
    // on-disk size, and the generic formula's `virtual_bytes + max_journal`
    // therefore refuses a compaction that would need a fraction of that.  The
    // bound below is still a real peak: the old pair stays until commit, the
    // candidate pair is bounded by the same durable bytes, and the delta journal
    // is bounded by the store's hard limit.  The one-shot `compact` keeps the
    // historical formula so the M3 tests and their low-space proof are
    // unchanged.
    std::optional<std::uint64_t> required_override;
    if (source_sized_space) {
        const auto& limits = config_.recovery.limits;
        std::uint64_t source_total = 0;
        if (!checked_add(
                source_total,
                image_.selected_manifest.base_expected_length) ||
            !checked_add(
                source_total,
                image_.backing_identity.journal_committed_length)) {
            return fail(provider_error(
                L"M3Provider::prepare_compaction",
                L"durable source size overflowed"));
        }
        std::uint64_t required = 0;
        const auto dirty_bound = (std::min)(
            config_.store.hard_limit_bytes,
            limits.max_journal_committed_bytes);
        if (!checked_add(required, source_total) ||
            !checked_add(required, source_total) ||
            !checked_add(required, dirty_bound) ||
            !checked_add(required, limits.max_io_scratch_bytes) ||
            !checked_add(required, config_.store.snapshot_cow_max_bytes) ||
            !checked_add(required, 2ULL * kVhdxDataBlockBytes)) {
            return fail(provider_error(
                L"M3Provider::prepare_compaction",
                L"compaction peak-space calculation overflowed"));
        }
        required_override = required;
    }

    auto preflight = compaction_preflight(
        config_, writer_, image_, request.operation_id, boundary,
        required_override);
    if (!preflight.ok()) {
        metrics.source_generation = boundary.source_generation;
        metrics.source_base_bytes = boundary.source_base_bytes;
        metrics.source_journal_bytes = boundary.source_journal_bytes;
        metrics.required_peak_free_bytes = boundary.required_peak_free_bytes;
        metrics.available_free_bytes = boundary.available_free_bytes;
        return fail(preflight.error());
    }
    metrics.source_generation = boundary.source_generation;
    metrics.source_base_bytes = boundary.source_base_bytes;
    metrics.source_journal_bytes = boundary.source_journal_bytes;
    metrics.required_peak_free_bytes = boundary.required_peak_free_bytes;
    metrics.available_free_bytes = boundary.available_free_bytes;

    if (boundary_handler != nullptr) {
        Result<M3CompactionBoundaryDecision> decision =
            Result<M3CompactionBoundaryDecision>::failure(provider_error(
                L"M3Provider::prepare_compaction",
                L"compaction boundary callback did not execute"));
        try {
            decision = (*boundary_handler)(boundary);
        } catch (...) {
            decision = Result<M3CompactionBoundaryDecision>::failure(
                provider_error(L"M3Provider::prepare_compaction",
                               L"compaction boundary callback threw"));
        }
        if (!decision.ok()) return fail(decision.error());
        if (!decision.value().continue_compaction
            || decision.value().durable_operation_revision == 0
            || decision.value().durable_profile_revision == 0
            || decision.value().evidence != boundary) {
            return fail(provider_error(
                L"M3Provider::prepare_compaction",
                L"compaction boundary continue proof was denied or mismatched"));
        }
    }

    auto correlation = compaction_correlation(image_, request.operation_id);
    if (!correlation.ok()) return fail(correlation.error());
    auto journal_writer = JournalWriter::create(
        writer_options(config_.recovery.limits));
    if (!journal_writer.ok()) return fail(journal_writer.error());

    CompactionCoordinator coordinator(session_, *store_);
    auto begun = coordinator.begin(context);
    if (!begun.ok()) return fail(begun.error());
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
    if (!built.ok()) return fail(built.error());
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
    std::uint64_t cut_dirty_epoch = 0;
    delta.after_snapshot_begin = [&](
        const ConsistentSnapshotToken& token) -> Result<void> {
        if (token.source_backing_generation != boundary.source_generation) {
            return Result<void>::failure(provider_error(
                L"M3Provider::prepare_compaction",
                L"compaction snapshot returned an invalid consistency cut"));
        }
        auto cut_evidence = boundary;
        cut_evidence.cut_dirty_epoch = token.cutoff_dirty_epoch;
        cut_evidence.cut_dirty_epoch_known = true;
        if (cut_handler != nullptr) {
            Result<M3CompactionBoundaryDecision> cut_decision =
                Result<M3CompactionBoundaryDecision>::failure(provider_error(
                    L"M3Provider::prepare_compaction",
                    L"compaction cut callback did not execute"));
            try {
                cut_decision = (*cut_handler)(cut_evidence);
            } catch (...) {
                cut_decision =
                    Result<M3CompactionBoundaryDecision>::failure(
                        provider_error(
                            L"M3Provider::prepare_compaction",
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
                    L"M3Provider::prepare_compaction",
                    L"compaction cut continue proof was denied or mismatched"));
            }
        }
        boundary = cut_evidence;
        cut_dirty_epoch = token.cutoff_dirty_epoch;
        return Result<void>::success();
    };
    auto prepared = coordinator.prepare_delta(
        context, std::move(operation), delta, *journal_writer.value());
    if (!prepared.ok()) {
        error_context = prepared.failure()->outcome.error_context;
        return fail(prepared.failure()->outcome.error.value_or(provider_error(
            L"M3Provider::prepare_compaction",
            L"compaction delta prepare failed")));
    }
    auto transaction = prepared.move_transaction();
    auto final_journal_identity = open_bundle_file_for_inspection(
        config_.bundle_root, artifacts.journal_relative_name);
    if (!final_journal_identity.ok()) {
        (void)transaction.abort();
        return fail(final_journal_identity.error());
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

    pending_compaction_.emplace(PendingCompaction{
        request.operation_id,
        image_.checkpoint_generation,
        target_generation,
        cut_dirty_epoch,
        boundary,
        metrics,
        std::move(transaction),
        journal_writer.move_value(),
        candidate_artifacts});

    M3CompactionPrepared result{};
    result.boundary = boundary;
    result.boundary.cut_dirty_epoch = 0;
    result.boundary.cut_dirty_epoch_known = false;
    result.cut_dirty_epoch = cut_dirty_epoch;
    result.metrics = metrics;
    return Result<M3CompactionPrepared>::success(std::move(result));
}

Result<M3CompactionPrepared> M3Provider::prepare_compaction(
    const OperationContext& context,
    const M3CompactionRequest request) {
    std::scoped_lock lock(operation_mutex_);
    M3CompactionBoundaryEvidence boundary{};
    M3CompactionMetrics metrics{};
    std::optional<persistence::PersistenceErrorContext> error_context;
    return prepare_compaction_impl(
        context, request, nullptr, nullptr, true, boundary, metrics,
        error_context);
}

M3CompactionOutcome M3Provider::commit_compaction_impl(
    const OperationContext& context) {
    if (!pending_compaction_.has_value()) {
        return M3CompactionOutcome{
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            {},
            {},
            provider_error(L"M3Provider::commit_compaction",
                           L"no compaction is prepared to commit"),
            std::nullopt};
    }
    auto state = std::move(*pending_compaction_);
    pending_compaction_.reset();
    const auto target_generation = state.candidate_generation;
    const auto fail = [&](const Error& error,
                          const CommitDisposition disposition,
                          const std::uint64_t generation,
                          std::optional<PersistenceErrorContext> context,
                          const std::string_view operation,
                          const SessionPhase phase,
                          const std::string_view safe_context) {
        if (disposition == CommitDisposition::NotCommitted) {
            (void)state.transaction.abort();
        }
        auto normalized = preserve_or_normalize_checkpoint_failure(
            image_, state.native_operation_id, error, disposition, generation,
            std::move(context), operation, phase, safe_context);
        return M3CompactionOutcome{
            normalized.disposition,
            normalized.generation,
            state.boundary,
            state.metrics,
            normalized.error,
            normalized.error_context};
    };

    auto live = check_operation_context(context);
    if (!live.ok()) {
        return fail(live.error(), CommitDisposition::NotCommitted,
                    target_generation, std::nullopt, "compaction.publish",
                    SessionPhase::Ready,
                    "compaction operation context rejected");
    }
    auto manifest_capability = open_locked_manifest_for_mutation(writer_);
    if (!manifest_capability.ok()) {
        return fail(manifest_capability.error(),
                    CommitDisposition::NotCommitted, target_generation,
                    std::nullopt, "compaction.publish",
                    SessionPhase::Compacting,
                    "compaction manifest capability open failed");
    }
    auto manifest = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest.ok()) {
        return fail(manifest.error(), CommitDisposition::NotCommitted,
                    target_generation, std::nullopt, "compaction.publish",
                    SessionPhase::Compacting,
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
    auto published = state.transaction.publish_manifest(
        context, std::move(publish));
    if (published.disposition == CommitDisposition::CommitIndeterminate) {
        published = state.transaction.resolve_commit(
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
            L"M3Provider::commit_compaction",
            L"compaction was not committed"));
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
    const auto reconciled = state.transaction.reconcile(
        context, reconcile_request);
    if (!reconciled.ok()) {
        return fail(
            reconciled.error.value_or(provider_error(
                L"M3Provider::commit_compaction",
                L"compaction reconciliation failed")),
            reconciled.disposition, reconciled.generation,
            reconciled.error_context, "compaction.reconcile",
            SessionPhase::Reconciling,
            "compaction reconciliation failed without complete context");
    }
    state.metrics.newer_dirty_preserved = reconciled.ack.newer_dirty_preserved;

    auto refreshed = recover_complete_image(
        context, config_.bundle_root, config_.manifest_name, config_.recovery);
    if (!refreshed.ok() || !refreshed.image.has_value()) {
        return fail(
            refreshed.error.value_or(provider_error(
                L"M3Provider::commit_compaction",
                L"committed image did not reopen")),
            CommitDisposition::CommittedNeedsReconciliation,
            reconciled.generation, std::nullopt, "compaction.reconcile",
            SessionPhase::Reconciling,
            "committed compaction did not reopen");
    }
    auto retired = retired_artifacts(image_);
    image_ = std::move(*refreshed.image);
    state.metrics.committed_generation = image_.checkpoint_generation;
    state.metrics.candidate_journal_bytes =
        image_.backing_identity.journal_committed_length;

    forget_active_retention_artifacts(retention_candidates_, image_);
    remember_retention_artifacts(retention_candidates_, retired);
    cleanup_provider_retention(
        context, session_, writer_, *store_, config_,
        retention_candidates_, state.metrics);
    return {
        CommitDisposition::Committed,
        image_.checkpoint_generation,
        state.boundary,
        state.metrics,
        std::nullopt,
        std::nullopt};
}

Result<void> M3Provider::purge_retired_compaction_artifacts(
    const OperationContext& context) {
    (void)context;
    if (retention_candidates_.empty()) return Result<void>::success();
    std::vector<OwnedImmutableArtifact> kept;
    kept.reserve(retention_candidates_.size());
    for (const auto& candidate : retention_candidates_) {
        // Never delete the pair the current manifest names.
        if (candidate.relative_name
                == image_.selected_manifest.base_relative_name
            || candidate.relative_name
                == image_.selected_manifest.journal_relative_name) {
            kept.push_back(candidate);
            continue;
        }
        const auto path =
            config_.bundle_root / std::filesystem::path(candidate.relative_name);
        {
            // Identity-anchored, read-only open first.  If the file is gone or
            // is not the object the artifact record observed, this method must
            // not delete it; the generic retention path will try again later.
            auto opened = open_bundle_file_for_inspection(
                config_.bundle_root, candidate.relative_name,
                candidate.windows_identity);
            if (!opened.ok()) {
                diagnostic_trace(
                    ("compaction.purge: kept " + candidate.relative_name
                     + " (identity or open check failed)").c_str());
                kept.push_back(candidate);
                continue;
            }
        }
        bool deleted = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (DeleteFileW(path.c_str()) != 0) {
                deleted = true;
                break;
            }
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                deleted = true;
                break;
            }
            Sleep(100);
        }
        if (deleted) {
            diagnostic_trace(
                ("compaction.purge: deleted " + candidate.relative_name)
                    .c_str());
        } else {
            diagnostic_trace(
                ("compaction.purge: kept " + candidate.relative_name
                 + " (delete refused)")
                    .c_str());
            kept.push_back(candidate);
        }
    }
    retention_candidates_ = std::move(kept);
    return Result<void>::success();
}

M3CompactionOutcome M3Provider::commit_compaction(
    const OperationContext& context,
    const std::uint64_t native_operation_id,
    const std::uint64_t cut_dirty_epoch) {
    std::scoped_lock lock(operation_mutex_);
    if (!pending_compaction_.has_value() ||
        pending_compaction_->native_operation_id != native_operation_id ||
        pending_compaction_->cut_dirty_epoch != cut_dirty_epoch) {
        return M3CompactionOutcome{
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            {},
            {},
            provider_error(L"M3Provider::commit_compaction",
                           L"the commit does not name the prepared compaction"),
            std::nullopt};
    }
    auto outcome = commit_compaction_impl(context);
    if (outcome.ok()) {
        (void)purge_retired_compaction_artifacts(context);
    }
    return outcome;
}

Result<void> M3Provider::abort_compaction(
    const std::uint64_t native_operation_id) {
    std::scoped_lock lock(operation_mutex_);
    if (!pending_compaction_.has_value()) {
        return Result<void>::success();
    }
    if (pending_compaction_->native_operation_id != native_operation_id) {
        return Result<void>::failure(provider_error(
            L"M3Provider::abort_compaction",
            L"the abort does not name the prepared compaction"));
    }
    auto state = std::move(*pending_compaction_);
    pending_compaction_.reset();
    return state.transaction.abort();
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

M3CheckpointOutcome M3Provider::checkpoint_prepare_impl(
    const OperationContext& context,
    const VolumeBarrierKind barrier,
    const std::uint64_t operation_id) {
    const auto target_generation = image_.checkpoint_generation + 1;
    if (operation_id == 0) {
        // Zero is the "no operation" encoding and cannot identify a prepare.
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"a zero native operation id cannot prepare"),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint native operation id must not be zero");
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return provider_checkpoint_failure(
            image_, operation_id, live.error(),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint operation context rejected");
    }
    if (!is_valid_barrier_for_checkpoint(barrier) || !store_ ||
        pending_compaction_.has_value()) {
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"checkpoint barrier or store is invalid"),
            CommitDisposition::NotCommitted, target_generation,
            "checkpoint.prepare", SessionPhase::Ready,
            "checkpoint barrier, store, or pending compaction is invalid");
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
    // Drop any tail a previous aborted attempt left, then guarantee this attempt leaves
    // none behind unless it becomes a pending transaction.  The committed length is the
    // only end any manifest names; bytes past it are unreachable by definition.
    const std::uint64_t committed_before =
        image_.backing_identity.journal_committed_length;
    {
        auto current_length = journal_file.value()->length();
        if (current_length.ok() && current_length.value() > committed_before) {
            auto trimmed = journal_file.value()->truncate(committed_before);
            if (!trimmed.ok()) {
                return provider_checkpoint_failure(
                    image_, operation_id, trimmed.error(),
                    CommitDisposition::NotCommitted, target_generation,
                    "checkpoint.prepare", SessionPhase::Ready,
                    "the uncommitted journal tail could not be removed");
            }
        }
    }
    JournalTailRollback journal_rollback(journal_file.value(), committed_before);
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
    // Freeze the candidate here, not at commit: the slot and the digest of the
    // exact candidate bytes are handed back to the caller and required again at
    // commit, so a commit cannot publish an object other than the one the
    // caller durably recorded.
    auto candidate = persistence::prepare_manifest_candidate(
        image_.selected_manifest, transaction.checkpoint(),
        publish.committed_time_100ns);
    if (!candidate.ok()) {
        const auto error = candidate.error();
        (void)transaction.abort();
        return provider_checkpoint_failure(
            image_, operation_id, error, CommitDisposition::NotCommitted,
            target_generation, "checkpoint.prepare",
            SessionPhase::Checkpointing,
            "checkpoint candidate could not be frozen");
    }
    auto digest = sha256_digest(candidate.value().bytes);
    if (!digest.ok()) {
        const auto error = digest.error();
        (void)transaction.abort();
        return provider_checkpoint_failure(
            image_, operation_id, error, CommitDisposition::NotCommitted,
            target_generation, "checkpoint.prepare",
            SessionPhase::Checkpointing,
            "checkpoint candidate digest could not be computed");
    }
    PreparedTwoPhaseCorrelation frozen{};
    frozen.native_operation_id = operation_id;
    frozen.prior_generation = image_.checkpoint_generation;
    frozen.candidate_generation = target_generation;
    frozen.manifest_slot = candidate.value().slot.slot_index;
    frozen.candidate_bytes_digest = digest.value();
    frozen.publish = std::move(publish);
    // The journal now belongs to the pending transaction: from here a failure must leave
    // the prepared bytes alone (the commit boundary decides their fate), so the rollback
    // guard stops owning the file's length.
    journal_rollback.commit();
    pending_checkpoint_.emplace(std::move(transaction));
    pending_two_phase_ = std::move(frozen);
    // No error: a prepared checkpoint is pending and nothing has been published.
    return M3CheckpointOutcome{
        CommitDisposition::NotCommitted, target_generation, std::nullopt,
        std::nullopt};
}

M3CheckpointOutcome M3Provider::publish_prepared_checkpoint(
    const OperationContext& context) {
    if (!pending_checkpoint_.has_value() || !pending_two_phase_.has_value()) {
        const auto operation_id = pending_two_phase_.has_value()
            ? pending_two_phase_->native_operation_id
            : image_.checkpoint_generation + 3902;
        return provider_checkpoint_failure(
            image_, operation_id,
            provider_error(L"M3Provider::checkpoint",
                           L"no prepared checkpoint is pending"),
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            "checkpoint.publish", SessionPhase::Ready,
            "no prepared checkpoint is pending");
    }
    auto& transaction = *pending_checkpoint_;
    const auto operation_id = pending_two_phase_->native_operation_id;
    auto publish = std::move(pending_two_phase_->publish);
    auto published = transaction.publish_manifest(context, std::move(publish));
    if (published.disposition != CommitDisposition::Committed &&
        published.disposition != CommitDisposition::CommitIndeterminate &&
        published.disposition !=
            CommitDisposition::CommittedNeedsReconciliation) {
        (void)transaction.abort();
        pending_two_phase_.reset();
        return preserve_or_normalize_checkpoint_failure(
            image_, operation_id,
            published.error.value_or(provider_error(
                L"M3Provider::checkpoint", L"checkpoint was not committed")),
            published.disposition, published.generation,
            published.error_context, "checkpoint.publish",
            SessionPhase::Checkpointing,
            "checkpoint publication failed without complete context");
    }
    // The manifest is on disk. The prepared correlation has served its purpose:
    // from here the transaction is reconciled or retried, never re-prepared,
    // and stop must resume it rather than abort it.
    pending_two_phase_.reset();
    return resume_pending_checkpoint(context);
}

M3CheckpointOutcome M3Provider::checkpoint_impl(
    const OperationContext& context,
    const VolumeBarrierKind barrier) {
    // M4 one-shot: prepare and publish in one call, keeping the M4 native id.
    const auto prepared = checkpoint_prepare_impl(
        context, barrier, image_.checkpoint_generation + 3902);
    if (prepared.error.has_value()) return prepared;
    return publish_prepared_checkpoint(context);
}

Result<M3CheckpointPrepared> M3Provider::prepare_checkpoint(
    const M3CheckpointPrepareRequest& request) {
    std::scoped_lock lock(operation_mutex_);
    if (state() != M3ProviderState::Ready) {
        return Result<M3CheckpointPrepared>::failure(provider_error(
            L"M3Provider::prepare_checkpoint", L"provider is not ready"));
    }
    if (pending_checkpoint_.has_value() || pending_two_phase_.has_value()) {
        return Result<M3CheckpointPrepared>::failure(provider_error(
            L"M3Provider::prepare_checkpoint",
            L"a checkpoint is already pending"));
    }
    const auto prepared = checkpoint_prepare_impl(
        request.context, request.barrier, request.native_operation_id);
    if (prepared.error.has_value()) {
        return Result<M3CheckpointPrepared>::failure(*prepared.error);
    }
    const auto& frozen = *pending_two_phase_;
    return Result<M3CheckpointPrepared>::success(M3CheckpointPrepared{
        frozen.prior_generation,
        frozen.candidate_generation,
        frozen.manifest_slot,
        frozen.candidate_bytes_digest});
}

M3CheckpointOutcome M3Provider::commit_checkpoint(
    const M3CheckpointCommitRequest& request) {
    std::scoped_lock lock(operation_mutex_);
    if (!pending_checkpoint_.has_value() || !pending_two_phase_.has_value()) {
        return provider_checkpoint_failure(
            image_, request.native_operation_id,
            provider_error(L"M3Provider::commit_checkpoint",
                           L"no prepared checkpoint is pending"),
            CommitDisposition::NotCommitted,
            image_.checkpoint_generation + 1,
            "checkpoint.publish", SessionPhase::Ready,
            "no prepared checkpoint is pending");
    }
    const auto& frozen = *pending_two_phase_;
    if (request.native_operation_id != frozen.native_operation_id ||
        request.manifest_slot != frozen.manifest_slot ||
        request.candidate_bytes_digest != frozen.candidate_bytes_digest) {
        // The request does not describe the candidate that was frozen, so
        // publishing would commit an object the caller never recorded. Fail
        // closed and leave the prepared transaction untouched for the caller to
        // abort deliberately.
        return provider_checkpoint_failure(
            image_, frozen.native_operation_id,
            provider_error(L"M3Provider::commit_checkpoint",
                           L"commit does not match the prepared candidate"),
            CommitDisposition::NotCommitted, frozen.candidate_generation,
            "checkpoint.publish", SessionPhase::Checkpointing,
            "commit request does not match the prepared checkpoint candidate");
    }
    return publish_prepared_checkpoint(request.context);
}

Result<void> M3Provider::abort_checkpoint(
    const std::uint64_t native_operation_id) {
    std::scoped_lock lock(operation_mutex_);
    if (!pending_checkpoint_.has_value() || !pending_two_phase_.has_value()) {
        return Result<void>::failure(provider_error(
            L"M3Provider::abort_checkpoint",
            L"no prepared checkpoint is pending"));
    }
    if (pending_two_phase_->native_operation_id != native_operation_id) {
        return Result<void>::failure(provider_error(
            L"M3Provider::abort_checkpoint",
            L"abort does not match the prepared checkpoint"));
    }
    auto& transaction = *pending_checkpoint_;
    auto aborted = transaction.abort();
    pending_checkpoint_.reset();
    pending_two_phase_.reset();
    return aborted;
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
    if (pending_compaction_.has_value()) {
        // An unpublished compaction is an open two-phase boundary.  Stopping
        // with it outstanding must discard it rather than publish a candidate
        // the service never authorized.  This is the same rule the checkpoint
        // prepare path follows, and it releases the snapshot and the publish
        // lease without touching a manifest slot.
        auto& transaction = pending_compaction_->transaction;
        const auto discarded = transaction.abort();
        pending_compaction_.reset();
        if (!discarded.ok()) {
            persistence_ready = false;
            remember(discarded.error(), std::nullopt, "stop.compaction",
                     SessionPhase::Compacting,
                     "unpublished prepared compaction could not be discarded");
        }
    }
    if (pending_two_phase_.has_value()) {
        // A two-phase prepare is outstanding and its manifest was never
        // published. Resuming it would commit a candidate the caller never
        // authorised, so discard it instead: that releases the snapshot and the
        // publish lease without touching any manifest slot. `pending_two_phase_`
        // is cleared by commit precisely so this test distinguishes
        // "prepared, unpublished" from "published, awaiting reconcile".
        auto& transaction = *pending_checkpoint_;
        const auto discarded = transaction.abort();
        pending_checkpoint_.reset();
        pending_two_phase_.reset();
        if (!discarded.ok()) {
            persistence_ready = false;
            remember(discarded.error(), std::nullopt, "stop.prepare",
                     SessionPhase::Checkpointing,
                     "unpublished prepared checkpoint could not be discarded");
        }
    } else if (pending_checkpoint_.has_value()) {
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

std::uint64_t M3Provider::memory_budget_bytes() const {
    std::scoped_lock lock(operation_mutex_);
    if (store_) return store_->options().hard_limit_bytes;
    // After the store has gone, the budget the provider was opened with is still the
    // truth about what this profile was allowed to use.
    return config_.store.hard_limit_bytes;
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

Result<persistence::CheckpointRecoveryInspection> M3Provider::inspect_checkpoint(
    const OperationContext& context,
    M3ProviderConfig config,
    const persistence::CheckpointRecoveryCorrelation& expected) {
    // Deliberately no writer lock, no store, no transport and no runtime: this
    // is the decision that precedes opening the bundle, so taking the writer
    // lock here would invert the ordering it exists to enforce.
    persistence::ImageInspectOptions options{};
    options.manifest_relative_name = config.manifest_name;
    options.recovery = config.recovery;
    return persistence::inspect_checkpoint(
        context, config.bundle_root, options, expected);
}

} // namespace myrd::m3
