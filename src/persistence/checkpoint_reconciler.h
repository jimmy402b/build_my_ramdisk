#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/storage/dynamic_block_store.h"

#include <cstdint>
#include <memory>

namespace myrd::persistence {

struct CheckpointAckReplayMetrics final {
    std::uint64_t records_verified{};
    std::uint64_t acknowledged{};
    std::uint64_t already_persisted{};
    std::uint64_t newer_dirty_preserved{};
    std::uint64_t peak_record_bytes{};
};

struct CheckpointAckReplayRequest final {
    std::shared_ptr<IPersistenceFile> journal_file{};
    JournalHeader journal_header{};
    JournalAckRecordStreamDescriptor stream{};
    DynamicBlockStore* store{};
    ConsistentSnapshotToken snapshot_token{};
};

// Replays only the block-record prefix frozen by JournalWriter in ack_stream.
// Every record is fully decoded and CRC-checked before its exact snapshot
// identity is presented to DynamicBlockStore. Memory is bounded by one fixed
// format record (kDataRecordBytes), independent of image size or dirty count.
[[nodiscard]] Result<CheckpointAckReplayMetrics>
replay_checkpoint_ack_stream(
    const OperationContext& context,
    const CheckpointAckReplayRequest& request);

}  // namespace myrd::persistence
