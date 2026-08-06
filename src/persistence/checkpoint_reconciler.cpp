#include "src/persistence/checkpoint_reconciler.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> replay_failure(const PersistenceErrorCode code,
                         const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"replay_checkpoint_ack_stream",
        message,
        0});
}

std::optional<DirtyRecordKind> dirty_kind(
    const JournalRecordType type) noexcept {
    switch (type) {
    case JournalRecordType::Data: return DirtyRecordKind::Data;
    case JournalRecordType::Zero: return DirtyRecordKind::Zero;
    case JournalRecordType::Discard: return DirtyRecordKind::Discard;
    case JournalRecordType::IndexNode:
    case JournalRecordType::CheckpointCommit:
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

Result<CheckpointAckReplayMetrics> replay_checkpoint_ack_stream(
    const OperationContext& context,
    const CheckpointAckReplayRequest& request) {
    const auto& stream = request.stream;
    if (!request.journal_file || request.store == nullptr ||
        stream.journal_identity != request.journal_header.journal_identity ||
        stream.journal_epoch != request.journal_header.epoch ||
        stream.generation == 0 || stream.generation == kInvalidGeneration ||
        stream.operation_id == 0 ||
        stream.begin_offset < kJournalAlignment ||
        stream.begin_offset % kJournalAlignment != 0 ||
        stream.end_offset < stream.begin_offset ||
        stream.end_offset % kJournalAlignment != 0 ||
        request.snapshot_token.source_backing_generation >=
            stream.generation ||
        request.store->backing_generation() != stream.generation ||
        request.journal_header.geometry.virtual_bytes !=
            request.store->size_bytes() ||
        request.journal_header.geometry.logical_sector_size !=
            request.store->sector_size()) {
        return replay_failure<CheckpointAckReplayMetrics>(
            PersistenceErrorCode::InvalidFormat,
            L"ack stream identity does not match the live transaction");
    }
    if ((stream.record_count == 0) !=
        (stream.begin_offset == stream.end_offset)) {
        return replay_failure<CheckpointAckReplayMetrics>(
            PersistenceErrorCode::InvalidFormat,
            L"empty ack stream shape is inconsistent");
    }

    CheckpointAckReplayMetrics metrics{};
    std::uint64_t offset = stream.begin_offset;
    std::optional<std::uint64_t> previous_block;
    for (std::uint64_t ordinal = 0; ordinal < stream.record_count; ++ordinal) {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(live.error());
        }
        if (offset > stream.end_offset ||
            stream.end_offset - offset < kFixedRecordBytes) {
            return replay_failure<CheckpointAckReplayMetrics>(
                PersistenceErrorCode::JournalCorrupt,
                L"ack stream ends inside a record header");
        }
        std::array<std::byte, kFixedRecordBytes> prefix{};
        auto read = request.journal_file->read_exact(
            context, offset, prefix);
        if (!read.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(read.error());
        }
        auto type = decode_journal_record_type_prefix(prefix);
        if (!type.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(type.error());
        }
        const auto kind = dirty_kind(type.value());
        if (!kind.has_value()) {
            return replay_failure<CheckpointAckReplayMetrics>(
                PersistenceErrorCode::JournalCorrupt,
                L"ack stream contains a non-block record");
        }
        auto physical = journal_record_physical_length(type.value());
        if (!physical.ok() || physical.value() > kDataRecordBytes ||
            physical.value() > stream.end_offset - offset ||
            physical.value() >
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return replay_failure<CheckpointAckReplayMetrics>(
                PersistenceErrorCode::JournalCorrupt,
                L"ack record length exceeds the frozen stream");
        }
        std::vector<std::byte> bytes;
        try {
            bytes.resize(static_cast<std::size_t>(physical.value()));
        } catch (const std::bad_alloc&) {
            return replay_failure<CheckpointAckReplayMetrics>(
                PersistenceErrorCode::SnapshotResourceExhausted,
                L"allocating the bounded ack record buffer failed");
        }
        read = request.journal_file->read_exact(context, offset, bytes);
        if (!read.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(read.error());
        }
        auto decoded = decode_block_record(
            bytes, offset, stream.end_offset,
            request.journal_header.geometry);
        if (!decoded.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(
                decoded.error());
        }
        const auto& record = decoded.value().metadata;
        if (record.common.type != type.value() ||
            record.common.checkpoint_generation != stream.generation ||
            record.common.operation_id != stream.operation_id ||
            record.common.sequence != ordinal ||
            record.common.flags != 0 ||
            (previous_block.has_value() &&
             *previous_block >= record.block_index)) {
            return replay_failure<CheckpointAckReplayMetrics>(
                PersistenceErrorCode::JournalCorrupt,
                L"ack record order or transaction identity is invalid");
        }
        previous_block = record.block_index;

        auto acknowledged = request.store->acknowledge_snapshot_member(
            request.snapshot_token,
            record.block_index,
            record.snapshot_version,
            *kind,
            record.source_generation,
            record.dirty_epoch,
            stream.generation);
        if (!acknowledged.ok()) {
            return Result<CheckpointAckReplayMetrics>::failure(
                acknowledged.error());
        }
        ++metrics.records_verified;
        switch (acknowledged.value()) {
        case SnapshotMemberAck::Acknowledged: ++metrics.acknowledged; break;
        case SnapshotMemberAck::AlreadyPersisted:
            ++metrics.already_persisted;
            break;
        case SnapshotMemberAck::NewerDirtyPreserved:
            ++metrics.newer_dirty_preserved;
            break;
        }
        metrics.peak_record_bytes = (std::max)(
            metrics.peak_record_bytes, physical.value());
        offset += physical.value();
    }
    if (offset != stream.end_offset) {
        return replay_failure<CheckpointAckReplayMetrics>(
            PersistenceErrorCode::JournalCorrupt,
            L"ack stream has unconsumed bytes after its record count");
    }
    return Result<CheckpointAckReplayMetrics>::success(std::move(metrics));
}

}  // namespace myrd::persistence
