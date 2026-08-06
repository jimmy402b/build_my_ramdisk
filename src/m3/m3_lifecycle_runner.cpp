#include "src/m3/m3_lifecycle_runner.h"

#include "src/persistence/checkpoint_coordinator.h"
#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/writer_lock.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace myrd::m3 {
namespace {

using namespace persistence;

constexpr std::string_view kBaseName = "base-0000.vhdx";
constexpr std::string_view kBaseTemporaryName = "base-0000.create.tmp";
constexpr std::string_view kJournalName = "journal-e0001.rdj";

JournalFileIdentity make_journal_identity() noexcept;

std::string compact_base_name(const std::uint64_t generation) {
    return "base-g0000-c" + std::to_string(generation) + ".vhdx";
}

std::string compact_base_temporary_name(const std::uint64_t generation) {
    return "base-g0000-c" + std::to_string(generation) + ".tmp";
}

std::string compact_journal_name(const std::uint64_t generation) {
    return "journal-e0002-c" + std::to_string(generation) + ".rdj";
}

ImageUuid make_image_uuid() noexcept {
    ImageUuid result{};
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        result.bytes[index] = static_cast<std::byte>(0x10U + index);
    }
    return result;
}

VhdxWriterIdentity make_base_identity() noexcept {
    VhdxWriterIdentity result{};
    for (std::size_t index = 0; index < 16; ++index) {
        result.file_identity.file_write_guid[index] =
            static_cast<std::byte>(0x31U + index);
        result.file_identity.data_write_guid[index] =
            static_cast<std::byte>(0x51U + index);
        result.virtual_disk_id[index] = static_cast<std::byte>(0x71U + index);
    }
    return result;
}

VhdxWriterIdentity make_compact_base_identity(
    const RecoveredImage& image,
    const std::uint64_t generation) noexcept {
    auto result = make_base_identity();
    result.virtual_disk_id = image.backing_identity.virtual_disk_id;
    result.file_identity.file_write_guid.fill(std::byte{});
    result.file_identity.data_write_guid.fill(std::byte{});
    for (std::size_t index = 0; index < sizeof(generation); ++index) {
        result.file_identity.file_write_guid[index] = static_cast<std::byte>(
            (generation >> (index * 8U)) & 0xffU);
        result.file_identity.data_write_guid[index] = static_cast<std::byte>(
            (generation >> (index * 8U)) & 0xffU);
    }
    result.file_identity.file_write_guid[8] = std::byte{0xb1};
    result.file_identity.data_write_guid[8] = std::byte{0xc1};
    return result;
}

JournalFileIdentity make_compact_journal_identity(
    const std::uint64_t generation) noexcept {
    auto result = make_journal_identity();
    result.journal_uuid.fill(std::byte{});
    for (std::size_t index = 0; index < sizeof(generation); ++index) {
        result.journal_uuid[index] = static_cast<std::byte>(
            (generation >> (index * 8U)) & 0xffU);
    }
    result.journal_uuid[8] = std::byte{0xa1};
    return result;
}

JournalFileIdentity make_journal_identity() noexcept {
    JournalFileIdentity result{};
    for (std::size_t index = 0; index < 16; ++index) {
        result.journal_uuid[index] = static_cast<std::byte>(0x91U + index);
    }
    return result;
}

M3LifecycleOutcome make_outcome(
    const M3CliCommand command,
    const bool ok,
    const CommitDisposition disposition,
    const std::uint64_t generation,
    const std::optional<Error>& error = std::nullopt) {
    M3LifecycleOutcome result{};
    result.exit_code = ok ? 0 : 1;
    result.disposition = disposition;
    result.generation = generation;
    result.error = error;
    result.json = "{\"schema_version\":1,\"command\":\"";
    result.json += command_name(command);
    result.json += "\",\"ok\":";
    result.json += ok ? "true" : "false";
    result.json += ",\"commit_disposition\":\"";
    result.json += to_string(disposition);
    result.json += "\",\"generation\":";
    result.json += std::to_string(generation);
    if (error.has_value()) {
        result.json += ",\"error_domain\":" +
            std::to_string(static_cast<unsigned>(error->domain));
        result.json += ",\"error_code\":" + std::to_string(error->code);
    }
    result.json += "}";
    if (error.has_value()) result.diagnostics = "m3 command failed\n";
    return result;
}

