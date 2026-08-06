#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "test_framework.h"

#include "src/common/crc32c.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"
#include "src/common/unique_handle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

void write_u32(ManifestSlotBytes& bytes, const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(ManifestSlotBytes& bytes, const std::size_t offset,
               const std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::uint32_t read_u32(const std::span<const std::byte> bytes,
                       const std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8U);
    }
    return value;
}

void refresh_manifest_crc(ManifestSlotBytes& bytes) {
    write_u32(bytes, 24, 0);
    write_u32(bytes, 24, crc32c(bytes));
}

void refresh_journal_header_crc(JournalHeaderBytes& bytes) {
    write_u32(bytes, 20, 0);
    write_u32(bytes, 20, crc32c(bytes));
}

void refresh_vhdx_header_crc(std::array<std::byte, 4096>& bytes) {
    write_u32(bytes, 4, 0);
    write_u32(bytes, 4, crc32c(bytes));
}

class RecoverySandbox final {
public:
    RecoverySandbox() {
        static std::uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-task26-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(sequence++));
        std::filesystem::create_directories(root_);
    }

    ~RecoverySandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

class LockedFileRange final {
public:
    LockedFileRange(const std::filesystem::path& path,
                    const std::uint64_t offset,
                    const std::uint64_t length)
        : length_(length) {
        handle_.reset(CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_RANDOM_ACCESS,
            nullptr));
        REQUIRE(static_cast<bool>(handle_));
        overlapped_.Offset = static_cast<DWORD>(offset);
        overlapped_.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        REQUIRE(LockFileEx(
            handle_.get(),
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, static_cast<DWORD>(length),
            static_cast<DWORD>(length >> 32U), &overlapped_));
        locked_ = true;
    }

    ~LockedFileRange() noexcept {
        if (locked_) {
            static_cast<void>(UnlockFileEx(
                handle_.get(), 0,
                static_cast<DWORD>(length_),
                static_cast<DWORD>(length_ >> 32U), &overlapped_));
        }
    }

    LockedFileRange(const LockedFileRange&) = delete;
    LockedFileRange& operator=(const LockedFileRange&) = delete;

private:
    UniqueHandle handle_;
    OVERLAPPED overlapped_{};
    std::uint64_t length_{};
    bool locked_{};
};

class PayloadSource final : public IVhdxPayloadSource {
public:
    std::map<std::uint64_t, std::vector<std::byte>> payloads;

    Result<void> read_payload(
        const OperationContext&, const std::uint64_t index,
        const std::span<std::byte> destination) const override {
        const auto found = payloads.find(index);
        if (found == payloads.end() ||
            found->second.size() != destination.size()) {
            return Result<void>::failure({
                ErrorDomain::internal, 0x6260,
                L"PayloadSource::read_payload", L"fixture payload missing"});
        }
        std::copy(found->second.begin(), found->second.end(),
                  destination.begin());
        return Result<void>::success();
    }
};

struct RecoveryFixture final {
    RecoverySandbox sandbox;
    ImageGeometry geometry{8 * kMiB, 512, 4096};
    ImageUuid image_uuid{};
    VhdxWriterIdentity base_identity{};
    JournalHeader journal_header{};
    PreparedCheckpoint checkpoint{};
    ManifestSlot valid_a{};
    std::filesystem::path manifest_path;
    std::filesystem::path base_path;
    std::filesystem::path journal_path;

    RecoveryFixture() {
        image_uuid.bytes[0] = std::byte{0x31};
        base_identity.file_identity.file_write_guid[0] = std::byte{0x41};
        base_identity.file_identity.data_write_guid[0] = std::byte{0x42};
        base_identity.virtual_disk_id[0] = std::byte{0x43};
        manifest_path = sandbox.root() / L"disk.rdm";
        base_path = sandbox.root() / L"base.vhdx";
        journal_path = sandbox.root() / L"journal.rdj";
        build_base();
        build_journal();
        valid_a = make_slot(0);
        publish(valid_a, std::nullopt);
    }

    ManifestSlot make_slot(const std::uint8_t index) const {
        return make_slot(index, checkpoint);
    }

    ManifestSlot make_slot(
        const std::uint8_t index,
        const PreparedCheckpoint& selected_checkpoint) const {
        ManifestSlot slot{};
        slot.slot_index = index;
        slot.image_uuid = image_uuid;
        slot.generation.checkpoint_generation =
            selected_checkpoint.generation;
        slot.generation.base_generation = 0;
        slot.generation.journal.epoch = journal_header.epoch;
        slot.generation.journal.committed_length =
            selected_checkpoint.committed_length;
        slot.generation.journal.commit = selected_checkpoint.commit;
        slot.generation.journal.root = selected_checkpoint.root;
        slot.geometry = geometry;
        slot.base_relative_name = "base.vhdx";
        slot.base_identity = base_identity.file_identity;
        slot.base_expected_length =
            static_cast<std::uint64_t>(std::filesystem::file_size(base_path));
        slot.journal_relative_name = "journal.rdj";
        slot.journal_identity = journal_header.journal_identity;
        return slot;
    }

