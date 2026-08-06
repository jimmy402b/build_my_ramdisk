#include "test_framework.h"

#include "src/persistence/checkpoint_reconciler.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "tests/support/fake_backing_store.h"
#include "tests/support/fault_file.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

std::unique_ptr<DynamicBlockStore> make_store(
    DynamicBlockStoreTestDependencies dependencies = {}) {
    DynamicBlockStoreOptions options{
        2 * kBlockSizeBytes,
        3 * kBlockSizeBytes,
        kBlockSizeBytes,
        2 * kBlockSizeBytes,
        8,
        64,
        2 * kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
    auto created = DynamicBlockStore::create_for_testing({
        4 * kBlockSizeBytes,
        512,
        options,
        InitialBlockState::CleanZero,
        {},
        0,
    }, std::move(dependencies));
    REQUIRE(created.ok());
    return created.move_value();
}

OperationContext bounded_context() {
    return OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

void dirty_block(DynamicBlockStore& store, const DirtyRecordKind kind) {
    std::array<std::byte, kBlockSizeBytes> bytes{};
    bytes.fill(std::byte{0x5A});
    REQUIRE(store.write(bounded_context(), 0, bytes).ok());
    const DataRange range{0, kBlockSizeBytes};
    if (kind == DirtyRecordKind::Zero) {
        REQUIRE(store.zero(bounded_context(), {&range, 1}).ok());
    } else if (kind == DirtyRecordKind::Discard) {
        REQUIRE(store.unmap(bounded_context(), {&range, 1}).ok());
    }
}

struct SnapshotRecord final {
    ConsistentSnapshotToken token{};
    DirtyBlockSnapshot record{};
};

struct SnapshotRecords final {
    ConsistentSnapshotToken token{};
    std::vector<DirtyBlockSnapshot> records{};
};

SnapshotRecords capture_records(DynamicBlockStore& store) {
    auto token = store.begin_consistent_snapshot(bounded_context());
    REQUIRE(token.ok());
    DirtyScanCursor cursor{};
    std::vector<DirtyBlockSnapshot> records;
    while (cursor.next_block_index < store.total_blocks()) {
        auto batch = store.copy_consistent_snapshot_batch(
            bounded_context(), token.value(), cursor, kBlockSizeBytes);
        REQUIRE(batch.ok());
        for (auto& record : batch.value()) {
            records.push_back(std::move(record));
        }
    }
    return {token.value(), std::move(records)};
}

SnapshotRecord capture_record(DynamicBlockStore& store) {
    auto captured = capture_records(store);
    auto& records = captured.records;
    REQUIRE_EQ(records.size(), std::size_t{1});
    return {captured.token, std::move(records.front())};
}

void install_successor(DynamicBlockStore& store,
                       const ConsistentSnapshotToken& token) {
    auto successor = myrd::test::FakeBackingStoreBuilder(0xA5).build();
    REQUIRE(store.install_backing_view_for_snapshot(
        token, {1, std::move(successor)}).ok());
}

JournalHeader make_journal_header() {
    JournalHeader header{};
    header.epoch = 1;
    header.geometry = {4 * kBlockSizeBytes, 512, 4096};
    header.journal_identity.journal_uuid[0] = std::byte{0x42};
    return header;
}

JournalRecordType journal_type(const DirtyRecordKind kind) {
    switch (kind) {
    case DirtyRecordKind::Data: return JournalRecordType::Data;
    case DirtyRecordKind::Zero: return JournalRecordType::Zero;
    case DirtyRecordKind::Discard: return JournalRecordType::Discard;
    }
    return static_cast<JournalRecordType>(0);
}

struct AckFile final {
    std::shared_ptr<IPersistenceFile> file{};
    JournalHeader header{};
    JournalAckRecordStreamDescriptor stream{};
};

AckFile make_ack_file(
    const std::vector<DirtyBlockSnapshot>& snapshots,
    const std::vector<test::FaultFileRule>& rules = {}) {
    auto header = make_journal_header();
    std::vector<std::vector<std::byte>> encoded_records;
    std::uint64_t end_offset = kJournalAlignment;
    for (std::size_t ordinal = 0; ordinal < snapshots.size(); ++ordinal) {
        const auto& snapshot = snapshots[ordinal];
        BlockJournalRecord record{};
        record.common = {journal_type(snapshot.kind), 1, 77, ordinal, 0};
        record.block_index = snapshot.block_index;
        record.snapshot_version = snapshot.version;
        record.source_generation = snapshot.base_generation;
        record.dirty_epoch = snapshot.dirty_epoch;
        auto encoded = encode_block_record(
            record, snapshot.data, header.geometry);
        REQUIRE(encoded.ok());
        end_offset += encoded.value().size();
        encoded_records.push_back(encoded.move_value());
    }
    auto fault = test::make_fault_file(end_offset, rules);
    std::uint64_t write_offset = kJournalAlignment;
    for (const auto& encoded : encoded_records) {
        REQUIRE(fault.file->write_exact(write_offset, encoded).ok());
        write_offset += encoded.size();
    }
    JournalAckRecordStreamDescriptor stream{
        header.journal_identity,
        header.epoch,
        kJournalAlignment,
        end_offset,
        snapshots.size(),
        1,
        77,
    };
    return {
        std::shared_ptr<IPersistenceFile>(std::move(fault.file)),
        std::move(header),
        std::move(stream),
    };
}

AckFile make_ack_file(
    const DirtyBlockSnapshot& snapshot,
    const std::vector<test::FaultFileRule>& rules = {}) {
    return make_ack_file(
        std::vector<DirtyBlockSnapshot>{snapshot}, rules);
}

TEST_CASE(checkpoint_exact_ack_is_idempotent_for_all_record_kinds) {
    constexpr std::array kinds{
        DirtyRecordKind::Data,
        DirtyRecordKind::Zero,
        DirtyRecordKind::Discard,
    };
    for (const auto kind : kinds) {
        auto store = make_store();
        dirty_block(*store, kind);
        auto captured = capture_record(*store);
        REQUIRE_EQ(captured.record.kind, kind);
        install_successor(*store, captured.token);

        const auto first = store->acknowledge_snapshot_member(
            captured.token,
            captured.record.block_index,
            captured.record.version,
            captured.record.kind,
            captured.record.base_generation,
            captured.record.dirty_epoch,
            1);
        REQUIRE(first.ok());
        REQUIRE_EQ(first.value(), SnapshotMemberAck::Acknowledged);

        const auto mismatched_retry = store->acknowledge_snapshot_member(
            captured.token,
            captured.record.block_index,
            captured.record.version + 1,
            captured.record.kind,
            captured.record.base_generation,
            captured.record.dirty_epoch,
            1);
        REQUIRE(!mismatched_retry.ok());

        const auto retry = store->acknowledge_snapshot_member(
            captured.token,
            captured.record.block_index,
            captured.record.version,
            captured.record.kind,
            captured.record.base_generation,
            captured.record.dirty_epoch,
            1);
        REQUIRE(retry.ok());
        REQUIRE_EQ(retry.value(), SnapshotMemberAck::AlreadyPersisted);
        REQUIRE(store->finish_consistent_snapshot(captured.token).ok());
    }
}

TEST_CASE(checkpoint_exact_ack_preserves_newer_dirty_for_all_record_kinds) {
    constexpr std::array kinds{
        DirtyRecordKind::Data,
        DirtyRecordKind::Zero,
        DirtyRecordKind::Discard,
    };
    for (const auto kind : kinds) {
        auto store = make_store();
        dirty_block(*store, kind);
        auto captured = capture_record(*store);

        dirty_block(*store, kind);
        install_successor(*store, captured.token);

        const auto acknowledged = store->acknowledge_snapshot_member(
            captured.token,
            captured.record.block_index,
            captured.record.version,
            captured.record.kind,
            captured.record.base_generation,
            captured.record.dirty_epoch,
            1);
        REQUIRE(acknowledged.ok());
        REQUIRE_EQ(acknowledged.value(),
                   SnapshotMemberAck::NewerDirtyPreserved);

        BlockEntry entry{};
        REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, entry));
        REQUIRE_EQ(entry.state,
                   kind == DirtyRecordKind::Data
                       ? BlockState::ResidentDirty
                       : BlockState::ZeroDirty);
        REQUIRE_EQ(entry.dirty_kind, kind);
        REQUIRE(entry.dirty_epoch > captured.record.dirty_epoch);
        REQUIRE(store->finish_consistent_snapshot(captured.token).ok());
    }
}