M3LifecycleOutcome failure(
    const M3CliCommand command,
    const Error& error,
    const CommitDisposition disposition = CommitDisposition::NotCommitted,
    const std::uint64_t generation = 0) {
    return make_outcome(command, false, disposition, generation, error);
}

Result<CompletePersistenceErrorContext> correlation(
    const char* operation,
    const RecoveredImage& image,
    const std::uint64_t operation_id) {
    PersistenceErrorContext context{};
    context.operation = operation;
    context.code = PersistenceErrorCode::InvalidFormat;
    context.phase = SessionPhase::Ready;
    context.operation_id = operation_id;
    context.image_uuid = image.image_uuid;
    context.generation = image.checkpoint_generation + 1;
    context.disposition = CommitDisposition::NotCommitted;
    context.safe_context = "M3 local lifecycle operation";
    auto complete = CompletePersistenceErrorContext::create(std::move(context));
    if (!complete.has_value()) {
        return Result<CompletePersistenceErrorContext>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::FatalInternalInvariant),
            L"m3_lifecycle_runner", L"failed to create error correlation"});
    }
    return Result<CompletePersistenceErrorContext>::success(
        std::move(*complete));
}

RecoveryOptions recovery_options(const M3CliConfig& config) {
    return RecoveryOptions{config.persistence_limits};
}

JournalWriterOptions journal_writer_options(
    const M3CliConfig& config) {
    JournalWriterOptions options{};
    options.index_builder.reader_cache = {
        config.persistence_limits.max_index_cache_bytes,
        config.persistence_limits.max_index_cache_entries};
    options.index_builder.max_new_nodes =
        (std::min)(config.persistence_limits.max_journal_records,
                   config.persistence_limits.max_index_cache_bytes /
                       kIndexNodeBytes);
    options.index_builder.max_plan_image_bytes =
        config.persistence_limits.max_index_cache_bytes;
    options.max_checkpoint_records =
        config.persistence_limits.max_journal_records;
    options.max_checkpoint_bytes =
        config.persistence_limits.max_journal_committed_bytes;
    return options;
}

VhdxWriterLimits vhdx_limits(const M3CliConfig& config) {
    const auto payloads = config.persistence_limits.max_virtual_bytes /
        kVhdxDataBlockBytes + 1;
    return {payloads, payloads,
            (std::max)(config.persistence_limits.max_io_scratch_bytes,
                       static_cast<std::uint64_t>(kVhdxDataBlockBytes))};
}

Result<RecoveredImage> recover(
    const OperationContext& context,
    const M3CliConfig& config) {
    const auto result = recover_complete_image(
        context, config.bundle_root, config.manifest_name,
        recovery_options(config));
    if (!result.ok() || !result.image.has_value()) {
        return Result<RecoveredImage>::failure(result.error.value_or(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"m3_lifecycle_runner", L"recovery did not select a complete image"}));
    }
    return Result<RecoveredImage>::success(*result.image);
}

Result<RecoveredImage> create_generation_zero(
    const OperationContext& context,
    const M3CliConfig& config) {
    if (!config.geometry.has_value()) {
        return Result<RecoveredImage>::failure({
            ErrorDomain::usage, 2, L"m3_lifecycle_runner",
            L"create requires geometry"});
    }
    GenerationZeroCreateRequest request{};
    request.bundle_root = config.bundle_root;
    request.manifest_relative_name = config.manifest_name;
    request.base_temporary_name = std::string{kBaseTemporaryName};
    request.base_relative_name = std::string{kBaseName};
    request.journal_relative_name = std::string{kJournalName};
    request.geometry = *config.geometry;
    request.image_uuid = make_image_uuid();
    request.base_identity = make_base_identity();
    request.journal_identity = make_journal_identity();
    request.journal_epoch = 1;
    request.operation_id = 1;
    request.created_time_100ns = 1;
    request.vhdx_limits = vhdx_limits(config);
    request.recovery = recovery_options(config);
    return create_generation_zero_image(context, request);
}