    PreparedCheckpoint append_checkpoint(
        const std::byte payload_byte = std::byte{0x7b}) const {
        auto opened = open_windows_persistence_file(
            journal_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(opened.ok());
        auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
        std::array<std::byte, kM2BlockSize> payload{};
        payload.fill(payload_byte);
        const std::array<JournalCheckpointBlockInput, 1> blocks{{
            {JournalRecordType::Data, 1, 9, checkpoint.generation, 4,
             payload}}};
        JournalCheckpointRequest request{};
        request.file = file;
        request.file_identity.volume_serial_number = 1;
        request.file_identity.file_id[0] = std::byte{1};
        request.file_identity.size = checkpoint.committed_length;
        request.file_identity.hard_link_count = 1;
        request.header = journal_header;
        request.previous_generation = checkpoint.generation;
        request.previous_digest = checkpoint.chain_digest;
        request.committed_length = checkpoint.committed_length;
        request.root = checkpoint.root;
        request.operation_id = 2602;
        request.blocks = blocks;
        JournalWriterOptions options{};
        options.index_builder = {
            {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
        options.max_checkpoint_records = 8192;
        options.max_checkpoint_bytes = 64 * kMiB;
        auto writer = JournalWriter::create(options);
        REQUIRE(writer.ok());
        auto prepared = writer.value()->prepare(OperationContext{}, request);
        REQUIRE(prepared.ok());
        REQUIRE(file->close().ok());
        return prepared.move_value();
    }

    void publish(const std::optional<ManifestSlot>& a,
                 const std::optional<ManifestSlot>& b) const {
        std::array<std::byte, kManifestFileBytes> bytes{};
        if (a.has_value()) {
            const auto encoded = encode_manifest_slot(*a);
            REQUIRE(encoded.ok());
            std::copy(encoded.value().begin(), encoded.value().end(),
                      bytes.begin());
        }
        if (b.has_value()) {
            const auto encoded = encode_manifest_slot(*b);
            REQUIRE(encoded.ok());
            std::copy(encoded.value().begin(), encoded.value().end(),
                      bytes.begin() + kManifestSlotBytes);
        }
        std::ofstream file(manifest_path, std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }

private:
    void build_base() {
        PayloadSource source;
        source.payloads.emplace(
            0, std::vector<std::byte>(kVhdxDataBlockBytes,
                                     std::byte{0x5b}));
        const std::array<std::uint64_t, 1> candidates{0};
        auto plan = plan_sparse_vhdx(
            OperationContext{}, {geometry.virtual_bytes,
                                 geometry.logical_sector_size},
            base_identity, candidates, source,
            {8, 8, kVhdxDataBlockBytes});
        REQUIRE(plan.ok());
        auto opened = open_windows_persistence_file(
            base_path, FileOpenMode::CreateNewReadWrite);
        REQUIRE(opened.ok());
        auto written = write_sparse_vhdx_file(
            OperationContext{}, plan.value(), source, opened.move_value());
        REQUIRE(written.ok());
        auto file = written.value().release_file();
        REQUIRE(file->close().ok());
    }

    void build_journal() {
        journal_header.image_uuid = image_uuid;
        journal_header.journal_identity.journal_uuid[0] = std::byte{0x51};
        journal_header.epoch = 1;
        journal_header.geometry = geometry;
        journal_header.geometry_digest =
            compute_geometry_digest(geometry).value();
        journal_header.base_identity_digest =
            compute_base_identity_digest(base_identity.file_identity).value();
        journal_header.predecessor_digest = compute_genesis_digest(
            image_uuid, journal_header.geometry_digest,
            journal_header.base_identity_digest).value();

        auto opened = open_windows_persistence_file(
            journal_path, FileOpenMode::CreateNewReadWrite);
        REQUIRE(opened.ok());
        auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
        const auto header_bytes = encode_journal_header(journal_header);
        REQUIRE(header_bytes.ok());
        REQUIRE(file->write_exact(0, header_bytes.value()).ok());

        std::array<std::byte, kM2BlockSize> payload{};
        payload.fill(std::byte{0x6a});
        const std::array<JournalCheckpointBlockInput, 2> blocks{{
            {JournalRecordType::Data, 1, 7, 0, 3, payload},
            {JournalRecordType::Zero, 2, 8, 0, 3, {}}}};
        JournalCheckpointRequest request{};
        request.file = file;
        request.file_identity.volume_serial_number = 1;
        request.file_identity.file_id[0] = std::byte{1};
        request.file_identity.size = kJournalAlignment;
        request.file_identity.hard_link_count = 1;
        request.header = journal_header;
        request.previous_digest = journal_header.predecessor_digest;
        request.committed_length = kJournalAlignment;
        request.operation_id = 2601;
        request.blocks = blocks;
        JournalWriterOptions options{};
        options.index_builder = {
            {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
        options.max_checkpoint_records = 8192;
        options.max_checkpoint_bytes = 64 * kMiB;
        auto writer = JournalWriter::create(options);
        REQUIRE(writer.ok());
        auto prepared = writer.value()->prepare(OperationContext{}, request);
        REQUIRE(prepared.ok());
        checkpoint = prepared.move_value();
        REQUIRE(file->close().ok());
    }
};

PersistenceLimits recovery_limits() {
    return {
        64 * kMiB, 64 * kMiB, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * kMiB, 2 * kMiB};
}

RecoveryOptions recovery_options() {
    RecoveryOptions options{};
    options.limits = recovery_limits();
    return options;
}

void require_selected(const RecoveryOutcome& outcome,
                      const std::uint8_t slot,
                      const bool fallback = false) {
    REQUIRE(outcome.ok());
    REQUIRE(outcome.image.has_value());
    REQUIRE_EQ(outcome.image->selected_slot, slot);
    REQUIRE_EQ(outcome.image->fallback, fallback);
    REQUIRE_EQ(outcome.image->checkpoint_generation,
               outcome.image->backing_view.generation);
    REQUIRE(outcome.image->backing_view.store != nullptr);
    REQUIRE_EQ(outcome.diagnostics[slot].selected, true);
}

TEST_CASE(recovery_selects_a_only_b_only_and_equivalent_tie) {
    RecoveryFixture fixture;
    auto a_only = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(a_only, 0);

    auto b = fixture.make_slot(1);
    fixture.publish(std::nullopt, b);
    auto b_only = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(b_only, 1);

    fixture.publish(fixture.valid_a, b);
    auto tied = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(tied, 0);
}

TEST_CASE(recovery_selects_real_newer_complete_generation_and_isolates_views) {
    RecoveryFixture fixture;
    auto old_outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(old_outcome, 0);
    auto checkpoint_two = fixture.append_checkpoint();
    auto newer = fixture.make_slot(1, checkpoint_two);
    fixture.publish(fixture.valid_a, newer);

    auto new_outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(new_outcome, 1);
    REQUIRE_EQ(new_outcome.image->checkpoint_generation,
               fixture.checkpoint.generation + 1);
    std::array<std::byte, kM2BlockSize> old_bytes{};
    std::array<std::byte, kM2BlockSize> new_bytes{};
    auto old_read = old_outcome.image->backing_view.store->load_block(
        OperationContext{}, 1, old_bytes);
    auto new_read = new_outcome.image->backing_view.store->load_block(
        OperationContext{}, 1, new_bytes);
    REQUIRE(old_read.ok());
    REQUIRE(new_read.ok());
    REQUIRE(std::all_of(old_bytes.begin(), old_bytes.end(),
        [](const std::byte value) { return value == std::byte{0x6a}; }));
    REQUIRE(std::all_of(new_bytes.begin(), new_bytes.end(),
        [](const std::byte value) { return value == std::byte{0x7b}; }));
}

TEST_CASE(recovery_opens_generation_zero_empty_commit_and_root) {
    RecoveryFixture fixture;
    CheckpointCommitRecord commit{};
    commit.common.type = JournalRecordType::CheckpointCommit;
    commit.common.checkpoint_generation = 0;
    commit.common.operation_id = 2600;
    commit.image_uuid = fixture.image_uuid;
    commit.journal_identity = fixture.journal_header.journal_identity;
    commit.journal_epoch = fixture.journal_header.epoch;
    commit.previous_generation = 0;
    commit.new_generation = 0;
    commit.previous_digest = fixture.journal_header.predecessor_digest;
    commit.new_chain_digest = fixture.journal_header.predecessor_digest;
    commit.checkpoint_begin_offset = kJournalAlignment;
    commit.journal_committed_end = 2 * kJournalAlignment;
    commit.geometry_digest = fixture.journal_header.geometry_digest;
    auto commit_bytes = encode_checkpoint_commit(
        commit, fixture.journal_header, kJournalAlignment,
        2 * kJournalAlignment);
    REQUIRE(commit_bytes.ok());
    const auto header_bytes = encode_journal_header(fixture.journal_header);
    REQUIRE(header_bytes.ok());
    auto journal = open_windows_persistence_file(
        fixture.journal_path, FileOpenMode::ReadWriteExisting);
    REQUIRE(journal.ok());
    REQUIRE(journal.value()->truncate(2 * kJournalAlignment).ok());
    REQUIRE(journal.value()->write_exact(0, header_bytes.value()).ok());
    REQUIRE(journal.value()->write_exact(
        kJournalAlignment, commit_bytes.value()).ok());
    REQUIRE(journal.value()->flush().ok());
    REQUIRE(journal.value()->close().ok());

    auto slot = fixture.make_slot(0);
    slot.generation.checkpoint_generation = 0;
    slot.generation.journal.committed_length = 2 * kJournalAlignment;
    slot.generation.journal.commit.offset = kJournalAlignment;
    slot.generation.journal.commit.length = kJournalAlignment;
    slot.generation.journal.commit.crc32c =
        read_u32(commit_bytes.value(), 32);
    slot.generation.journal.commit.chain_digest =
        fixture.journal_header.predecessor_digest;
    slot.generation.journal.root = {};
    fixture.publish(slot, std::nullopt);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0);
    REQUIRE_EQ(outcome.image->checkpoint_generation, std::uint64_t{0});
    std::array<std::byte, kM2BlockSize> bytes{};
    auto loaded = outcome.image->backing_view.store->load_block(
        OperationContext{}, 7, bytes);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0x5b};
    }));
}

TEST_CASE(recovery_view_reads_journal_first_then_base_without_eager_payload) {
    RecoveryFixture fixture;
    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(outcome, 0);
    REQUIRE_EQ(outcome.diagnostics[0].base_structural_read_calls,
               std::uint64_t{11});
    REQUIRE_EQ(outcome.diagnostics[0].base_structural_read_bytes,
               std::uint64_t{204848});
    REQUIRE_EQ(outcome.diagnostics[0].base_bat_read_calls_at_open,
               std::uint64_t{0});
    REQUIRE_EQ(outcome.diagnostics[0].base_payload_read_calls_at_open,
               std::uint64_t{0});
    std::array<std::byte, kM2BlockSize> bytes{};

    auto journal = outcome.image->backing_view.store->load_block(
        OperationContext{}, 1, bytes);
    REQUIRE(journal.ok());
    REQUIRE_EQ(journal.value().kind, BlockLoadKind::Data);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0x6a};
    }));

    bytes.fill(std::byte{0xa5});
    auto zero = outcome.image->backing_view.store->load_block(
        OperationContext{}, 2, bytes);
    REQUIRE(zero.ok());
    REQUIRE_EQ(zero.value().kind, BlockLoadKind::AllZero);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0xa5};
    }));

    auto base = outcome.image->backing_view.store->load_block(
        OperationContext{}, 0, bytes);
    REQUIRE(base.ok());
    REQUIRE_EQ(base.value().kind, BlockLoadKind::Data);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0x5b};
    }));
}

