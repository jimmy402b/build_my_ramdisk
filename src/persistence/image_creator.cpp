#include "image_creator.h"

#include "src/persistence/byte_codec.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/writer_lock.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> create_failure(
    const PersistenceErrorCode code,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"create_generation_zero_image",
        message,
        native_code});
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
thread_local const GenerationZeroCreateHook* g_create_hook{};

Result<void> create_phase(const GenerationZeroCreatePhase phase) {
    if (g_create_hook == nullptr) return Result<void>::success();
    try {
        return (*g_create_hook)(phase);
    } catch (...) {
        return create_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"generation-zero creation test hook threw");
    }
}
#endif

class EmptyPayloadSource final : public IVhdxPayloadSource {
public:
    Result<void> read_payload(
        const OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        return create_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"an empty VHDX plan requested payload data");
    }
};

template <std::size_t N>
bool all_zero(const std::array<std::byte, N>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{};
    });
}

Result<void> validate_request(const GenerationZeroCreateRequest& request) {
    if (request.bundle_root.empty() ||
        validate_geometry(request.geometry) != GeometryStatus::Ok ||
        request.image_uuid.is_nil() ||
        all_zero(request.base_identity.file_identity.file_write_guid) ||
        all_zero(request.base_identity.file_identity.data_write_guid) ||
        all_zero(request.base_identity.virtual_disk_id) ||
        all_zero(request.journal_identity.journal_uuid) ||
        request.journal_epoch == 0 ||
        request.journal_epoch == kInvalidGeneration ||
        request.operation_id == 0) {
        return create_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"generation-zero request fields are invalid");
    }
    for (const auto name : {
             std::string_view{request.manifest_relative_name},
             std::string_view{request.base_relative_name},
             std::string_view{request.journal_relative_name}}) {
        auto component = validate_image_component(name);
        if (!component.ok()) return Result<void>::failure(component.error());
    }
    if (!request.base_temporary_name.empty()) {
        auto component = validate_image_component(
            request.base_temporary_name);
        if (!component.ok()) return Result<void>::failure(component.error());
    }
    return Result<void>::success();
}

std::string planned_base_temporary_name(
    const GenerationZeroCreateRequest& request) {
    if (!request.base_temporary_name.empty()) {
        return request.base_temporary_name;
    }
    return request.base_relative_name + ".create-" +
        std::to_string(request.operation_id) + ".tmp";
}

Result<void> write_and_verify_genesis_journal(
    const OperationContext& context,
    BundleWriterLock& writer,
    const GenerationZeroCreateRequest& request,
    const JournalHeader& header,
    JournalCommitDescriptor& descriptor) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto phase = create_phase(GenerationZeroCreatePhase::BeforeJournalCreate);
    if (!phase.ok()) return phase;
#endif
    auto capability = create_bundle_file_for_mutation(
        writer, request.journal_relative_name);
    if (!capability.ok()) return Result<void>::failure(capability.error());
    auto opened = open_validated_persistence_file(capability.move_value());
    if (!opened.ok()) return Result<void>::failure(opened.error());
    auto file = opened.move_value();

    auto header_bytes = encode_journal_header(header);
    if (!header_bytes.ok()) {
        return Result<void>::failure(header_bytes.error());
    }
    CheckpointCommitRecord commit{};
    commit.common.type = JournalRecordType::CheckpointCommit;
    commit.common.checkpoint_generation = 0;
    commit.common.operation_id = request.operation_id;
    commit.image_uuid = request.image_uuid;
    commit.journal_identity = request.journal_identity;
    commit.journal_epoch = request.journal_epoch;
    commit.previous_generation = 0;
    commit.new_generation = 0;
    commit.previous_digest = header.predecessor_digest;
    commit.new_chain_digest = header.predecessor_digest;
    commit.checkpoint_begin_offset = kJournalAlignment;
    commit.journal_committed_end = 2 * kJournalAlignment;
    commit.geometry_digest = header.geometry_digest;
    auto commit_bytes = encode_checkpoint_commit(
        commit, header, kJournalAlignment, 2 * kJournalAlignment);
    if (!commit_bytes.ok()) {
        return Result<void>::failure(commit_bytes.error());
    }

    auto live = check_operation_context(context);
    if (!live.ok()) return live;
    auto resized = file->truncate(context, 2 * kJournalAlignment);
    if (!resized.ok()) return resized;
    auto written = file->write_exact(context, 0, header_bytes.value());
    if (!written.ok()) return written;
    written = file->write_exact(
        context, kJournalAlignment, commit_bytes.value());
    if (!written.ok()) return written;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeJournalFlush);
    if (!phase.ok()) return phase;
