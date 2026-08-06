#include "test_framework.h"

#include "src/persistence/checkpoint_coordinator.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "tests/support/fake_backing_store.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <class T>
concept CallerCanSupplyPublicationIdentity = requires(T request) {
    request.old_generation;
    request.attempted_generation;
    request.attempted_slot_index;
};

static_assert(!CallerCanSupplyPublicationIdentity<CommitResolutionRequest>);
static_assert(!std::default_initializable<FrozenPublicationIdentity>);
static_assert(!std::copy_constructible<FrozenPublicationIdentity>);

JournalHeader make_header(
    const std::uint64_t virtual_bytes = 4 * kM2BlockSize) {
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] =
            static_cast<std::byte>(0x10U + index);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x20U + index);
    }
    header.epoch = 1;
    header.geometry = {virtual_bytes, 512, 4096};
    header.base_checkpoint_generation = 0;
    header.predecessor_kind = JournalPredecessorKind::Genesis;
    header.geometry_digest = compute_geometry_digest(header.geometry).value();
    VhdxFileIdentity base{};
    base.file_write_guid[0] = std::byte{0x31};
    base.data_write_guid[0] = std::byte{0x41};
    header.base_identity_digest = compute_base_identity_digest(base).value();
    header.predecessor_digest = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest).value();
    header.feature_flags = 1;
    return header;
}

WindowsFileIdentity make_file_identity() {
    WindowsFileIdentity identity{};
    identity.volume_serial_number = 23;
    identity.file_id[0] = std::byte{0x51};
    identity.attributes = 1;
    identity.hard_link_count = 1;
    return identity;
}

std::unique_ptr<DynamicBlockStore> make_store(
    const std::uint64_t logical_bytes = 4 * kM2BlockSize) {
    DynamicBlockStoreOptions options{
        2 * kM2BlockSize,
        3 * kM2BlockSize,
        kM2BlockSize,
        2 * kM2BlockSize,
        8,
        64,
        2 * kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
    auto created = DynamicBlockStore::create({
        logical_bytes,
        512,
        options,
        InitialBlockState::CleanZero,
        {},
        0,
    });
    REQUIRE(created.ok());
    return created.move_value();
}

struct WriterFixture final {
    JournalCheckpointRequest request{};
    std::unique_ptr<JournalWriter> writer{};
    std::shared_ptr<test::FaultFileState> state{};
};

WriterFixture make_writer(
    const std::optional<JournalWriterPhase> fail_phase = std::nullopt,
    const std::uint64_t virtual_bytes = 4 * kM2BlockSize,
    test::FaultFileObserver observer = {},
    const std::optional<JournalWriterPhase> throw_phase = std::nullopt) {
    auto fault = test::make_fault_file(
        kJournalAlignment, {}, std::move(observer));
    const auto header = make_header(virtual_bytes);
    REQUIRE(fault.file->write_exact(
        0, encode_journal_header(header).value()).ok());

    JournalWriterOptions options{};
    options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    options.max_checkpoint_records = 8192;
    options.max_checkpoint_bytes = 64ull * 1024 * 1024;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (fail_phase.has_value() || throw_phase.has_value()) {
        options.phase_hook = [fail_phase, throw_phase](
            const JournalWriterPhaseEvent& event) -> Result<void> {
            if (event.phase == throw_phase) {
                throw std::runtime_error("injected writer exception");
            }
            if (fail_phase.has_value() && event.phase == *fail_phase) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::IoError),
                    L"m3_checkpoint_tests",
                    L"injected pre-manifest failure", 0});
            }
            return Result<void>::success();
        };
    }
#else
    (void)fail_phase;
    (void)throw_phase;
#endif
    auto writer = JournalWriter::create(std::move(options));
    REQUIRE(writer.ok());

    JournalCheckpointRequest request{};
    request.file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    request.file_identity = make_file_identity();
    request.header = header;
    request.previous_generation = 0;
    request.previous_digest = header.predecessor_digest;
    request.committed_length = kJournalAlignment;
    request.operation_id = 9001;
    return {std::move(request), writer.move_value(), std::move(fault.state)};
}