TEST_CASE(recovery_falls_back_only_from_a_higher_trusted_generation) {
    RecoveryFixture fixture;
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    fixture.publish(fixture.valid_a, newer);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0, true);
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::JournalCommit});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::Corrupt);
}

TEST_CASE(recovery_does_not_trust_a_torn_slots_generation) {
    RecoveryFixture fixture;
    auto b = fixture.make_slot(1);
    b.generation.checkpoint_generation += 1;
    fixture.publish(fixture.valid_a, std::nullopt);
    auto encoded = encode_manifest_slot(b);
    REQUIRE(encoded.ok());
    encoded.value()[100] ^= std::byte{0x01};
    auto manifest = open_windows_persistence_file(
        fixture.manifest_path, FileOpenMode::ReadWriteExisting);
    REQUIRE(manifest.ok());
    REQUIRE(manifest.value()->write_exact(
        kManifestSlotBytes, encoded.value()).ok());
    REQUIRE(manifest.value()->close().ok());

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0, false);
    REQUIRE(!outcome.diagnostics[1].generation.has_value());
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::SlotDecode});
}

TEST_CASE(recovery_never_uses_operational_error_as_fallback_authority) {
    RecoveryFixture fixture;
    const auto busy_path = fixture.sandbox.root() / L"busy.vhdx";
    std::filesystem::copy_file(fixture.base_path, busy_path);
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    newer.base_relative_name = "busy.vhdx";
    fixture.publish(fixture.valid_a, newer);
    UniqueHandle held{CreateFileW(
        busy_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(static_cast<bool>(held));

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.image.has_value());
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.error->code == static_cast<std::uint32_t>(
                PersistenceErrorCode::IoError) ||
            outcome.error->code == static_cast<std::uint32_t>(
                PersistenceErrorCode::AccessDenied));
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::BaseOpen});
    REQUIRE(!outcome.diagnostics[0].selected);
}