#endif
    auto flushed = file->flush(context);
    if (!flushed.ok()) return flushed;

    JournalHeaderBytes header_readback{};
    std::array<std::byte, kJournalAlignment> commit_readback{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeJournalReadback);
    if (!phase.ok()) return phase;
#endif
    auto read = file->read_exact(context, 0, header_readback);
    if (!read.ok()) return read;
    read = file->read_exact(
        context, kJournalAlignment, commit_readback);
    if (!read.ok()) return read;
    if (header_readback != header_bytes.value() ||
        commit_readback != commit_bytes.value()) {
        return create_failure<void>(
            PersistenceErrorCode::VerificationFailed,
            L"journal genesis readback differs from durable bytes");
    }
    std::size_t crc_offset = 32;
    std::uint32_t commit_crc{};
    if (read_u32_le(commit_bytes.value(), crc_offset, commit_crc) !=
        ByteCodecError::none) {
        return create_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"encoded genesis commit did not contain a CRC");
    }
    descriptor.offset = kJournalAlignment;
    descriptor.length = kJournalAlignment;
    descriptor.crc32c = commit_crc;
    descriptor.chain_digest = header.predecessor_digest;
    return file->close();
}

Result<void> publish_manifest_slot(
    const OperationContext& context,
    BundleWriterLock& writer,
    const ManifestSlot& slot) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto phase = create_phase(
        GenerationZeroCreatePhase::BeforeManifestInitialization);
    if (!phase.ok()) return phase;
#endif
    auto capability = open_locked_manifest_for_mutation(writer);
    if (!capability.ok()) return Result<void>::failure(capability.error());
    auto opened = open_validated_persistence_file(capability.move_value());
    if (!opened.ok()) return Result<void>::failure(opened.error());
    auto file = opened.move_value();
    std::array<std::byte, kManifestFileBytes> zeros{};
    auto resized = file->truncate(context, kManifestFileBytes);
    if (!resized.ok()) return resized;
    auto written = file->write_exact(context, 0, zeros);
    if (!written.ok()) return written;
    auto flushed = file->flush(context);
    if (!flushed.ok()) return flushed;
    std::array<std::byte, kManifestFileBytes> initial_readback{};
    auto read = file->read_exact(context, 0, initial_readback);
    if (!read.ok()) return read;
    if (initial_readback != zeros) {
        return create_failure<void>(
            PersistenceErrorCode::VerificationFailed,
            L"empty manifest readback differs before publication");
    }

    auto encoded = encode_manifest_slot(slot);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeManifestSlotWrite);
    if (!phase.ok()) return phase;
#endif
    written = file->write_exact(context, 0, encoded.value());
    if (!written.ok()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot write failed after publication may have begun",
            written.error().native_code);
    }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeManifestSlotFlush);
    if (!phase.ok()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot publication stopped before durability was proven",
            phase.error().native_code);
    }
#endif
    flushed = file->flush(context);
    if (!flushed.ok()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot flush failed after publication bytes were written",
            flushed.error().native_code);
    }
    ManifestSlotBytes readback{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeManifestSlotReadback);
    if (!phase.ok()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot durability was not read back",
            phase.error().native_code);
    }