OperationContext bounded_context() {
    return OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

void make_mixed_dirty_image(DynamicBlockStore& store) {
    std::array<std::byte, kM2BlockSize> data{};
    data.fill(std::byte{0x5A});
    REQUIRE(store.write(bounded_context(), 0, data).ok());
    data.fill(std::byte{0x6B});
    REQUIRE(store.write(bounded_context(), kM2BlockSize, data).ok());
    REQUIRE(store.write(bounded_context(), 2 * kM2BlockSize, data).ok());
    const DataRange zero_range{2 * kM2BlockSize, kM2BlockSize};
    REQUIRE(store.zero(bounded_context(), {&zero_range, 1}).ok());
    REQUIRE(store.write(bounded_context(), 3 * kM2BlockSize, data).ok());
    const DataRange discard_range{3 * kM2BlockSize, kM2BlockSize};
    REQUIRE(store.unmap(bounded_context(), {&discard_range, 1}).ok());
}

CheckpointPrepareRequest make_checkpoint_request(
    const JournalCheckpointRequest& journal,
    const std::optional<VolumeBarrierKind> barrier,
    std::string operation = "checkpoint.prepare") {
    PersistenceErrorContext context{};
    context.operation = std::move(operation);
    context.code = PersistenceErrorCode::InvalidFormat;
    context.phase = SessionPhase::Ready;
    context.operation_id = journal.operation_id;
    context.image_uuid = journal.header.image_uuid;
    context.generation = journal.previous_generation + 1;
    context.disposition = CommitDisposition::NotCommitted;
    auto complete = CompletePersistenceErrorContext::create(
        std::move(context));
    REQUIRE(complete.has_value());
    CheckpointPrepareRequest request{std::move(*complete)};
    request.volume_barrier = barrier;
    request.journal = journal;
    request.snapshot_batch_bytes = kM2BlockSize;
    return request;
}

ManifestSlot make_active_manifest(const JournalHeader& header) {
    ManifestSlot slot{};
    slot.slot_index = 0;
    slot.image_uuid = header.image_uuid;
    slot.generation.checkpoint_generation = 0;
    slot.generation.base_generation = 0;
    slot.generation.journal.epoch = header.epoch;
    slot.generation.journal.committed_length = 2 * kJournalAlignment;
    slot.generation.journal.commit = {
        kJournalAlignment, kJournalAlignment, 0x12345678,
        header.predecessor_digest};
    slot.geometry = header.geometry;
    slot.base_relative_name = "base-0000.vhdx";
    slot.base_identity.file_write_guid[0] = std::byte{0x31};
    slot.base_identity.data_write_guid[0] = std::byte{0x41};
    slot.base_expected_length = 8ull * 1024 * 1024;
    slot.journal_relative_name = "journal-e0001.rdj";
    slot.journal_identity = header.journal_identity;
    slot.created_time_100ns = 100;
    slot.committed_time_100ns = 100;
    REQUIRE(validate_manifest_slot_semantics(slot).ok());
    return slot;
}

std::unique_ptr<IPersistenceFile> make_manifest_file(
    const ManifestSlot& active,
    const std::vector<test::FaultFileRule>& rules = {},
    std::shared_ptr<test::FaultFileState>* state_out = nullptr) {
    auto fault = test::make_fault_file(kManifestFileBytes, rules);
    if (state_out != nullptr) *state_out = fault.state;
    const auto encoded = encode_manifest_slot(active);
    REQUIRE(encoded.ok());
    REQUIRE(fault.file->write_exact(0, encoded.value()).ok());
    return std::move(fault.file);
}

TEST_CASE(checkpoint_prepare_requires_barrier_before_snapshot_begin) {
    auto store = make_store();
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(writer.request, std::nullopt);

    const auto missing = coordinator.prepare(bounded_context(), request);
    REQUIRE(!missing.ok());
    REQUIRE_EQ(missing.disposition(), CommitDisposition::NotCommitted);
    REQUIRE(missing.failure() != nullptr);
    REQUIRE(is_valid_checkpoint_outcome(missing.failure()->outcome));
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);

    request.volume_barrier = static_cast<VolumeBarrierKind>(0xFF);
    const auto unknown = coordinator.prepare(bounded_context(), request);
    REQUIRE(!unknown.ok());
    REQUIRE_EQ(unknown.disposition(), CommitDisposition::NotCommitted);
    REQUIRE(is_valid_checkpoint_outcome(unknown.failure()->outcome));
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);

    auto malformed_correlation = make_checkpoint_request(
        writer.request, std::nullopt, "caller.supplied.operation");
    const auto malformed = coordinator.prepare(
        bounded_context(), malformed_correlation);
    REQUIRE(!malformed.ok());
    REQUIRE(is_valid_checkpoint_outcome(malformed.failure()->outcome));
    REQUIRE_EQ(malformed.failure()->outcome.error->operation,
               std::wstring{L"checkpoint.prepare"});
    REQUIRE_EQ(malformed.failure()
                   ->outcome.error_context->operation,
               std::string{"checkpoint.prepare"});
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);

    const auto token = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(token.ok());
    REQUIRE(store->abort_consistent_snapshot(token.value()).ok());
}