TEST_CASE(recovery_root_io_error_in_newer_generation_blocks_fallback) {
    RecoveryFixture fixture;
    const auto newer_checkpoint = fixture.append_checkpoint();
    auto newer = fixture.make_slot(1, newer_checkpoint);
    fixture.publish(fixture.valid_a, newer);
    LockedFileRange locked{
        fixture.journal_path, newer_checkpoint.root.offset,
        newer_checkpoint.root.length};

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.image.has_value());
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::JournalRoot});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::IoError);
    REQUIRE(!outcome.diagnostics[0].selected);
}

TEST_CASE(recovery_digest_io_error_in_newer_generation_blocks_fallback) {
    RecoveryFixture fixture;
    const auto newer_checkpoint = fixture.append_checkpoint();
    auto newer = fixture.make_slot(1, newer_checkpoint);
    fixture.publish(fixture.valid_a, newer);
    LockedFileRange locked{
        fixture.journal_path, fixture.checkpoint.committed_length,
        kJournalAlignment};

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.image.has_value());
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::JournalDigest});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::IoError);
    REQUIRE(!outcome.diagnostics[0].selected);
}

TEST_CASE(recovery_manifest_slot_read_error_preserves_slot_provenance) {
    RecoveryFixture fixture;
    auto b = fixture.make_slot(1);
    fixture.publish(fixture.valid_a, b);
    LockedFileRange locked{
        fixture.manifest_path, kManifestSlotBytes, kManifestSlotBytes};

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.image.has_value());
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::SlotDecode});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::IoError);
    REQUIRE(outcome.diagnostics[1].has_error_provenance);
    REQUIRE(!outcome.diagnostics[1].safe_context.empty());
}

