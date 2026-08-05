#include "src/persistence/journal_writer.h"

#include "src/common/crc32c.h"
#include "src/persistence/image_paths.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <typename T>
Result<T> writer_failure(std::wstring_view message,
                         const PersistenceErrorCode code =
                             PersistenceErrorCode::InvalidFormat) {
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        L"JournalWriter", std::wstring{message}, 0});
}

bool checked_add(const std::uint64_t left,
                 const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    output = left + right;
    return true;
}

template <typename Buffer>
bool all_zero(const Buffer& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0};
    });
}

std::uint32_t record_crc(
    const std::span<const std::byte> bytes) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[32 + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t block_record_length(const JournalRecordType type) noexcept {
    return type == JournalRecordType::Data
        ? kDataRecordBytes : kFixedRecordBytes;
}

Result<PreparedCheckpoint> verification_failure(
    std::wstring_view message) {
    return writer_failure<PreparedCheckpoint>(
        message, PersistenceErrorCode::VerificationFailed);
}

}  // namespace

struct JournalWriter::Impl final {
    Impl(JournalWriterOptions source_options,
         std::unique_ptr<JournalIndexBuilder> source_builder,
         std::unique_ptr<JournalIndexReader> source_reader)
        : options(std::move(source_options)),
          index_builder(std::move(source_builder)),
          index_reader(std::move(source_reader)) {}

    Result<void> phase(const OperationContext& context,
                       const JournalWriterPhase phase_value,
                       const std::uint64_t ordinal,
                       const std::uint64_t offset,
                       const std::uint64_t length) {
        auto checked = check_operation_context(context);
        if (!checked.ok()) return checked;
#if defined(MYRD_ENABLE_TEST_SEAMS)
        if (options.phase_hook) {
            auto hook = options.phase_hook(
                {phase_value, ordinal, offset, length});
            if (!hook.ok()) return hook;
        }
#else
        (void)phase_value;
        (void)ordinal;
        (void)offset;
        (void)length;
#endif
        return check_operation_context(context);
    }

    JournalWriterOptions options{};
    std::unique_ptr<JournalIndexBuilder> index_builder{};
    std::unique_ptr<JournalIndexReader> index_reader{};
    std::mutex mutex{};
};