TEST_CASE(checkpoint_prepare_invalid_journal_still_returns_valid_outcome) {
    auto store = make_store();
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);

    auto invalid_identity = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    invalid_identity.journal.operation_id = 0;
    invalid_identity.journal.header.image_uuid = {};
    const auto identity_result = coordinator.prepare(
        bounded_context(), invalid_identity);
    REQUIRE(!identity_result.ok());
    REQUIRE(is_valid_checkpoint_outcome(
        identity_result.failure()->outcome));
    REQUIRE_EQ(identity_result.failure()
                   ->outcome.error_context->operation_id,
               writer.request.operation_id);
    REQUIRE_EQ(identity_result.failure()
                   ->outcome.error_context->image_uuid,
               writer.request.header.image_uuid);

    auto invalid_generation = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    invalid_generation.journal.previous_generation = kInvalidGeneration;
    const auto generation_result = coordinator.prepare(
        bounded_context(), invalid_generation);
    REQUIRE(!generation_result.ok());
    REQUIRE(is_valid_checkpoint_outcome(
        generation_result.failure()->outcome));
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(checkpoint_prepare_streams_mixed_records_and_owns_abort_lifetime) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::RawBlockTestNoFilesystemCache);

    {
        auto result = coordinator.prepare(bounded_context(), request);
        REQUIRE(result.ok());
        auto prepared = result.move_transaction();
        REQUIRE(prepared.active());
        REQUIRE_EQ(session.phase(), SessionPhase::Checkpointing);
        REQUIRE_EQ(prepared.disposition(), CommitDisposition::NotCommitted);
        REQUIRE_EQ(prepared.volume_barrier(),
                   VolumeBarrierKind::RawBlockTestNoFilesystemCache);
        REQUIRE(!prepared.filesystem_durability_claimed());
        REQUIRE_EQ(prepared.checkpoint().data_count, 2ull);
        REQUIRE_EQ(prepared.checkpoint().zero_count, 1ull);
        REQUIRE_EQ(prepared.checkpoint().discard_count, 1ull);
        REQUIRE_EQ(prepared.metrics().snapshot_records, 4ull);
        REQUIRE(prepared.metrics().snapshot_batches >= 2);
        REQUIRE(prepared.metrics().peak_batch_payload_bytes <=
                request.snapshot_batch_bytes);
        const auto competing_snapshot =
            store->begin_consistent_snapshot(bounded_context());
        REQUIRE(!competing_snapshot.ok());
        REQUIRE_EQ(competing_snapshot.error().code,
                   static_cast<std::uint32_t>(
                       DynamicBlockStoreErrorCode::SnapshotBusy));
        REQUIRE_EQ(writer.state->calls(test::FaultFilePhase::Flush), 1ull);
    }
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE_EQ(store->backing_generation(), 0ull);
    const auto retry = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(retry.ok());
    DirtyScanCursor retry_cursor{};
    std::uint64_t retry_records = 0;
    while (retry_cursor.next_block_index < store->total_blocks()) {
        const auto batch = store->copy_consistent_snapshot_batch(
            bounded_context(), retry.value(), retry_cursor, kM2BlockSize);
        REQUIRE(batch.ok());
        retry_records += batch.value().size();
    }
    REQUIRE_EQ(retry_records, 4ull);
    REQUIRE(store->abort_consistent_snapshot(retry.value()).ok());
}