TEST_CASE(recovery_treats_handle_proven_hardlink_as_path_corruption) {
    RecoveryFixture fixture;
    const auto linked_path = fixture.sandbox.root() / L"linked.vhdx";
    const auto alias_path = fixture.sandbox.root() / L"linked-alias.vhdx";
    std::filesystem::copy_file(fixture.base_path, linked_path);
    REQUIRE(CreateHardLinkW(alias_path.c_str(), linked_path.c_str(), nullptr));
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    newer.base_relative_name = "linked.vhdx";
    fixture.publish(fixture.valid_a, newer);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0, true);
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::BasePath});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::Corrupt);
}

TEST_CASE(recovery_rejects_missing_replaced_and_truncated_references) {
    {
        RecoveryFixture fixture;
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.base_relative_name = "missing.vhdx";
        fixture.publish(fixture.valid_a, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::BasePresence});
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Missing);
    }
    {
        RecoveryFixture fixture;
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.base_identity.file_write_guid[1] = std::byte{0x77};
        fixture.publish(fixture.valid_a, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::BaseIdentity});
    }
    {
        RecoveryFixture fixture;
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.journal_relative_name = "journal-truncated.rdj";
        const auto truncated_path =
            fixture.sandbox.root() / L"journal-truncated.rdj";
        std::filesystem::copy_file(fixture.journal_path, truncated_path);
        auto truncated = open_windows_persistence_file(
            truncated_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(truncated.ok());
        REQUIRE(truncated.value()->truncate(
            fixture.checkpoint.committed_length - kJournalAlignment).ok());
        REQUIRE(truncated.value()->close().ok());
        fixture.publish(fixture.valid_a, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{
                       ManifestCandidateCheck::JournalCommittedLength});
    }
}

TEST_CASE(recovery_failure_and_fallback_are_symmetric_across_slots) {
    RecoveryFixture fixture;
    auto higher_a = fixture.make_slot(0);
    higher_a.generation.checkpoint_generation += 1;
    higher_a.journal_relative_name = "missing-journal.rdj";
    auto lower_b = fixture.make_slot(1);
    fixture.publish(higher_a, lower_b);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 1, true);
    REQUIRE_EQ(outcome.diagnostics[0].first_failed_check,
               std::optional{ManifestCandidateCheck::JournalPresence});
    REQUIRE_EQ(outcome.diagnostics[0].status,
               ManifestCandidateCheckStatus::Missing);
}

TEST_CASE(recovery_ignores_and_preserves_uncommitted_journal_tail) {
    RecoveryFixture fixture;
    auto journal = open_windows_persistence_file(
        fixture.journal_path, FileOpenMode::ReadWriteExisting);
    REQUIRE(journal.ok());
    std::array<std::byte, kJournalAlignment> tail{};
    tail.fill(std::byte{0xdd});
    REQUIRE(journal.value()->write_exact(
        fixture.checkpoint.committed_length, tail).ok());
    REQUIRE(journal.value()->flush().ok());
    REQUIRE(journal.value()->close().ok());
    const auto size_before = std::filesystem::file_size(fixture.journal_path);
    const auto time_before =
        std::filesystem::last_write_time(fixture.journal_path);
    const auto base_size_before =
        std::filesystem::file_size(fixture.base_path);
    const auto base_time_before =
        std::filesystem::last_write_time(fixture.base_path);
    const auto manifest_size_before =
        std::filesystem::file_size(fixture.manifest_path);
    const auto manifest_time_before =
        std::filesystem::last_write_time(fixture.manifest_path);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0);
    REQUIRE_EQ(std::filesystem::file_size(fixture.journal_path), size_before);
    REQUIRE_EQ(std::filesystem::last_write_time(fixture.journal_path),
               time_before);
    REQUIRE_EQ(std::filesystem::file_size(fixture.base_path),
               base_size_before);
    REQUIRE_EQ(std::filesystem::last_write_time(fixture.base_path),
               base_time_before);
    REQUIRE_EQ(std::filesystem::file_size(fixture.manifest_path),
               manifest_size_before);
    REQUIRE_EQ(std::filesystem::last_write_time(fixture.manifest_path),
               manifest_time_before);
}