TEST_CASE(checkpoint_exact_ack_rejects_every_identity_mismatch) {
    auto store = make_store();
    dirty_block(*store, DirtyRecordKind::Data);
    auto captured = capture_record(*store);
    install_successor(*store, captured.token);

    struct Identity final {
        std::uint64_t version;
        DirtyRecordKind kind;
        BackingGeneration base_generation;
        std::uint64_t dirty_epoch;
        BackingGeneration persisted_generation;
    };
    const auto& record = captured.record;
    const std::array mismatches{
        Identity{record.version + 1, record.kind, record.base_generation,
                 record.dirty_epoch, 1},
        Identity{record.version, DirtyRecordKind::Zero,
                 record.base_generation, record.dirty_epoch, 1},
        Identity{record.version, record.kind, record.base_generation + 1,
                 record.dirty_epoch, 1},
        Identity{record.version, record.kind, record.base_generation,
                 record.dirty_epoch + 1, 1},
        Identity{record.version, record.kind, record.base_generation,
                 record.dirty_epoch, 2},
    };
    for (const auto& mismatch : mismatches) {
        const auto rejected = store->acknowledge_snapshot_member(
            captured.token, record.block_index, mismatch.version,
            mismatch.kind, mismatch.base_generation, mismatch.dirty_epoch,
            mismatch.persisted_generation);
        REQUIRE(!rejected.ok());
        BlockEntry entry{};
        REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(
            *store, record.block_index, entry));
        REQUIRE_EQ(entry.state, BlockState::ResidentDirty);
        REQUIRE_EQ(entry.version, record.version);
        REQUIRE_EQ(entry.dirty_epoch, record.dirty_epoch);
    }

    const auto exact = store->acknowledge_snapshot_member(
        captured.token, record.block_index, record.version, record.kind,
        record.base_generation, record.dirty_epoch, 1);
    REQUIRE(exact.ok());
    REQUIRE_EQ(exact.value(), SnapshotMemberAck::Acknowledged);
    REQUIRE(store->finish_consistent_snapshot(captured.token).ok());
}