JournalWriter::JournalWriter(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
JournalWriter::~JournalWriter() = default;
JournalWriter::JournalWriter(JournalWriter&&) noexcept = default;
JournalWriter& JournalWriter::operator=(JournalWriter&&) noexcept = default;

Result<std::unique_ptr<JournalWriter>> JournalWriter::create(
    JournalWriterOptions options) {
    if (options.max_checkpoint_records < 2 ||
        options.max_checkpoint_bytes < 3 * kJournalAlignment ||
        options.max_checkpoint_bytes % kJournalAlignment != 0 ||
        options.max_checkpoint_records >
            options.max_checkpoint_bytes / kJournalAlignment - 1 ||
        options.max_checkpoint_records ==
            (std::numeric_limits<std::uint64_t>::max)() ||
        options.max_checkpoint_bytes ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return writer_failure<std::unique_ptr<JournalWriter>>(
            L"invalid writer limits");
    }
    try {
        auto builder = JournalIndexBuilder::create(options.index_builder);
        if (!builder.ok()) {
            return Result<std::unique_ptr<JournalWriter>>::failure(
                builder.error());
        }
        auto reader = JournalIndexReader::create(
            options.index_builder.reader_cache);
        if (!reader.ok()) {
            return Result<std::unique_ptr<JournalWriter>>::failure(
                reader.error());
        }
        auto impl = std::make_unique<Impl>(
            std::move(options), builder.move_value(), reader.move_value());
        return Result<std::unique_ptr<JournalWriter>>::success(
            std::unique_ptr<JournalWriter>(
                new JournalWriter(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<JournalWriter>>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalWriter::create", L"allocating writer failed", 0});
    }
}

Result<PreparedCheckpoint> JournalWriter::prepare(
    const OperationContext& context,
    const JournalCheckpointRequest& request) {
    if (!impl_) {
        return writer_failure<PreparedCheckpoint>(
            L"writer is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    std::unique_lock writer_lock(impl_->mutex, std::try_to_lock);
    if (!writer_lock.owns_lock()) {
        return writer_failure<PreparedCheckpoint>(
            L"another checkpoint is already being prepared",
            PersistenceErrorCode::OperationBusy);
    }
    auto context_result = check_operation_context(context);
    if (!context_result.ok()) {
        return Result<PreparedCheckpoint>::failure(context_result.error());
    }
    if (!request.file || request.operation_id == 0 || request.blocks.empty() ||
        request.blocks.size() > impl_->options.max_checkpoint_records ||
        request.committed_length < kJournalAlignment ||
        request.committed_length % kJournalAlignment != 0 ||
        all_zero(request.file_identity.file_id) ||
        all_zero(request.previous_digest)) {
        return writer_failure<PreparedCheckpoint>(L"request is invalid");
    }
    auto* validated_file =
        dynamic_cast<ValidatedPersistenceFile*>(request.file.get());
#if !defined(MYRD_ENABLE_TEST_SEAMS)
    if (validated_file == nullptr) {
        return writer_failure<PreparedCheckpoint>(
            L"journal mutation lacks a validated writer capability",
            PersistenceErrorCode::AccessDenied);
    }
#endif
    if (validated_file != nullptr) {
        auto authority = validated_file->verify_mutation_capability();
        if (!authority.ok()) {
            return Result<PreparedCheckpoint>::failure(authority.error());
        }
        if (!same_file_object(authority.value(), request.file_identity)) {
            return writer_failure<PreparedCheckpoint>(
                L"journal file identity does not match mutation capability",
                PersistenceErrorCode::AccessDenied);
        }
    }
    for (std::size_t index = 1; index < request.blocks.size(); ++index) {
        if (request.blocks[index - 1].block_index >=
            request.blocks[index].block_index) {
            return writer_failure<PreparedCheckpoint>(
                L"checkpoint block inputs are not strictly ordered");
        }
    }
    auto next_generation = request.previous_generation;
    if (!checked_increment(next_generation)) {
        return writer_failure<PreparedCheckpoint>(
            L"checkpoint generation is exhausted");
    }

    std::array<std::byte, kJournalAlignment> actual_header_bytes{};
    auto read_header = request.file->read_exact(
        context, 0, actual_header_bytes);
    if (!read_header.ok()) {
        return Result<PreparedCheckpoint>::failure(read_header.error());
    }
    auto actual_header = decode_journal_header(actual_header_bytes);
    if (!actual_header.ok() || actual_header.value() != request.header) {
        return writer_failure<PreparedCheckpoint>(
            L"journal header does not match request");
    }
    auto file_length = request.file->length(context);
    if (!file_length.ok()) {
        return Result<PreparedCheckpoint>::failure(file_length.error());
    }
    if (file_length.value() < request.committed_length) {
        return writer_failure<PreparedCheckpoint>(
            L"journal is shorter than the committed prefix");
    }
    if (request.previous_generation ==
            request.header.base_checkpoint_generation &&
        request.previous_digest != request.header.predecessor_digest) {
        return writer_failure<PreparedCheckpoint>(
            L"base checkpoint digest does not match journal predecessor");
    }
    if (request.previous_generation <
        request.header.base_checkpoint_generation) {
        return writer_failure<PreparedCheckpoint>(
            L"checkpoint precedes the journal base generation");
    }

    JournalIndexView base_view{};
    base_view.file = request.file;
    base_view.file_identity = request.file_identity;
    base_view.journal_identity = request.header.journal_identity;
    base_view.epoch = request.header.epoch;
    base_view.root_generation = request.previous_generation;
    base_view.committed_length = request.committed_length;
    base_view.root = request.root;

    auto digest = CheckpointChainDigest::begin({
        request.previous_digest, request.header.image_uuid,
        request.header.epoch, request.previous_generation,
        next_generation, request.header.geometry_digest});
    if (!digest.ok()) {
        return Result<PreparedCheckpoint>::failure(digest.error());
    }

    std::vector<JournalIndexLeafEntry> updates;
    try {
        updates.reserve(request.blocks.size());
    } catch (const std::bad_alloc&) {
        return Result<PreparedCheckpoint>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalWriter::prepare", L"allocating checkpoint state failed",
            0});
    }

    std::uint64_t cursor = request.committed_length;
    std::uint64_t data_count = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t discard_count = 0;
    for (std::size_t index = 0; index < request.blocks.size(); ++index) {
        const auto& input = request.blocks[index];
        BlockJournalRecord record{};
        record.common = {input.type, next_generation, request.operation_id,
                         static_cast<std::uint64_t>(index), 0};
        record.block_index = input.block_index;
        record.snapshot_version = input.snapshot_version;
        record.source_generation = input.source_generation;
        record.dirty_epoch = input.dirty_epoch;
        auto encoded = encode_block_record(
            record, input.payload, request.header.geometry);
        if (!encoded.ok()) {
            return Result<PreparedCheckpoint>::failure(encoded.error());
        }
        std::uint64_t end = 0;
        if (!checked_add(cursor, encoded.value().size(), end) ||
            end - request.committed_length >
                impl_->options.max_checkpoint_bytes) {
            return writer_failure<PreparedCheckpoint>(
                L"checkpoint byte limit exceeded",
                PersistenceErrorCode::SnapshotResourceExhausted);
        }
        auto frozen = decode_block_record(
            encoded.value(), cursor, end, request.header.geometry);
        if (!frozen.ok()) {
            return Result<PreparedCheckpoint>::failure(frozen.error());
        }
        auto phase = impl_->phase(
            context, JournalWriterPhase::BeforeBlockWrite, index, cursor,
            encoded.value().size());
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
        auto write = request.file->write_exact(context, cursor,
                                               encoded.value());
        if (!write.ok()) {
            return Result<PreparedCheckpoint>::failure(write.error());
        }
        phase = impl_->phase(
            context, JournalWriterPhase::AfterBlockWrite, index, cursor,
            encoded.value().size());
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
        auto append = digest.value().append_non_commit_record(encoded.value());
        if (!append.ok()) {
            return Result<PreparedCheckpoint>::failure(append.error());
        }
        try {
            updates.push_back({
                frozen.value().metadata.block_index,
                frozen.value().metadata.common.type,
                frozen.value().payload_crc32c, cursor,
                static_cast<std::uint64_t>(encoded.value().size()),
                next_generation});
        } catch (const std::bad_alloc&) {
            return Result<PreparedCheckpoint>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"JournalWriter::prepare",
                L"allocating index updates failed", 0});
        }
        if (input.type == JournalRecordType::Data) ++data_count;
        else if (input.type == JournalRecordType::Zero) ++zero_count;
        else ++discard_count;
        cursor = end;
    }
    const auto ack_end = cursor;

    auto plan = impl_->index_builder->plan(
        context, base_view, updates, next_generation, request.operation_id);
    if (!plan.ok()) {
        return Result<PreparedCheckpoint>::failure(plan.error());
    }
    const auto node_count = plan.value().planned_nodes().size();
    if (node_count > impl_->options.max_checkpoint_records -
                         request.blocks.size()) {
        return writer_failure<PreparedCheckpoint>(
            L"checkpoint record limit exceeded",
            PersistenceErrorCode::SnapshotResourceExhausted);
    }
    std::uint64_t node_bytes = 0;
    std::uint64_t commit_offset = 0;
    std::uint64_t committed_end = 0;
    if (node_count >
            (std::numeric_limits<std::uint64_t>::max)() / kIndexNodeBytes ||
        !checked_add(0, node_count * kIndexNodeBytes, node_bytes) ||
        !checked_add(cursor, node_bytes, commit_offset) ||
        !checked_add(commit_offset, kFixedRecordBytes, committed_end) ||
        committed_end - request.committed_length >
            impl_->options.max_checkpoint_bytes) {
        return writer_failure<PreparedCheckpoint>(
            L"checkpoint layout overflows configured limits",
            PersistenceErrorCode::SnapshotResourceExhausted);
    }
    auto assigned = impl_->index_builder->assign_offsets(
        plan.value(), base_view, cursor, request.blocks.size(),
        committed_end);
    if (!assigned.ok()) {
        return Result<PreparedCheckpoint>::failure(assigned.error());
    }
    if (assigned.value().nodes.size() != node_count ||
        assigned.value().next_offset != commit_offset) {
        return writer_failure<PreparedCheckpoint>(
            L"index assignment does not match planned checkpoint",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    for (std::size_t index = 0; index < assigned.value().nodes.size(); ++index) {
        const auto& node = assigned.value().nodes[index];
        auto phase = impl_->phase(
            context, JournalWriterPhase::BeforeIndexWrite, index,
            node.offset, node.bytes.size());
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
        auto write = request.file->write_exact(context, node.offset,
                                               node.bytes);
        if (!write.ok()) {
            return Result<PreparedCheckpoint>::failure(write.error());
        }
        phase = impl_->phase(
            context, JournalWriterPhase::AfterIndexWrite, index,
            node.offset, node.bytes.size());
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
        auto append = digest.value().append_non_commit_record(node.bytes);
        if (!append.ok()) {
            return Result<PreparedCheckpoint>::failure(append.error());
        }
    }

    CheckpointCommitRecord commit{};
    commit.common = {
        JournalRecordType::CheckpointCommit, next_generation,
        request.operation_id,
        static_cast<std::uint64_t>(request.blocks.size() + node_count), 0};
    commit.image_uuid = request.header.image_uuid;
    commit.journal_identity = request.header.journal_identity;
    commit.journal_epoch = request.header.epoch;
    commit.previous_generation = request.previous_generation;
    commit.new_generation = next_generation;
    commit.previous_digest = request.previous_digest;
    commit.new_chain_digest = request.previous_digest;
    commit.data_count = data_count;
    commit.zero_count = zero_count;
    commit.discard_count = discard_count;
    commit.index_node_count = node_count;
    commit.root = assigned.value().root;
    commit.checkpoint_begin_offset = request.committed_length;
    commit.journal_committed_end = committed_end;
    commit.geometry_digest = request.header.geometry_digest;
    commit.input_record_count = request.blocks.size() + node_count;
    auto provisional = encode_checkpoint_commit(
        commit, request.header, commit_offset, committed_end);
    if (!provisional.ok()) {
        return Result<PreparedCheckpoint>::failure(provisional.error());
    }
    auto chain_digest = digest.value().finalize(provisional.value());
    if (!chain_digest.ok()) {
        return Result<PreparedCheckpoint>::failure(chain_digest.error());
    }
    commit.new_chain_digest = chain_digest.value();
    auto commit_bytes = encode_checkpoint_commit(
        commit, request.header, commit_offset, committed_end);
    if (!commit_bytes.ok()) {
        return Result<PreparedCheckpoint>::failure(commit_bytes.error());
    }
    auto phase = impl_->phase(
        context, JournalWriterPhase::BeforeCommitWrite, 0, commit_offset,
        commit_bytes.value().size());
    if (!phase.ok()) {
        return Result<PreparedCheckpoint>::failure(phase.error());
    }
    auto write_commit = request.file->write_exact(
        context, commit_offset, commit_bytes.value());
    if (!write_commit.ok()) {
        return Result<PreparedCheckpoint>::failure(write_commit.error());
    }
    phase = impl_->phase(
        context, JournalWriterPhase::AfterCommitWrite, 0, commit_offset,
        commit_bytes.value().size());
    if (!phase.ok()) {
        return Result<PreparedCheckpoint>::failure(phase.error());
    }
    phase = impl_->phase(
        context, JournalWriterPhase::BeforeFlush, 0,
        request.committed_length,
        committed_end - request.committed_length);
    if (!phase.ok()) {
        return Result<PreparedCheckpoint>::failure(phase.error());
    }
    auto flush = request.file->flush(context);
    if (!flush.ok()) {
        return Result<PreparedCheckpoint>::failure(flush.error());
    }
    phase = impl_->phase(
        context, JournalWriterPhase::AfterFlush, 0,
        request.committed_length,
        committed_end - request.committed_length);
    if (!phase.ok()) {
        return Result<PreparedCheckpoint>::failure(phase.error());
    }

    auto validator = CheckpointSequenceValidator::begin(
        request.header, request.previous_generation, next_generation,
        request.previous_digest, request.committed_length, committed_end,
        impl_->options.max_checkpoint_records);
    if (!validator.ok()) {
        return Result<PreparedCheckpoint>::failure(validator.error());
    }
    cursor = request.committed_length;
    const auto readback_record = [&](const std::uint64_t ordinal,
                                     const std::uint64_t length)
        -> Result<void> {
        auto before = impl_->phase(
            context, JournalWriterPhase::BeforeReadback, ordinal, cursor,
            length);
        if (!before.ok()) return before;
        std::vector<std::byte> bytes;
        try {
            bytes.resize(static_cast<std::size_t>(length));
        } catch (const std::bad_alloc&) {
            return Result<void>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"JournalWriter::prepare",
                L"allocating readback record failed", 0});
        }
        auto read = request.file->read_exact(context, cursor, bytes);
        if (!read.ok()) return read;
        auto verified = validator.value().append(bytes, cursor);
        if (!verified.ok()) {
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::VerificationFailed),
                L"JournalWriter::prepare",
                L"checkpoint readback verification failed", 0});
        }
        auto after = impl_->phase(
            context, JournalWriterPhase::AfterReadback, ordinal, cursor,
            length);
        if (!after.ok()) return after;
        cursor += length;
        return Result<void>::success();
    };
    std::uint64_t ordinal = 0;
    for (const auto& input : request.blocks) {
        auto verified = readback_record(ordinal++,
                                        block_record_length(input.type));
        if (!verified.ok()) {
            return Result<PreparedCheckpoint>::failure(verified.error());
        }
    }
    for (std::size_t index = 0; index < node_count; ++index) {
        auto verified = readback_record(ordinal++, kIndexNodeBytes);
        if (!verified.ok()) {
            return Result<PreparedCheckpoint>::failure(verified.error());
        }
    }
    auto verified_commit = readback_record(ordinal, kFixedRecordBytes);
    if (!verified_commit.ok()) {
        return Result<PreparedCheckpoint>::failure(verified_commit.error());
    }
    auto verified = validator.value().finalize();
    if (!verified.ok() || verified.value() != commit ||
        cursor != committed_end) {
        return verification_failure(L"checkpoint readback is incomplete");
    }
    JournalIndexView verified_view = base_view;
    verified_view.root_generation = next_generation;
    verified_view.committed_length = committed_end;
    verified_view.root = assigned.value().root;
    for (std::size_t index = 0; index < updates.size(); ++index) {
        phase = impl_->phase(
            context, JournalWriterPhase::BeforeIndexCrosscheck, index,
            verified_view.root.offset, verified_view.root.length);
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
        auto lookup = impl_->index_reader->lookup(
            context, verified_view, updates[index].block_index);
        if (!lookup.ok() ||
            lookup.value().kind == JournalIndexLookupKind::Absent ||
            lookup.value().descriptor != updates[index]) {
            return verification_failure(
                L"final index does not match a readback block record");
        }
        phase = impl_->phase(
            context, JournalWriterPhase::AfterIndexCrosscheck, index,
            verified_view.root.offset, verified_view.root.length);
        if (!phase.ok()) {
            return Result<PreparedCheckpoint>::failure(phase.error());
        }
    }
    if (validated_file != nullptr) {
        auto authority = validated_file->verify_mutation_capability();
        if (!authority.ok()) {
            return Result<PreparedCheckpoint>::failure(authority.error());
        }
        if (!same_file_object(authority.value(), request.file_identity)) {
            return writer_failure<PreparedCheckpoint>(
                L"journal identity changed during checkpoint preparation",
                PersistenceErrorCode::AccessDenied);
        }
    }
    phase = impl_->phase(
        context, JournalWriterPhase::Prepared, 0,
        request.committed_length,
        committed_end - request.committed_length);
    if (!phase.ok()) {
        return Result<PreparedCheckpoint>::failure(phase.error());
    }

    PreparedCheckpoint prepared{};
    prepared.generation = next_generation;
    prepared.root = assigned.value().root;
    prepared.commit = {commit_offset, kFixedRecordBytes,
                       record_crc(commit_bytes.value()),
                       commit.new_chain_digest};
    prepared.committed_length = committed_end;
    prepared.chain_digest = commit.new_chain_digest;
    prepared.data_count = data_count;
    prepared.zero_count = zero_count;
    prepared.discard_count = discard_count;
    prepared.index_node_count = node_count;
    prepared.ack_stream = {
        request.header.journal_identity, request.header.epoch,
        request.committed_length, ack_end, request.blocks.size(),
        next_generation, request.operation_id};
    return Result<PreparedCheckpoint>::success(std::move(prepared));
}

}  // namespace myrd::persistence