TEST_CASE(recovery_fails_closed_for_both_invalid_split_brain_and_unsupported) {
    {
        RecoveryFixture fixture;
        fixture.publish(std::nullopt, std::nullopt);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE(outcome.error.has_value());
        REQUIRE_EQ(outcome.diagnostics[0].status,
                   ManifestCandidateCheckStatus::Corrupt);
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Corrupt);
    }
    {
        RecoveryFixture fixture;
        auto conflict = fixture.make_slot(1);
        conflict.base_relative_name = "other.vhdx";
        fixture.publish(fixture.valid_a, conflict);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.diagnostics[0].status,
                   ManifestCandidateCheckStatus::Conflict);
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Conflict);
    }
    {
        RecoveryFixture fixture;
        fixture.publish(std::nullopt, std::nullopt);
        auto file = open_windows_persistence_file(
            fixture.manifest_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(file.ok());
        auto encoded = encode_manifest_slot(fixture.valid_a);
        REQUIRE(encoded.ok());
        encoded.value()[8] = std::byte{0x7f};
        refresh_manifest_crc(encoded.value());
        REQUIRE(file.value()->write_exact(0, encoded.value()).ok());
        REQUIRE(file.value()->close().ok());
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.diagnostics[0].status,
                   ManifestCandidateCheckStatus::Unsupported);
        REQUIRE_EQ(outcome.diagnostics[0].first_failed_check,
                   std::optional{ManifestCandidateCheck::SlotSupport});
        REQUIRE(!outcome.image.has_value());
    }
}

TEST_CASE(recovery_separates_slot_support_semantics_and_path_evidence) {
    {
        RecoveryFixture fixture;
        auto b = fixture.make_slot(1);
        auto encoded = encode_manifest_slot(b);
        REQUIRE(encoded.ok());
        write_u64(encoded.value(), 760, 1ULL << 32U);
        refresh_manifest_crc(encoded.value());
        auto manifest = open_windows_persistence_file(
            fixture.manifest_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(manifest.ok());
        REQUIRE(manifest.value()->write_exact(
            kManifestSlotBytes, encoded.value()).ok());
        REQUIRE(manifest.value()->close().ok());
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, false);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::SlotSupport});
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Unsupported);
        REQUIRE(!outcome.diagnostics[1].generation.has_value());
    }
    {
        RecoveryFixture fixture;
        auto b = fixture.make_slot(1);
        auto encoded = encode_manifest_slot(b);
        REQUIRE(encoded.ok());
        write_u32(encoded.value(), 72,
                  static_cast<std::uint32_t>(kM2BlockSize / 2));
        refresh_manifest_crc(encoded.value());
        auto manifest = open_windows_persistence_file(
            fixture.manifest_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(manifest.ok());
        REQUIRE(manifest.value()->write_exact(
            kManifestSlotBytes, encoded.value()).ok());
        REQUIRE(manifest.value()->close().ok());
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, false);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::SlotSemantics});
        REQUIRE(!outcome.diagnostics[1].generation.has_value());
    }
    {
        RecoveryFixture fixture;
        auto physical_b_with_a_index = fixture.make_slot(0);
        fixture.publish(fixture.valid_a, physical_b_with_a_index);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, false);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::SlotSemantics});
    }
    {
        RecoveryFixture fixture;
        auto b = fixture.make_slot(1);
        auto encoded = encode_manifest_slot(b);
        REQUIRE(encoded.ok());
        write_u64(encoded.value(), 48, b.generation.checkpoint_generation + 1);
        constexpr std::array<char, 9> escaped{
            '.', '.', '\\', 'x', '.', 'v', 'h', 'd', 'x'};
        for (std::size_t index = 0; index < escaped.size(); ++index) {
            encoded.value()[88 + index] =
                static_cast<std::byte>(escaped[index]);
        }
        refresh_manifest_crc(encoded.value());
        auto manifest = open_windows_persistence_file(
            fixture.manifest_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(manifest.ok());
        REQUIRE(manifest.value()->write_exact(
            kManifestSlotBytes, encoded.value()).ok());
        REQUIRE(manifest.value()->close().ok());
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::BasePath});
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Corrupt);
    }
}