TEST_CASE(checkpoint_exact_ack_requires_a_successor_to_the_snapshot_source) {
    auto store = make_store();
    dirty_block(*store, DirtyRecordKind::Data);
    auto current = myrd::test::FakeBackingStoreBuilder(0xA4).build();
    REQUIRE(store->install_backing_view({1, current}).ok());
    auto captured = capture_record(*store);
    REQUIRE_EQ(captured.token.source_backing_generation, 1ull);
    REQUIRE_EQ(captured.record.base_generation, 0ull);

    const auto rejected = store->acknowledge_snapshot_member(
        captured.token, captured.record.block_index,
        captured.record.version, captured.record.kind,
        captured.record.base_generation, captured.record.dirty_epoch, 1);
    REQUIRE(!rejected.ok());
    BlockEntry entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 0, entry));
    REQUIRE_EQ(entry.state, BlockState::ResidentDirty);
    REQUIRE(store->abort_consistent_snapshot(captured.token).ok());
}

TEST_CASE(checkpoint_exact_ack_generation_check_is_linearized_with_cleaning) {
    DynamicBlockStoreTestDependencies dependencies{};
    dependencies.pause_after_persisted_validation = true;
    auto store = make_store(std::move(dependencies));
    dirty_block(*store, DirtyRecordKind::Data);
    auto captured = capture_record(*store);
    auto generation_one = myrd::test::FakeBackingStoreBuilder(0xD1).build();
    REQUIRE(store->install_backing_view_for_snapshot(
        captured.token, {1, generation_one}).ok());

    std::optional<Result<SnapshotMemberAck>> ack_result;
    std::thread acknowledger([&] {
        ack_result = store->acknowledge_snapshot_member(
            captured.token, captured.record.block_index,
            captured.record.version, captured.record.kind,
            captured.record.base_generation, captured.record.dirty_epoch, 1);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!DynamicBlockStoreTestAccess::persisted_ack_gap_reached(*store) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool paused =
        DynamicBlockStoreTestAccess::persisted_ack_gap_reached(*store);
    auto generation_two = myrd::test::FakeBackingStoreBuilder(0xD2).build();
    const auto advanced = store->install_backing_view_for_snapshot(
        captured.token, {2, generation_two});
    const bool released =
        DynamicBlockStoreTestAccess::release_persisted_ack_gap(*store);
    acknowledger.join();

    REQUIRE(paused);
    REQUIRE(advanced.ok());
    REQUIRE(released);
    REQUIRE(ack_result.has_value());
    REQUIRE(!ack_result->ok());
    BlockEntry entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 0, entry));
    REQUIRE_EQ(entry.state, BlockState::ResidentDirty);
    REQUIRE_EQ(store->backing_generation(), 2ull);
    REQUIRE(store->abort_consistent_snapshot(captured.token).ok());
}