Result<void> close_store_and_session(
    const OperationContext& context,
    DynamicBlockStore& store,
    ImageSession& session) {
    store.close();
    const auto closed = session.close(context);
    if (!closed.ok()) return Result<void>::failure(closed.error());
    return Result<void>::success();
}

Result<std::shared_ptr<IPersistenceFile>> open_journal_for_checkpoint(
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
    auto file = open_validated_persistence_file(capability.move_value());
    if (!file.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            file.error());
    }
    JournalHeaderBytes bytes{};
    auto read = file.value()->read_exact(context, 0, bytes);
    if (!read.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            read.error());
    }
    auto decoded = decode_journal_header(bytes);
    if (!decoded.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            decoded.error());
    }
    header = decoded.move_value();
    return Result<std::shared_ptr<IPersistenceFile>>::success(
        std::shared_ptr<IPersistenceFile>(file.move_value()));
}

M3LifecycleOutcome finish_mutation(
    const OperationContext& context,
    const M3CliConfig& config,
    std::unique_ptr<DynamicBlockStore> store,
    ImageSession& session,
    BundleWriterLock& writer,
    const CommitDisposition disposition,
    const std::uint64_t generation,
    const std::optional<Error>& operation_error = std::nullopt) {
    const auto closed = close_store_and_session(
        context, *store, session);
    const auto writer_closed = writer.close();
    if (!closed.ok()) {
        auto result = failure(
            config.command, closed.error(), disposition, generation);
        result.cleanup_succeeded = false;
        return result;
    }
    if (!writer_closed.ok()) {
        auto result = failure(
            config.command, writer_closed.error(), disposition, generation);
        result.cleanup_succeeded = false;
        return result;
    }
    if (operation_error.has_value()) {
        return failure(config.command, *operation_error, disposition, generation);
    }
    return make_outcome(config.command, true, disposition, generation);
}