TEST_CASE(recovery_reports_crc_valid_unsupported_journal_header) {
    RecoveryFixture fixture;
    const auto unsupported_path =
        fixture.sandbox.root() / L"journal-unsupported.rdj";
    std::filesystem::copy_file(fixture.journal_path, unsupported_path);
    auto file = open_windows_persistence_file(
        unsupported_path, FileOpenMode::ReadWriteExisting);
    REQUIRE(file.ok());
    JournalHeaderBytes header{};
    REQUIRE(file.value()->read_exact(0, header).ok());
    write_u32(header, 8, kM3FormatMajor + 1);
    refresh_journal_header_crc(header);
    REQUIRE(file.value()->write_exact(0, header).ok());
    REQUIRE(file.value()->close().ok());
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    newer.journal_relative_name = "journal-unsupported.rdj";
    fixture.publish(fixture.valid_a, newer);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0, true);
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::JournalHeader});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::Unsupported);
}

TEST_CASE(recovery_reports_unsupported_vhdx_at_profile_boundary) {
    RecoveryFixture fixture;
    const auto unsupported_path =
        fixture.sandbox.root() / L"base-unsupported.vhdx";
    std::filesystem::copy_file(fixture.base_path, unsupported_path);
    auto file = open_windows_persistence_file(
        unsupported_path, FileOpenMode::ReadWriteExisting);
    REQUIRE(file.ok());
    std::array<std::byte, 4096> header{};
    REQUIRE(file.value()->read_exact(128 * 1024, header).ok());
    header[48] = std::byte{1};
    refresh_vhdx_header_crc(header);
    REQUIRE(file.value()->write_exact(128 * 1024, header).ok());
    REQUIRE(file.value()->close().ok());
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    newer.base_relative_name = "base-unsupported.vhdx";
    fixture.publish(fixture.valid_a, newer);

    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());

    require_selected(outcome, 0, true);
    REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
               std::optional{ManifestCandidateCheck::BaseProfile});
    REQUIRE_EQ(outcome.diagnostics[1].status,
               ManifestCandidateCheckStatus::Unsupported);
}

TEST_CASE(recovery_preserves_vhdx_identity_length_profile_layout_order) {
    {
        RecoveryFixture fixture;
        const auto short_path = fixture.sandbox.root() / L"short.vhdx";
        std::filesystem::copy_file(fixture.base_path, short_path);
        auto short_file = open_windows_persistence_file(
            short_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(short_file.ok());
        REQUIRE(short_file.value()->truncate(2 * kMiB).ok());
        REQUIRE(short_file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.base_relative_name = "short.vhdx";
        fixture.publish(fixture.valid_a, newer);

        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{
                       ManifestCandidateCheck::BaseExpectedLength});
        REQUIRE_EQ(outcome.diagnostics[1].base_structural_read_calls,
                   std::uint64_t{3});
        REQUIRE_EQ(outcome.diagnostics[1].base_structural_read_bytes,
                   std::uint64_t{8200});
    }
    {
        RecoveryFixture fixture;
        const auto corrupt_path = fixture.sandbox.root() / L"layout.vhdx";
        std::filesystem::copy_file(fixture.base_path, corrupt_path);
        auto corrupt_file = open_windows_persistence_file(
            corrupt_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(corrupt_file.ok());
        std::array<std::byte, 1> byte{};
        REQUIRE(corrupt_file.value()->read_exact(2 * kMiB, byte).ok());
        byte[0] ^= std::byte{1};
        REQUIRE(corrupt_file.value()->write_exact(2 * kMiB, byte).ok());
        REQUIRE(corrupt_file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.base_relative_name = "layout.vhdx";
        fixture.publish(fixture.valid_a, newer);

        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::BaseLayout});
    }
}

TEST_CASE(recovery_detects_journal_identity_header_root_and_digest_mutations) {
    {
        RecoveryFixture fixture;
        const auto path = fixture.sandbox.root() / L"journal-identity.rdj";
        std::filesystem::copy_file(fixture.journal_path, path);
        auto file = open_windows_persistence_file(
            path, FileOpenMode::ReadWriteExisting);
        REQUIRE(file.ok());
        JournalHeaderBytes header{};
        REQUIRE(file.value()->read_exact(0, header).ok());
        header[40] ^= std::byte{1};
        refresh_journal_header_crc(header);
        REQUIRE(file.value()->write_exact(0, header).ok());
        REQUIRE(file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.journal_relative_name = "journal-identity.rdj";
        fixture.publish(fixture.valid_a, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::JournalIdentity});
    }
    {
        RecoveryFixture fixture;
        const auto path = fixture.sandbox.root() / L"journal-header.rdj";
        std::filesystem::copy_file(fixture.journal_path, path);
        auto file = open_windows_persistence_file(
            path, FileOpenMode::ReadWriteExisting);
        REQUIRE(file.ok());
        JournalHeaderBytes header{};
        REQUIRE(file.value()->read_exact(0, header).ok());
        header[24] ^= std::byte{1};
        refresh_journal_header_crc(header);
        REQUIRE(file.value()->write_exact(0, header).ok());
        REQUIRE(file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.generation.checkpoint_generation += 1;
        newer.journal_relative_name = "journal-header.rdj";
        fixture.publish(fixture.valid_a, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        require_selected(outcome, 0, true);
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::JournalHeader});
    }
    {
        RecoveryFixture fixture;
        const auto path = fixture.sandbox.root() / L"journal-root.rdj";
        std::filesystem::copy_file(fixture.journal_path, path);
        auto file = open_windows_persistence_file(
            path, FileOpenMode::ReadWriteExisting);
        REQUIRE(file.ok());
        std::array<std::byte, 1> byte{};
        const auto offset = fixture.checkpoint.root.offset + 256;
        REQUIRE(file.value()->read_exact(offset, byte).ok());
        byte[0] ^= std::byte{1};
        REQUIRE(file.value()->write_exact(offset, byte).ok());
        REQUIRE(file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.journal_relative_name = "journal-root.rdj";
        fixture.publish(std::nullopt, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::JournalRoot});
    }
    {
        RecoveryFixture fixture;
        const auto path = fixture.sandbox.root() / L"journal-digest.rdj";
        std::filesystem::copy_file(fixture.journal_path, path);
        auto file = open_windows_persistence_file(
            path, FileOpenMode::ReadWriteExisting);
        REQUIRE(file.ok());
        std::array<std::byte, 1> byte{};
        const auto offset = kJournalAlignment + kJournalAlignment + 17;
        REQUIRE(file.value()->read_exact(offset, byte).ok());
        byte[0] ^= std::byte{1};
        REQUIRE(file.value()->write_exact(offset, byte).ok());
        REQUIRE(file.value()->close().ok());
        auto newer = fixture.make_slot(1);
        newer.journal_relative_name = "journal-digest.rdj";
        fixture.publish(std::nullopt, newer);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::JournalDigest});
    }
}