#endif
    read = file->read_exact(context, 0, readback);
    if (!read.ok()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot could not be read back after flush",
            read.error().native_code);
    }
    if (readback != encoded.value()) {
        return create_failure<void>(
            PersistenceErrorCode::CommitIndeterminate,
            L"manifest slot readback differs after flush");
    }
    return file->close();
}

}  // namespace

Result<RecoveredImage> create_generation_zero_image(
    const OperationContext& context,
    const GenerationZeroCreateRequest& request) {
    auto valid = validate_request(request);
    if (!valid.ok()) {
        return Result<RecoveredImage>::failure(valid.error());
    }
    auto live = check_operation_context(context);
    if (!live.ok()) return Result<RecoveredImage>::failure(live.error());

#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto phase = create_phase(GenerationZeroCreatePhase::BeforeManifestClaim);
    if (!phase.ok()) return Result<RecoveredImage>::failure(phase.error());
#endif

    EmptyPayloadSource source;
    const std::vector<std::uint64_t> no_payloads;
    auto plan = plan_sparse_vhdx(
        context,
        {request.geometry.virtual_bytes,
         request.geometry.logical_sector_size},
        request.base_identity,
        no_payloads,
        source,
        request.vhdx_limits);
    if (!plan.ok()) return Result<RecoveredImage>::failure(plan.error());
    constexpr auto fixed_bytes = static_cast<std::uint64_t>(
        kManifestFileBytes + 2 * kJournalAlignment);
    if (plan.value().file_length() >
            (std::numeric_limits<std::uint64_t>::max)() - fixed_bytes ||
        plan.value().file_length() + fixed_bytes >
            (std::numeric_limits<std::uint64_t>::max)() -
                request.free_space_reserve_bytes) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::DiskFull,
            L"generation-zero free-space requirement overflowed");
    }
    const auto required_free = plan.value().file_length() + fixed_bytes +
        request.free_space_reserve_bytes;
    std::error_code space_error;
    const auto space = std::filesystem::space(
        request.bundle_root, space_error);
    if (space_error) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::IoError,
            L"bundle free space could not be queried",
            static_cast<std::uint32_t>(space_error.value()));
    }
    if (space.available < required_free) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::DiskFull,
            L"bundle volume has insufficient free space");
    }

    auto writer_result = create_bundle_manifest_and_acquire_writer(
        request.bundle_root, request.manifest_relative_name);
    if (!writer_result.ok()) {
        return Result<RecoveredImage>::failure(writer_result.error());
    }
    auto writer = writer_result.move_value();
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::AfterManifestClaim);
    if (!phase.ok()) return Result<RecoveredImage>::failure(phase.error());
    phase = create_phase(GenerationZeroCreatePhase::BeforeBasePublish);
    if (!phase.ok()) return Result<RecoveredImage>::failure(phase.error());
#endif
    auto base = publish_sparse_vhdx_base(
        context, writer, planned_base_temporary_name(request),
        request.base_relative_name, plan.value(), source);
    if (!base.ok()) return Result<RecoveredImage>::failure(base.error());
#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::AfterBasePublish);
    if (!phase.ok()) return Result<RecoveredImage>::failure(phase.error());