TEST_CASE(checkpoint_prepare_handles_clean_and_final_partial_images) {
    {
        auto store = make_store();
        auto writer = make_writer();
        auto session = ImageSession::ready();
        CheckpointCoordinator coordinator(session, *store, *writer.writer);
        auto request = make_checkpoint_request(
            writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
        auto result = coordinator.prepare(bounded_context(), request);
        REQUIRE(result.ok());
        auto prepared = result.move_transaction();
        REQUIRE(prepared.filesystem_durability_claimed());
        REQUIRE_EQ(prepared.checkpoint().data_count, 0ull);
        REQUIRE_EQ(prepared.checkpoint().zero_count, 0ull);
        REQUIRE_EQ(prepared.checkpoint().discard_count, 0ull);
        REQUIRE_EQ(prepared.metrics().snapshot_records, 0ull);
        REQUIRE(prepared.active());
        REQUIRE(prepared.abort().ok());
        REQUIRE(!prepared.active());
        REQUIRE(prepared.abort().ok());
        REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    }

    constexpr std::uint64_t partial_bytes = kM2BlockSize + 512;
    auto store = make_store(partial_bytes);
    std::array<std::byte, 512> tail{};
    tail.fill(std::byte{0x7C});
    REQUIRE(store->write(bounded_context(), kM2BlockSize, tail).ok());
    auto writer = make_writer(std::nullopt, partial_bytes);
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    auto result = coordinator.prepare(bounded_context(), request);
    REQUIRE(result.ok());
    auto prepared = result.move_transaction();
    REQUIRE_EQ(prepared.checkpoint().data_count, 1ull);
    REQUIRE_EQ(prepared.metrics().peak_batch_payload_bytes,
               kM2BlockSize);
}

TEST_CASE(checkpoint_prepare_holds_atomic_install_and_ack_fence) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    BlockEntry cut_entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));
    auto successor = myrd::test::FakeBackingStoreBuilder(0xA5).build();

    bool install_rejected = false;
    bool acknowledgement_rejected = false;
    auto writer = make_writer(
        std::nullopt, 4 * kM2BlockSize,
        [&](const test::FaultFilePhase phase, const std::uint64_t) {
            if (phase != test::FaultFilePhase::Flush || install_rejected) {
                return;
            }
            const auto install =
                store->install_backing_view({1, successor});
            install_rejected = !install.ok() &&
                install.error().code == static_cast<std::uint32_t>(
                    DynamicBlockStoreErrorCode::SnapshotBusy);
            const auto acknowledge = store->mark_persisted(
                0, cut_entry.version, cut_entry.dirty_kind, 1);
            acknowledgement_rejected = !acknowledge.ok() &&
                acknowledge.error().code == static_cast<std::uint32_t>(
                    DynamicBlockStoreErrorCode::SnapshotBusy);
        });
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);

    auto result = coordinator.prepare(bounded_context(), request);
    REQUIRE(result.ok());
    REQUIRE(install_rejected);
    REQUIRE(acknowledgement_rejected);
    REQUIRE_EQ(store->backing_generation(), 0ull);
    auto prepared = result.move_transaction();
    REQUIRE(prepared.active());
    REQUIRE(prepared.abort().ok());

    BlockEntry still_dirty{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, still_dirty));
    REQUIRE_EQ(still_dirty.state, BlockState::ResidentDirty);
    REQUIRE_EQ(still_dirty.version, cut_entry.version);
}

TEST_CASE(checkpoint_publish_preattempt_failure_aborts_cut) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    auto prepared_result = coordinator.prepare(bounded_context(), request);
    REQUIRE(prepared_result.ok());
    auto transaction = prepared_result.move_transaction();
    const auto active = make_active_manifest(writer.request.header);

    const auto outcome = transaction.publish_manifest(
        bounded_context(),
        {make_manifest_file(active), active, 0, std::nullopt,
         [](const ManifestSlot&, std::span<const std::byte>) {
             return Result<void>::success();
         }});
    REQUIRE_EQ(outcome.disposition, CommitDisposition::NotCommitted);
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.error_context.has_value());
    REQUIRE(!transaction.active());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    const auto retry = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(retry.ok());
    REQUIRE(store->abort_consistent_snapshot(retry.value()).ok());
}