M3LifecycleOutcome run_checkpoint(
    const OperationContext& context,
    const M3CliConfig& config,
    const bool compact) {
    auto image = recover(context, config);
    if (!image.ok()) return failure(config.command, image.error());
    if (!config.store_options.has_value() || !config.barrier.has_value()) {
        return failure(config.command, Error{
            ErrorDomain::usage, 2, L"m3_lifecycle_runner",
            L"checkpoint requires store and barrier options"},
            CommitDisposition::NotCommitted,
            image.value().checkpoint_generation);
    }
    auto store = open_recovered_dynamic_store(
        image.value(), *config.store_options);
    if (!store.ok()) return failure(config.command, store.error());
    auto writer = acquire_bundle_writer_lock(
        config.bundle_root, config.manifest_name);
    if (!writer.ok()) return failure(config.command, writer.error());
    auto writer_lock = writer.move_value();
    auto journal_writer = JournalWriter::create(journal_writer_options(config));
    if (!journal_writer.ok()) {
        const auto error = journal_writer.error();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }
    ImageSession session = ImageSession::ready();

    auto complete = correlation(
        compact ? "compaction.prepare" : "checkpoint.prepare",
        image.value(), compact ? 2 : 2);
    if (!complete.ok()) {
        const auto error = complete.error();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }

    if (!compact) {
        JournalHeader header{};
        auto journal = open_journal_for_checkpoint(
            context, writer_lock, image.value(), header);
        if (!journal.ok()) {
            const auto error = journal.error();
            (void)writer_lock.close();
            return failure(config.command, error,
                           CommitDisposition::NotCommitted,
                           image.value().checkpoint_generation);
        }
        JournalCheckpointRequest journal_request{};
        journal_request.file = journal.value();
        journal_request.file_identity = image.value().backing_identity
            .journal_windows_identity;
        journal_request.header = header;
        journal_request.previous_generation = image.value().checkpoint_generation;
        journal_request.previous_digest = image.value().backing_identity
            .commit.chain_digest;
        journal_request.committed_length = image.value().backing_identity
            .journal_committed_length;
        journal_request.root = image.value().backing_identity.root;
        journal_request.operation_id = 2;
        CheckpointPrepareRequest request{std::move(complete.value())};
        request.volume_barrier = config.barrier;
        request.journal = std::move(journal_request);
        request.snapshot_batch_bytes = (std::min)(
            config.persistence_limits.max_io_scratch_bytes,
            config.store_options->max_snapshot_batch_bytes);
        CheckpointCoordinator coordinator(session, *store.value(),
                                           *journal_writer.value());
        auto prepared = coordinator.prepare(context, request);
        if (!prepared.ok()) {
            const auto outcome = prepared.failure()->outcome;
            (void)writer_lock.close();
            return failure(config.command,
                           outcome.error.value_or(Error{
                               ErrorDomain::persistence,
                               static_cast<std::uint32_t>(
                                   PersistenceErrorCode::VerificationFailed),
                               L"m3_lifecycle_runner",
                               L"checkpoint preparation failed"}),
                           outcome.disposition, outcome.generation);
        }
        auto transaction = prepared.move_transaction();
        auto manifest_capability = open_locked_manifest_for_mutation(writer_lock);
        if (!manifest_capability.ok()) {
            const auto error = manifest_capability.error();
            (void)transaction.abort();
            (void)writer_lock.close();
            return failure(config.command, error,
                           CommitDisposition::NotCommitted,
                           image.value().checkpoint_generation);
        }
        auto manifest = open_validated_persistence_file(
            manifest_capability.move_value());
        if (!manifest.ok()) {
            const auto error = manifest.error();
            (void)transaction.abort();
            (void)writer_lock.close();
            return failure(config.command, error,
                           CommitDisposition::NotCommitted,
                           image.value().checkpoint_generation);
        }
        CheckpointManifestPublishRequest publish{};
        publish.manifest_file = manifest.move_value();
        publish.active_slot = image.value().selected_manifest;
        publish.committed_time_100ns = image.value().checkpoint_generation + 2;
        publish.complete_validation = CompleteCandidateValidation{
            config.bundle_root, config.manifest_name, recovery_options(config)};
        auto published = transaction.publish_manifest(context, std::move(publish));
        if (published.disposition == CommitDisposition::CommitIndeterminate) {
            published = transaction.resolve_commit(context,
                {FilesystemCommitResolution{config.bundle_root,
                                             config.manifest_name,
                                             recovery_options(config),
                                             &writer_lock}});
        }
        if (published.disposition != CommitDisposition::Committed &&
            published.disposition !=
                CommitDisposition::CommittedNeedsReconciliation) {
            const auto error = published.error.value_or(Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
                L"m3_lifecycle_runner", L"checkpoint publication failed"});
            (void)transaction.abort();
            (void)writer_lock.close();
            return failure(config.command, error, published.disposition,
                           published.generation);
        }
        const auto reconciled = transaction.reconcile(context);
        if (!reconciled.ok()) {
            const auto error = reconciled.error.value_or(Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
                L"m3_lifecycle_runner", L"checkpoint reconciliation failed"});
            (void)writer_lock.close();
            return failure(config.command, error, reconciled.disposition,
                           reconciled.generation);
        }
        auto finished_store = store.move_value();
        return finish_mutation(context, config, std::move(finished_store),
                               session, writer_lock, reconciled.disposition,
                               reconciled.generation);
    }

    CompactionCoordinator coordinator(session, *store.value());
    auto begun = coordinator.begin(context);
    if (!begun.ok()) {
        const auto error = begun.error();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }
    auto operation = begun.move_value();
    const auto generation = image.value().checkpoint_generation + 1;
    CompactionArtifactBuildRequest artifacts{};
    artifacts.writer_lock = &writer_lock;
    artifacts.active_slot = image.value().selected_manifest;
    artifacts.base_temporary_name = compact_base_temporary_name(generation);
    artifacts.base_relative_name = compact_base_name(generation);
    artifacts.base_identity = make_compact_base_identity(image.value(), generation);
    artifacts.journal_relative_name = compact_journal_name(generation);
    artifacts.journal_identity = make_compact_journal_identity(generation);
    artifacts.journal_epoch = image.value().journal_epoch + 1;
    artifacts.operation_id = 2;
    artifacts.extent_options.max_extent_bytes = kVhdxDataBlockBytes;
    artifacts.vhdx_limits = vhdx_limits(config);
    artifacts.limits.max_live_extents = artifacts.vhdx_limits.max_candidate_payloads;
    artifacts.offline_vhdx_validator = [](const OperationContext&,
                                          const std::filesystem::path& path)
        -> Result<WindowsFileIdentity> {
        auto opened = open_bundle_file_for_inspection(
            path.parent_path(), path.filename().string());
        if (!opened.ok()) return Result<WindowsFileIdentity>::failure(opened.error());
        return Result<WindowsFileIdentity>::success(opened.value().identity());
    };
    auto built = operation.build_artifacts(context, artifacts);
    if (!built.ok()) {
        const auto error = built.error();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }
    CompactionDeltaPrepareRequest delta{std::move(complete.value())};
    delta.volume_barrier = config.barrier;
    delta.snapshot_batch_bytes = (std::min)(
        config.persistence_limits.max_io_scratch_bytes,
        config.store_options->max_snapshot_batch_bytes);
    auto prepared = coordinator.prepare_delta(
        context, std::move(operation), delta, *journal_writer.value());
    if (!prepared.ok()) {
        const auto outcome = prepared.failure()->outcome;
        (void)writer_lock.close();
        return failure(config.command,
                       outcome.error.value_or(Error{
                           ErrorDomain::persistence,
                           static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
                           L"m3_lifecycle_runner", L"compaction preparation failed"}),
                       outcome.disposition, outcome.generation);
    }
    auto transaction = prepared.move_transaction();
    auto manifest_capability = open_locked_manifest_for_mutation(writer_lock);
    if (!manifest_capability.ok()) {
        const auto error = manifest_capability.error();
        (void)transaction.abort();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }
    auto manifest = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest.ok()) {
        const auto error = manifest.error();
        (void)transaction.abort();
        (void)writer_lock.close();
        return failure(config.command, error, CommitDisposition::NotCommitted,
                       image.value().checkpoint_generation);
    }
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest.move_value();
    publish.active_slot = image.value().selected_manifest;
    publish.committed_time_100ns = generation + 2;
    publish.complete_validation = CompleteCandidateValidation{
        config.bundle_root, config.manifest_name, recovery_options(config)};
    auto published = transaction.publish_manifest(context, std::move(publish));
    if (published.disposition == CommitDisposition::CommitIndeterminate) {
        published = transaction.resolve_commit(context,
            {FilesystemCommitResolution{config.bundle_root,
                                         config.manifest_name,
                                         recovery_options(config),
                                         &writer_lock}});
    }
    if (published.disposition != CommitDisposition::Committed &&
        published.disposition !=
            CommitDisposition::CommittedNeedsReconciliation) {
        const auto error = published.error.value_or(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"m3_lifecycle_runner", L"compaction publication failed"});
        (void)transaction.abort();
        (void)writer_lock.close();
        return failure(config.command, error, published.disposition,
                       published.generation);
    }
    const auto reconciled = transaction.reconcile(context);
    if (!reconciled.ok()) {
        const auto error = reconciled.error.value_or(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"m3_lifecycle_runner", L"compaction reconciliation failed"});
        (void)writer_lock.close();
        return failure(config.command, error, reconciled.disposition,
                       reconciled.generation);
    }
    auto finished_store = store.move_value();
    return finish_mutation(context, config, std::move(finished_store),
                           session, writer_lock, reconciled.disposition,
                           reconciled.generation);
}