TEST_CASE(checkpoint_ack_stream_replay_is_bounded_crc_checked_and_idempotent) {
    constexpr std::array kinds{
        DirtyRecordKind::Data,
        DirtyRecordKind::Zero,
        DirtyRecordKind::Discard,
    };
    for (const auto kind : kinds) {
        auto store = make_store();
        dirty_block(*store, kind);
        auto captured = capture_record(*store);
        auto ack = make_ack_file(captured.record);
        install_successor(*store, captured.token);

        const CheckpointAckReplayRequest request{
            ack.file, ack.header, ack.stream, store.get(), captured.token};
        const auto first = replay_checkpoint_ack_stream(
            bounded_context(), request);
        REQUIRE(first.ok());
        REQUIRE_EQ(first.value().records_verified, 1ull);
        REQUIRE_EQ(first.value().acknowledged, 1ull);
        REQUIRE_EQ(first.value().peak_record_bytes,
                   kind == DirtyRecordKind::Data
                       ? kDataRecordBytes : kFixedRecordBytes);

        const auto retry = replay_checkpoint_ack_stream(
            bounded_context(), request);
        REQUIRE(retry.ok());
        REQUIRE_EQ(retry.value().already_persisted, 1ull);
        REQUIRE(store->finish_consistent_snapshot(captured.token).ok());
    }
}

TEST_CASE(checkpoint_ack_stream_corruption_never_clears_dirty_state) {
    auto store = make_store();
    dirty_block(*store, DirtyRecordKind::Data);
    auto captured = capture_record(*store);
    auto ack = make_ack_file(captured.record, {
        {test::FaultFilePhase::Read, 2,
         static_cast<std::uint64_t>(kJournalAlignment + 128),
         test::FaultFileAction::ReadbackCorruption, 0}});
    install_successor(*store, captured.token);

    const auto replayed = replay_checkpoint_ack_stream(
        bounded_context(),
        {ack.file, ack.header, ack.stream, store.get(), captured.token});
    REQUIRE(!replayed.ok());
    BlockEntry entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 0, entry));
    REQUIRE_EQ(entry.state, BlockState::ResidentDirty);
    REQUIRE(store->abort_consistent_snapshot(captured.token).ok());
}

TEST_CASE(checkpoint_ack_stream_partial_failure_restarts_idempotently) {
    auto store = make_store();
    std::array<std::byte, kBlockSizeBytes> bytes{};
    bytes.fill(std::byte{0x61});
    REQUIRE(store->write(bounded_context(), 0, bytes).ok());
    bytes.fill(std::byte{0x72});
    REQUIRE(store->write(bounded_context(), kBlockSizeBytes, bytes).ok());
    auto captured = capture_records(*store);
    REQUIRE_EQ(captured.records.size(), std::size_t{2});

    const auto second_record_offset =
        kJournalAlignment + kDataRecordBytes;
    auto ack = make_ack_file(captured.records, {
        {test::FaultFilePhase::Read, 4,
         second_record_offset + 128,
         test::FaultFileAction::ReadbackCorruption, 0}});
    install_successor(*store, captured.token);
    const CheckpointAckReplayRequest request{
        ack.file, ack.header, ack.stream, store.get(), captured.token};

    const auto interrupted = replay_checkpoint_ack_stream(
        bounded_context(), request);
    REQUIRE(!interrupted.ok());
    BlockEntry first{};
    BlockEntry second{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 0, first));
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 1, second));
    REQUIRE_EQ(first.state, BlockState::ResidentClean);
    REQUIRE_EQ(first.clean_generation, 1ull);
    REQUIRE_EQ(second.state, BlockState::ResidentDirty);

    const auto retry = replay_checkpoint_ack_stream(
        bounded_context(), request);
    REQUIRE(retry.ok());
    REQUIRE_EQ(retry.value().records_verified, 2ull);
    REQUIRE_EQ(retry.value().already_persisted, 1ull);
    REQUIRE_EQ(retry.value().acknowledged, 1ull);
    REQUIRE(store->finish_consistent_snapshot(captured.token).ok());
}

}  // namespace
}  // namespace myrd::persistence