TEST_CASE(checkpoint_publish_indeterminate_resolves_old_without_clearing_dirty) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    auto prepared_result = coordinator.prepare(bounded_context(), request);
    REQUIRE(prepared_result.ok());
    auto transaction = prepared_result.move_transaction();
    const auto active = make_active_manifest(writer.request.header);
    std::shared_ptr<test::FaultFileState> stale_manifest_state;
    auto manifest = make_manifest_file(active, {
        {test::FaultFilePhase::Flush, 1, std::nullopt,
         test::FaultFileAction::FlushError, 1117}},
        &stale_manifest_state);

    const auto published = transaction.publish_manifest(
        bounded_context(),
        {std::move(manifest), active, 200, std::nullopt,
         [](const ManifestSlot&, std::span<const std::byte>) {
             return Result<void>::success();
         }});
    REQUIRE_EQ(published.disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(published.error_context.has_value());
    REQUIRE_EQ(published.error_context->code,
               PersistenceErrorCode::CommitIndeterminate);
    REQUIRE(transaction.active());
    REQUIRE_EQ(session.phase(), SessionPhase::ResolvingCommit);
    REQUIRE(!transaction.abort().ok());

    bool reflushed = false;
    const auto resolved = transaction.resolve_commit(
        bounded_context(),
        {std::nullopt,
         [active, stale_manifest_state](const OperationContext&)
             -> Result<CommitRecoveryEvidence> {
             REQUIRE(stale_manifest_state->closed());
             return Result<CommitRecoveryEvidence>::success({
                 CommitRecoveryFinding::OldGenerationOnly,
                 active, true});
         },
         [&reflushed](const OperationContext&, const ManifestSlot&)
             -> Result<void> {
             reflushed = true;
             return Result<void>::success();
         }});
    REQUIRE_EQ(resolved.disposition, CommitDisposition::NotCommitted);
    REQUIRE(!reflushed);
    REQUIRE(!transaction.active());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(session.snapshot().first_failure.has_value());
    REQUIRE_EQ(session.snapshot().first_failure->context.code,
               PersistenceErrorCode::CommitIndeterminate);
    const auto duplicate_resolution = transaction.resolve_commit(
        bounded_context(), {});
    REQUIRE_EQ(duplicate_resolution.disposition,
               CommitDisposition::NotCommitted);
    REQUIRE(duplicate_resolution.error.has_value());
    REQUIRE(duplicate_resolution.error_context.has_value());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);

    BlockEntry entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(*store, 0, entry));
    REQUIRE_EQ(entry.state, BlockState::ResidentDirty);
}