M3LifecycleOutcome run_read_only(
    const OperationContext& context,
    const M3CliConfig& config) {
    const auto options = ImageInspectOptions{
        config.manifest_name, recovery_options(config)};
    if (config.command == M3CliCommand::Inspect) {
        const auto inspected = inspect_image(context, config.bundle_root, options);
        if (!inspected.ok()) return failure(config.command, inspected.error());
        const auto& value = inspected.value();
        if (!value.selected.has_value()) {
            return failure(config.command, value.rejection.value_or(Error{
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
                L"m3_lifecycle_runner", L"inspection selected no image"}));
        }
        return make_outcome(config.command, true,
                            CommitDisposition::NotCommitted,
                            value.selected->checkpoint_generation);
    }
    const auto depth = config.full_verify ? VerificationDepth::Full
                                          : VerificationDepth::Structural;
    const auto verified = verify_image(
        context, config.bundle_root, depth, options);
    if (!verified.ok()) return failure(config.command, verified.error());
    const auto& value = verified.value();
    if (!value.verified || !value.inspection.selected.has_value()) {
        return failure(config.command, value.failure.value_or(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"m3_lifecycle_runner", L"image verification failed"}));
    }
    return make_outcome(config.command, true,
                        CommitDisposition::NotCommitted,
                        value.inspection.selected->checkpoint_generation);
}

