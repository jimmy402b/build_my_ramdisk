#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_index.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace myrd::persistence {

struct JournalCheckpointBlockInput final {
    JournalRecordType type{JournalRecordType::Data};
    std::uint64_t block_index{0};
    std::uint64_t snapshot_version{0};
    BaseGeneration source_generation{0};
    std::uint64_t dirty_epoch{0};
    std::span<const std::byte> payload{};
};

struct JournalCheckpointRequest final {
    std::shared_ptr<IPersistenceFile> file{};
    WindowsFileIdentity file_identity{};
    JournalHeader header{};
    CheckpointGeneration previous_generation{0};
    Sha256Digest previous_digest{};
    std::uint64_t committed_length{0};
    PersistentIndexRootDescriptor root{};
    std::uint64_t operation_id{0};
    // Borrowed for the synchronous prepare call. Entries must be strictly
    // increasing by block_index; DATA payloads remain live for the call.
    std::span<const JournalCheckpointBlockInput> blocks{};
};

struct JournalAckRecordStreamDescriptor final {
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{0};
    std::uint64_t begin_offset{0};
    std::uint64_t end_offset{0};
    std::uint64_t record_count{0};
    CheckpointGeneration generation{0};
    std::uint64_t operation_id{0};

    bool operator==(const JournalAckRecordStreamDescriptor&) const noexcept =
        default;
};

struct PreparedCheckpoint final {
    CheckpointGeneration generation{0};
    PersistentIndexRootDescriptor root{};
    JournalCommitDescriptor commit{};
    std::uint64_t committed_length{0};
    Sha256Digest chain_digest{};
    std::uint64_t data_count{0};
    std::uint64_t zero_count{0};
    std::uint64_t discard_count{0};
    std::uint64_t index_node_count{0};
    JournalAckRecordStreamDescriptor ack_stream{};

    bool operator==(const PreparedCheckpoint&) const noexcept = default;
};

enum class JournalWriterPhase : std::uint8_t {
    BeforeBlockWrite,
    AfterBlockWrite,
    BeforeIndexWrite,
    AfterIndexWrite,
    BeforeCommitWrite,
    AfterCommitWrite,
    BeforeFlush,
    AfterFlush,
    BeforeReadback,
    AfterReadback,
    BeforeIndexCrosscheck,
    AfterIndexCrosscheck,
    Prepared,
};

struct JournalWriterPhaseEvent final {
    JournalWriterPhase phase{};
    std::uint64_t ordinal{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

struct JournalWriterOptions final {
    JournalIndexBuilderOptions index_builder{};
    // Required, finite per-operation ceilings derived by the caller from its
    // already validated PersistenceLimits. There is deliberately no hidden
    // "unlimited" product default.
    std::uint64_t max_checkpoint_records{0};
    std::uint64_t max_checkpoint_bytes{0};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::function<Result<void>(const JournalWriterPhaseEvent&)> phase_hook{};
#endif
};

class JournalWriter final {
public:
    // The caller must hold the bundle's exclusive WriterLock for the entire
    // prepare call. The instance mutex only rejects accidental re-entry on the
    // same object; it is not a substitute for cross-process writer ownership.
    static Result<std::unique_ptr<JournalWriter>> create(
        JournalWriterOptions options);

    ~JournalWriter();
    JournalWriter(JournalWriter&&) noexcept;
    JournalWriter& operator=(JournalWriter&&) noexcept;
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    Result<PreparedCheckpoint> prepare(
        const OperationContext& context,
        const JournalCheckpointRequest& request);

private:
    struct Impl;
    explicit JournalWriter(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace myrd::persistence