TEST_CASE(recovery_checks_manifest_commit_crc_and_container_length) {
    {
        RecoveryFixture fixture;
        auto b = fixture.make_slot(1);
        b.generation.journal.commit.crc32c ^= 1U;
        fixture.publish(std::nullopt, b);
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        REQUIRE_EQ(outcome.diagnostics[1].first_failed_check,
                   std::optional{ManifestCandidateCheck::JournalCommit});
        REQUIRE_EQ(outcome.diagnostics[1].status,
                   ManifestCandidateCheckStatus::Corrupt);
    }
    {
        RecoveryFixture fixture;
        auto manifest = open_windows_persistence_file(
            fixture.manifest_path, FileOpenMode::ReadWriteExisting);
        REQUIRE(manifest.ok());
        REQUIRE(manifest.value()->truncate(kManifestSlotBytes).ok());
        REQUIRE(manifest.value()->close().ok());
        auto outcome = recover_complete_image(
            OperationContext{}, fixture.sandbox.root(), "disk.rdm",
            recovery_options());
        REQUIRE(!outcome.ok());
        for (const auto& diagnostic : outcome.diagnostics) {
            REQUIRE_EQ(diagnostic.first_failed_check,
                       std::optional{ManifestCandidateCheck::SlotDecode});
            REQUIRE_EQ(diagnostic.status,
                       ManifestCandidateCheckStatus::Corrupt);
        }
    }
}

TEST_CASE(recovery_diagnostics_are_redacted_and_invalid_limits_fail_before_io) {
    RecoveryFixture fixture;
    auto newer = fixture.make_slot(1);
    newer.generation.checkpoint_generation += 1;
    newer.base_relative_name = "missing.vhdx";
    fixture.publish(fixture.valid_a, newer);
    auto outcome = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm",
        recovery_options());
    require_selected(outcome, 0, true);
    for (const auto& diagnostic : outcome.diagnostics) {
        REQUIRE(diagnostic.safe_context.find("myrd-task26") ==
                std::string::npos);
        REQUIRE(diagnostic.safe_context.find("\\") == std::string::npos);
        REQUIRE(diagnostic.safe_context.find(":") == std::string::npos);
    }

    auto invalid = recovery_options();
    invalid.limits.max_journal_records = 0;
    auto rejected = recover_complete_image(
        OperationContext{}, fixture.sandbox.root(), "disk.rdm", invalid);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error->code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::InvalidFormat));
    REQUIRE_EQ(rejected.diagnostics[0].status,
               ManifestCandidateCheckStatus::NotRun);
}

}  // namespace
}  // namespace myrd::persistence