#endif

    JournalHeader header{};
    header.image_uuid = request.image_uuid;
    header.journal_identity = request.journal_identity;
    header.epoch = request.journal_epoch;
    header.geometry = request.geometry;
    header.base_checkpoint_generation = 0;
    header.predecessor_kind = JournalPredecessorKind::Genesis;
    auto geometry_digest = compute_geometry_digest(request.geometry);
    auto base_digest = compute_base_identity_digest(
        request.base_identity.file_identity);
    if (!geometry_digest.ok()) {
        return Result<RecoveredImage>::failure(geometry_digest.error());
    }
    if (!base_digest.ok()) {
        return Result<RecoveredImage>::failure(base_digest.error());
    }
    header.geometry_digest = geometry_digest.value();
    header.base_identity_digest = base_digest.value();
    auto genesis_digest = compute_genesis_digest(
        request.image_uuid, header.geometry_digest,
        header.base_identity_digest);
    if (!genesis_digest.ok()) {
        return Result<RecoveredImage>::failure(genesis_digest.error());
    }
    header.predecessor_digest = genesis_digest.value();

    JournalCommitDescriptor commit{};
    auto journal = write_and_verify_genesis_journal(
        context, writer, request, header, commit);
    if (!journal.ok()) {
        return Result<RecoveredImage>::failure(journal.error());
    }

    ManifestSlot slot{};
    slot.slot_index = 0;
    slot.image_uuid = request.image_uuid;
    slot.generation.checkpoint_generation = 0;
    slot.generation.base_generation = 0;
    slot.generation.journal.epoch = request.journal_epoch;
    slot.generation.journal.committed_length = 2 * kJournalAlignment;
    slot.generation.journal.commit = commit;
    slot.geometry = request.geometry;
    slot.base_relative_name = request.base_relative_name;
    slot.base_identity = request.base_identity.file_identity;
    slot.base_expected_length = base.value().layout().file_length();
    slot.journal_relative_name = request.journal_relative_name;
    slot.journal_identity = request.journal_identity;
    slot.created_time_100ns = request.created_time_100ns;
    slot.committed_time_100ns = request.created_time_100ns;
    auto published = publish_manifest_slot(context, writer, slot);
    if (!published.ok()) {
        return Result<RecoveredImage>::failure(published.error());
    }
    auto closed = writer.close();
    if (!closed.ok()) {
        return Result<RecoveredImage>::failure(closed.error());
    }

#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = create_phase(GenerationZeroCreatePhase::BeforeRecovery);
    if (!phase.ok()) return Result<RecoveredImage>::failure(phase.error());
#endif
    auto recovered = recover_complete_image(
        context, request.bundle_root, request.manifest_relative_name,
        request.recovery);
    if (!recovered.ok() || !recovered.image.has_value()) {
        if (recovered.error.has_value()) {
            return Result<RecoveredImage>::failure(*recovered.error);
        }
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::VerificationFailed,
            L"generation zero could not be reopened through recovery");
    }
    if (recovered.image->checkpoint_generation != 0 ||
        recovered.image->selected_slot != 0) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::VerificationFailed,
            L"recovery did not select generation-zero slot A");
    }
    std::array<std::byte, kM2BlockSize> boundary{};
    auto first = recovered.image->backing_view.store->load_block(
        context, 0, boundary);
    if (!first.ok() || first.value().kind != BlockLoadKind::AllZero) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::VerificationFailed,
            L"generation-zero first block did not reopen as zero");
    }
    const auto last_block =
        (request.geometry.virtual_bytes - 1) / kM2BlockSize;
    auto last = recovered.image->backing_view.store->load_block(
        context, last_block, boundary);
    if (!last.ok() || last.value().kind != BlockLoadKind::AllZero) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::VerificationFailed,
            L"generation-zero last block did not reopen as zero");
    }
    return Result<RecoveredImage>::success(
        std::move(*recovered.image));
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<RecoveredImage> create_generation_zero_image_for_testing(
    const OperationContext& context,
    const GenerationZeroCreateRequest& request,
    GenerationZeroCreateHook hook) {
    if (!hook || g_create_hook != nullptr) {
        return create_failure<RecoveredImage>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"generation-zero test hook must be nonempty and nonnested");
    }
    struct ScopedHook final {
        explicit ScopedHook(const GenerationZeroCreateHook& value) noexcept {
            g_create_hook = &value;
        }
        ~ScopedHook() noexcept { g_create_hook = nullptr; }
    } scoped(hook);
    return create_generation_zero_image(context, request);
}
#endif

}  // namespace myrd::persistence