TEST_CASE(checkpoint_resolution_manifest_close_failure_fails_closed) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    auto prepared_result = coordinator.prepare(bounded_context(), request);
    REQUIRE(prepared_result.ok());
    auto transaction = prepared_result.move_transaction();
    const auto active = make_active_manifest(writer.request.header);
    auto manifest = make_manifest_file(active, {
        {test::FaultFilePhase::Flush, 1, std::nullopt,
         test::FaultFileAction::FlushError, 1117},
        {test::FaultFilePhase::Close, 1, std::nullopt,
         test::FaultFileAction::CloseError, 5}});

    const auto published = transaction.publish_manifest(
        bounded_context(),
        {std::move(manifest), active, 200, std::nullopt,
         [](const ManifestSlot&, std::span<const std::byte>) {
             return Result<void>::success();
         }});
    REQUIRE_EQ(published.disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(transaction.active());

    bool recovery_called = false;
    const auto resolved = transaction.resolve_commit(
        bounded_context(),
        {std::nullopt,
         [&recovery_called](const OperationContext&)
             -> Result<CommitRecoveryEvidence> {
             recovery_called = true;
             return Result<CommitRecoveryEvidence>::failure({
                 ErrorDomain::persistence,
                 static_cast<std::uint32_t>(
                     PersistenceErrorCode::FatalInternalInvariant),
                 L"m3_checkpoint_tests", L"must not recover", 0});
         },
         [](const OperationContext&, const ManifestSlot&)
             -> Result<void> {
             return Result<void>::success();
         }});

    REQUIRE(!recovery_called);
    REQUIRE_EQ(resolved.disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(resolved.error.has_value());
    REQUIRE_EQ(resolved.error->native_code, 5u);
    REQUIRE(resolved.error_context.has_value());
    REQUIRE_EQ(resolved.error_context->disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(transaction.active());
    REQUIRE_EQ(session.phase(), SessionPhase::PersistenceFaulted);
    REQUIRE_EQ(session.snapshot().disposition,
               CommitDisposition::CommitIndeterminate);
}

TEST_CASE(checkpoint_publish_success_hands_commit_proof_to_task32) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    auto prepared_result = coordinator.prepare(bounded_context(), request);
    REQUIRE(prepared_result.ok());
    {
        auto transaction = prepared_result.move_transaction();
        const auto active = make_active_manifest(writer.request.header);
        const auto published = transaction.publish_manifest(
            bounded_context(),
            {make_manifest_file(active), active, 200, std::nullopt,
             [](const ManifestSlot&, std::span<const std::byte>) {
                 return Result<void>::success();
             }});
        REQUIRE(published.ok());
        REQUIRE_EQ(transaction.disposition(),
                   CommitDisposition::Committed);
        REQUIRE(transaction.active());
        REQUIRE_EQ(session.phase(), SessionPhase::Checkpointing);
        REQUIRE_EQ(session.snapshot().publication_stage,
                   PublicationStage::CommitProven);
        REQUIRE(!transaction.abort().ok());
    }
    REQUIRE_EQ(session.phase(), SessionPhase::Reconciling);
    const auto retry = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(retry.ok());
    REQUIRE(store->abort_consistent_snapshot(retry.value()).ok());
}

TEST_CASE(checkpoint_prepare_journals_cut_preimage_when_ram_is_overwritten) {
    auto store = make_store();
    std::array<std::byte, kM2BlockSize> before{};
    std::array<std::byte, kM2BlockSize> after{};
    before.fill(std::byte{0x34});
    after.fill(std::byte{0xA9});
    REQUIRE(store->write(bounded_context(), 0, before).ok());
    BlockEntry cut_entry{};
    REQUIRE(DynamicBlockStoreTestAccess::snapshot_entry(
        *store, 0, cut_entry));

    bool overwritten = false;
    auto writer = make_writer(
        std::nullopt, 4 * kM2BlockSize,
        [&](const test::FaultFilePhase phase, const std::uint64_t) {
            if (phase == test::FaultFilePhase::Read && !overwritten) {
                overwritten = true;
                REQUIRE(store->write(bounded_context(), 0, after).ok());
            }
        });
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::RawBlockTestNoFilesystemCache);

    auto result = coordinator.prepare(bounded_context(), request);
    REQUIRE(result.ok());
    REQUIRE(overwritten);
    auto prepared = result.move_transaction();
    REQUIRE_EQ(prepared.checkpoint().data_count, 1ull);
    std::array<std::byte, kDataRecordBytes> encoded{};
    REQUIRE(writer.request.file->read_exact(
        request.journal.committed_length, encoded).ok());
    const auto decoded = decode_block_record(
        encoded,
        request.journal.committed_length,
        request.journal.committed_length + encoded.size(),
        request.journal.header.geometry);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().metadata.dirty_epoch,
               cut_entry.dirty_epoch);
    REQUIRE_EQ(decoded.value().metadata.snapshot_version,
               cut_entry.version);
    REQUIRE(std::equal(
        before.begin(), before.end(), decoded.value().payload.begin()));
}