M3LifecycleOutcome run_recover_test(
    const OperationContext& context,
    const M3CliConfig& config) {
    if (config.injected_phase.has_value()) {
        const auto error = Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"m3_lifecycle_runner", L"explicit recovery test phase injected"};
        return failure(config.command, error);
    }
    auto image = recover(context, config);
    if (!image.ok()) return failure(config.command, image.error());
    if (!config.store_options.has_value()) {
        return failure(config.command, Error{
            ErrorDomain::usage, 2, L"m3_lifecycle_runner",
            L"recover-test requires store options"});
    }
    auto store = open_recovered_dynamic_store(
        image.value(), *config.store_options);
    if (!store.ok()) return failure(config.command, store.error());
    ImageSession session = ImageSession::ready();
    const auto closed = close_store_and_session(context, *store.value(), session);
    if (!closed.ok()) return failure(config.command, closed.error(),
                                     CommitDisposition::NotCommitted,
                                     image.value().checkpoint_generation);
    return make_outcome(config.command, true,
                        CommitDisposition::NotCommitted,
                        image.value().checkpoint_generation);
}

} // namespace

M3LifecycleOutcome run_m3_lifecycle(
    const OperationContext& context,
    const M3CliConfig& config) {
    if (config.command == M3CliCommand::Help) {
        return make_outcome(config.command, true,
                            CommitDisposition::NotCommitted, 0);
    }
    if (config.command == M3CliCommand::Create) {
        const auto created = create_generation_zero(context, config);
        if (!created.ok()) return failure(config.command, created.error());
        return make_outcome(config.command, true,
                            CommitDisposition::NotCommitted,
                            created.value().checkpoint_generation);
    }
    if (config.command == M3CliCommand::Inspect ||
        config.command == M3CliCommand::Verify) {
        return run_read_only(context, config);
    }
    if (config.command == M3CliCommand::RecoverTest) {
        return run_recover_test(context, config);
    }
    if (config.command == M3CliCommand::Checkpoint) {
        return run_checkpoint(context, config, false);
    }
    if (config.command == M3CliCommand::Compact) {
        return run_checkpoint(context, config, true);
    }
    if (config.command == M3CliCommand::FaultMatrix) {
        // The local fake matrix deliberately uses the same recovery seam as
        // recover-test. The injected phase is explicit CLI input, so a
        // requested fault is reported as a stable failed outcome rather than
        // being mistaken for a successful production operation.
        return run_recover_test(context, config);
    }
    return failure(config.command, Error{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy),
        L"m3_lifecycle_runner", L"invalid lifecycle command"});
}

} // namespace myrd::m3