TEST_CASE(checkpoint_prepare_cancelled_context_leaks_no_gate_or_snapshot) {
    auto store = make_store();
    auto writer = make_writer();
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    std::stop_source stop;
    stop.request_stop();

    const auto result = coordinator.prepare(
        OperationContext{stop.get_token()}, request);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.disposition(), CommitDisposition::NotCommitted);
    REQUIRE(is_valid_checkpoint_outcome(result.failure()->outcome));
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    const auto token = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(token.ok());
    REQUIRE(store->abort_consistent_snapshot(token.value()).ok());

    auto deadline_store = make_store();
    auto deadline_writer = make_writer();
    auto deadline_session = ImageSession::ready();
    CheckpointCoordinator deadline_coordinator(
        deadline_session, *deadline_store, *deadline_writer.writer);
    request.journal = deadline_writer.request;
    const auto timed_out = deadline_coordinator.prepare(
        OperationContext{{}, std::chrono::steady_clock::now()}, request);
    REQUIRE(!timed_out.ok());
    REQUIRE_EQ(timed_out.failure()->outcome.error_context->code,
               PersistenceErrorCode::DeadlineExceeded);
    REQUIRE_EQ(deadline_session.phase(), SessionPhase::Ready);

    auto mid_store = make_store();
    make_mixed_dirty_image(*mid_store);
    std::stop_source mid_stop;
    bool cancellation_requested = false;
    auto mid_writer = make_writer(
        std::nullopt, 4 * kM2BlockSize,
        [&](const test::FaultFilePhase phase, const std::uint64_t) {
            if (phase == test::FaultFilePhase::Read &&
                !cancellation_requested) {
                cancellation_requested = true;
                mid_stop.request_stop();
            }
        });
    auto mid_session = ImageSession::ready();
    CheckpointCoordinator mid_coordinator(
        mid_session, *mid_store, *mid_writer.writer);
    auto mid_request = make_checkpoint_request(
        mid_writer.request, VolumeBarrierKind::WindowsVolumeFlushed);
    const auto cancelled_mid_prepare = mid_coordinator.prepare(
        OperationContext{mid_stop.get_token()}, mid_request);
    REQUIRE(cancellation_requested);
    REQUIRE(!cancelled_mid_prepare.ok());
    REQUIRE_EQ(cancelled_mid_prepare.failure()
                   ->outcome.error_context->code,
               PersistenceErrorCode::Cancelled);
    REQUIRE_EQ(mid_session.phase(), SessionPhase::Ready);
}

TEST_CASE(checkpoint_prepare_exception_unwinds_snapshot_and_session) {
    auto store = make_store();
    make_mixed_dirty_image(*store);
    auto writer = make_writer(
        std::nullopt, 4 * kM2BlockSize, {},
        JournalWriterPhase::BeforeIndexWrite);
    auto session = ImageSession::ready();
    CheckpointCoordinator coordinator(session, *store, *writer.writer);
    auto request = make_checkpoint_request(
        writer.request, VolumeBarrierKind::WindowsVolumeFlushed);

    bool exception_observed = false;
    try {
        (void)coordinator.prepare(bounded_context(), request);
    } catch (const std::runtime_error&) {
        exception_observed = true;
    }
    REQUIRE(exception_observed);
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    const auto retry = store->begin_consistent_snapshot(bounded_context());
    REQUIRE(retry.ok());
    REQUIRE(store->abort_consistent_snapshot(retry.value()).ok());
}

TEST_CASE(checkpoint_prepare_failure_aborts_snapshot_and_session) {
    constexpr JournalWriterPhase phases[]{
        JournalWriterPhase::BeforeBlockWrite,
        JournalWriterPhase::AfterBlockWrite,
        JournalWriterPhase::BeforeIndexWrite,
        JournalWriterPhase::AfterIndexWrite,
        JournalWriterPhase::BeforeCommitWrite,
        JournalWriterPhase::AfterCommitWrite,
        JournalWriterPhase::BeforeFlush,
        JournalWriterPhase::AfterFlush,
        JournalWriterPhase::BeforeReadback,
        JournalWriterPhase::AfterReadback,
        JournalWriterPhase::BeforeIndexCrosscheck,
        JournalWriterPhase::AfterIndexCrosscheck,
        JournalWriterPhase::Prepared,
    };
    for (const auto phase : phases) {
        auto store = make_store();
        make_mixed_dirty_image(*store);
        auto writer = make_writer(phase);
        auto session = ImageSession::ready();
        CheckpointCoordinator coordinator(session, *store, *writer.writer);
        auto request = make_checkpoint_request(
            writer.request, VolumeBarrierKind::WindowsVolumeFlushed);

        const auto result = coordinator.prepare(bounded_context(), request);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.disposition(), CommitDisposition::NotCommitted);
        REQUIRE(is_valid_checkpoint_outcome(result.failure()->outcome));
        REQUIRE_EQ(session.phase(), SessionPhase::Ready);
        const auto snapshot = session.snapshot();
        REQUIRE_EQ(snapshot.disposition, CommitDisposition::NotCommitted);
        REQUIRE(snapshot.first_failure.has_value());
        REQUIRE_EQ(snapshot.first_failure->context.disposition,
                   CommitDisposition::NotCommitted);
        const auto retry_token =
            store->begin_consistent_snapshot(bounded_context());
        REQUIRE(retry_token.ok());
        REQUIRE(store->abort_consistent_snapshot(retry_token.value()).ok());
    }
}

}  // namespace
}  // namespace myrd::persistence
