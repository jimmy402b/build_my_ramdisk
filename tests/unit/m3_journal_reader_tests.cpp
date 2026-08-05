#include "test_framework.h"

#include "src/persistence/journal_reader.h"
#include "src/persistence/journal_writer.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

JournalHeader reader_header() {
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] =
            static_cast<std::byte>(0x30U + index);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x50U + index);
    }
    header.epoch = 1;
    header.geometry = {64ull * 1024 * 1024, 512, 4096};
    header.geometry_digest = compute_geometry_digest(header.geometry).value();
    VhdxFileIdentity base{};
    base.file_write_guid[0] = std::byte{0x71};
    base.data_write_guid[0] = std::byte{0x81};
    header.base_identity_digest = compute_base_identity_digest(base).value();
    header.predecessor_digest = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest).value();
    header.feature_flags = 1;
    return header;
}

WindowsFileIdentity reader_file_identity() {
    WindowsFileIdentity identity{};
    identity.volume_serial_number = 41;
    identity.file_id[0] = std::byte{0x61};
    identity.attributes = 1;
    identity.hard_link_count = 1;
    return identity;
}

JournalWriterOptions reader_writer_options() {
    JournalWriterOptions options{};
    options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    options.max_checkpoint_records = 8192;
    options.max_checkpoint_bytes = 64ull * 1024 * 1024;
    return options;
}

struct ReaderFixture final {
    std::shared_ptr<IPersistenceFile> file{};
    std::shared_ptr<test::FaultFileState> state{};
    WindowsFileIdentity file_identity{};
    JournalHeader header{};
    PreparedCheckpoint prepared{};
    std::shared_ptr<std::array<std::byte, kM2BlockSize>> payload{};
};

ReaderFixture make_reader_fixture(
    std::vector<test::FaultFileRule> rules = {},
    test::FaultFileObserver observer = {},
    const bool final_partial_block = false) {
    ReaderFixture fixture{};
    fixture.payload =
        std::make_shared<std::array<std::byte, kM2BlockSize>>();
    for (std::size_t index = 0; index < fixture.payload->size(); ++index) {
        (*fixture.payload)[index] = final_partial_block && index >= 512
            ? std::byte{0}
            : static_cast<std::byte>((index * 17U + 3U) & 0xFFU);
    }
    auto fault = test::make_fault_file(
        kJournalAlignment, std::move(rules), std::move(observer));
    fixture.file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    fixture.state = std::move(fault.state);
    fixture.file_identity = reader_file_identity();
    fixture.header = reader_header();
    if (final_partial_block) {
        fixture.header.geometry.virtual_bytes += 512;
        fixture.header.geometry_digest = compute_geometry_digest(
            fixture.header.geometry).value();
        fixture.header.predecessor_digest = compute_genesis_digest(
            fixture.header.image_uuid, fixture.header.geometry_digest,
            fixture.header.base_identity_digest).value();
    }
    const auto header_bytes = encode_journal_header(fixture.header);
    REQUIRE(header_bytes.ok());
    REQUIRE(fixture.file->write_exact(0, header_bytes.value()).ok());

    std::vector<JournalCheckpointBlockInput> blocks{
        {JournalRecordType::Data, 2, 10, 0, 7, *fixture.payload},
        {JournalRecordType::Zero, 3, 11, 0, 7, {}},
        {JournalRecordType::Discard, 4, 12, 0, 7, {}},
    };
    if (final_partial_block) {
        blocks.push_back(
            {JournalRecordType::Data, 1024, 13, 0, 7,
             *fixture.payload});
    }
    JournalCheckpointRequest request{};
    request.file = fixture.file;
    request.file_identity = fixture.file_identity;
    request.header = fixture.header;
    request.previous_digest = fixture.header.predecessor_digest;
    request.committed_length = kJournalAlignment;
    request.operation_id = 91;
    request.blocks = blocks;
    auto writer = JournalWriter::create(reader_writer_options());
    REQUIRE(writer.ok());
    auto prepared = writer.value()->prepare(OperationContext{}, request);
    REQUIRE(prepared.ok());
    fixture.prepared = prepared.move_value();
    return fixture;
}

JournalReaderOptions reader_options(const ReaderFixture& fixture) {
    JournalReaderOptions options{};
    options.view.file = fixture.file;
    options.view.file_identity = fixture.file_identity;
    options.view.journal_identity = fixture.header.journal_identity;
    options.view.epoch = fixture.header.epoch;
    options.view.root_generation = fixture.prepared.generation;
    options.view.committed_length = fixture.prepared.committed_length;
    options.view.root = fixture.prepared.root;
    options.geometry = fixture.header.geometry;
    options.index_cache = {64 * kIndexNodeBytes, 64};
    return options;
}

std::vector<std::byte> replacement_data_record(
    const ReaderFixture& fixture,
    const std::span<const std::byte> payload,
    const std::uint64_t block_index = 2,
    const CheckpointGeneration generation = 1) {
    BlockJournalRecord record{};
    record.common = {
        JournalRecordType::Data, generation, 91, 0, 0};
    record.block_index = block_index;
    record.snapshot_version = 10;
    record.source_generation = 0;
    record.dirty_epoch = 7;
    auto encoded = encode_block_record(
        record, payload, fixture.header.geometry);
    REQUIRE(encoded.ok());
    return encoded.move_value();
}

void require_backing_data_corrupt(
    const Result<std::optional<BlockLoadResult>>& result) {
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::BackingDataCorrupt));
}

TEST_CASE(journal_reader_create_reports_its_own_operation) {
    auto fixture = make_reader_fixture();
    auto options = reader_options(fixture);
    options.geometry.virtual_bytes = 0;
    const auto result = JournalReader::create(std::move(options));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().operation,
               std::wstring(L"JournalReader::create"));
}

TEST_CASE(journal_reader_resolves_data_zero_discard_and_absent) {
    auto fixture = make_reader_fixture();
    auto reader = JournalReader::create(reader_options(fixture));
    REQUIRE(reader.ok());

    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0xCC});
    auto data = reader.value()->read_block(
        OperationContext{}, 2, destination);
    REQUIRE(data.ok());
    REQUIRE(data.value().has_value());
    REQUIRE_EQ(data.value()->kind, BlockLoadKind::Data);
    REQUIRE_EQ(destination, *fixture.payload);

    for (const auto block_index : {3ull, 4ull}) {
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        auto all_zero = reader.value()->read_block(
            OperationContext{}, block_index, destination);
        REQUIRE(all_zero.ok());
        REQUIRE(all_zero.value().has_value());
        REQUIRE_EQ(all_zero.value()->kind, BlockLoadKind::AllZero);
        REQUIRE_EQ(destination, before);
    }

    destination.fill(std::byte{0xCC});
    const auto before = destination;
    auto absent = reader.value()->read_block(
        OperationContext{}, 5, destination);
    REQUIRE(absent.ok());
    REQUIRE(!absent.value().has_value());
    REQUIRE_EQ(destination, before);
}

TEST_CASE(journal_reader_rejects_valid_record_with_wrong_payload_descriptor) {
    auto fixture = make_reader_fixture();
    auto changed = *fixture.payload;
    changed[123] ^= std::byte{0x5A};
    const auto replacement = replacement_data_record(fixture, changed);
    REQUIRE(fixture.file->write_exact(
        kJournalAlignment, replacement).ok());
    auto reader = JournalReader::create(reader_options(fixture));
    REQUIRE(reader.ok());
    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0xCC});
    const auto before = destination;
    const auto result = reader.value()->read_block(
        OperationContext{}, 2, destination);
    require_backing_data_corrupt(result);
    REQUIRE_EQ(destination, before);
}

TEST_CASE(journal_reader_validates_zero_and_discard_records_before_all_zero) {
    constexpr std::uint64_t offsets[]{0x12000, 0x13000};
    constexpr std::uint64_t blocks[]{3, 4};
    for (std::size_t index = 0; index < 2; ++index) {
        auto fixture = make_reader_fixture();
        std::array<std::byte, kFixedRecordBytes> bytes{};
        REQUIRE(fixture.file->read_exact(offsets[index], bytes).ok());
        bytes[97] ^= std::byte{0x01};
        REQUIRE(fixture.file->write_exact(offsets[index], bytes).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, blocks[index], destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_rejects_record_type_block_and_generation_mismatch) {
    {
        auto fixture = make_reader_fixture();
        BlockJournalRecord zero{};
        zero.common = {JournalRecordType::Zero, 1, 91, 0, 0};
        zero.block_index = 2;
        zero.snapshot_version = 10;
        zero.dirty_epoch = 7;
        const auto encoded = encode_block_record(
            zero, {}, fixture.header.geometry);
        REQUIRE(encoded.ok());
        REQUIRE(fixture.file->write_exact(
            kJournalAlignment, encoded.value()).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture();
        const auto encoded = replacement_data_record(
            fixture, *fixture.payload, 6, 1);
        REQUIRE(fixture.file->write_exact(
            kJournalAlignment, encoded).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture();
        const auto encoded = replacement_data_record(
            fixture, *fixture.payload, 2, 2);
        REQUIRE(fixture.file->write_exact(
            kJournalAlignment, encoded).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_rejects_crc_corruption_and_truncated_record) {
    {
        auto fixture = make_reader_fixture();
        std::vector<std::byte> bytes(kDataRecordBytes);
        REQUIRE(fixture.file->read_exact(kJournalAlignment, bytes).ok());
        bytes[kJournalAlignment + 77] ^= std::byte{0x01};
        REQUIRE(fixture.file->write_exact(kJournalAlignment, bytes).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture();
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        REQUIRE(reader.value()->read_block(
            OperationContext{}, 3, destination).ok());
        // The first lookup cached the immutable root. Truncating into the
        // DATA record now leaves a valid cached descriptor but no payload.
        REQUIRE(fixture.file->truncate(2 * kJournalAlignment).ok());
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_classifies_index_damage_as_journal_corrupt) {
    {
        auto fixture = make_reader_fixture();
        std::array<std::byte, kIndexNodeBytes> root{};
        REQUIRE(fixture.file->read_exact(
            fixture.prepared.root.offset, root).ok());
        root[211] ^= std::byte{0x01};
        REQUIRE(fixture.file->write_exact(
            fixture.prepared.root.offset, root).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::JournalCorrupt));
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture();
        REQUIRE(fixture.file->truncate(
            fixture.prepared.root.offset).ok());
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::JournalCorrupt));
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture({
            {test::FaultFilePhase::Read, 8, 0x14010,
             test::FaultFileAction::NativeError}});
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::IoError));
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_enforces_geometry_destination_and_final_block) {
    {
        auto fixture = make_reader_fixture();
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto reads_before = fixture.state->calls(
            test::FaultFilePhase::Read);
        auto outside = reader.value()->read_block(
            OperationContext{}, 1024, destination);
        REQUIRE(!outside.ok());
        REQUIRE_EQ(outside.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::InvalidFormat));
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Read),
                   reads_before);
        REQUIRE_EQ(destination, before);

        auto short_destination = reader.value()->read_block(
            OperationContext{}, 2,
            std::span<std::byte>(destination).first(512));
        REQUIRE(!short_destination.ok());
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Read),
                   reads_before);
    }
    {
        auto fixture = make_reader_fixture({}, {}, true);
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        auto result = reader.value()->read_block(
            OperationContext{}, 1024, destination);
        REQUIRE(result.ok());
        REQUIRE(result.value().has_value());
        REQUIRE_EQ(result.value()->kind, BlockLoadKind::Data);
        REQUIRE_EQ(destination, *fixture.payload);
        REQUIRE(std::all_of(destination.begin() + 512,
                            destination.end(),
                            [](const std::byte value) {
                                return value == std::byte{0};
                            }));
    }
}

TEST_CASE(journal_reader_exact_io_handles_short_reads_and_maps_io_damage) {
    constexpr std::uint64_t record_offsets[]{0x1010, 0x12010, 0x13010};
    constexpr std::uint64_t block_indices[]{2, 3, 4};
    for (std::size_t index = 0; index < 3; ++index) {
        auto fixture = make_reader_fixture({
            {test::FaultFilePhase::Read, 8, record_offsets[index],
             test::FaultFileAction::ShortRead}});
        const auto reads_before = fixture.state->calls(
            test::FaultFilePhase::Read);
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        const auto result = reader.value()->read_block(
            OperationContext{}, block_indices[index], destination);
        REQUIRE(result.ok());
        REQUIRE(result.value().has_value());
        const auto expected_kind = index == 0
            ? BlockLoadKind::Data : BlockLoadKind::AllZero;
        REQUIRE_EQ(result.value()->kind, expected_kind);
        if (index == 0) REQUIRE_EQ(destination, *fixture.payload);
        REQUIRE(fixture.state->calls(test::FaultFilePhase::Read) >
                reads_before + 2);
    }

    {
        auto fixture = make_reader_fixture({
            {test::FaultFilePhase::Read, 8, 0x1010,
             test::FaultFileAction::ZeroProgress}});
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        require_backing_data_corrupt(result);
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture({
            {test::FaultFilePhase::Read, 8, 0x1010,
             test::FaultFileAction::NativeError}});
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::IoError));
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_propagates_cancel_and_deadline_without_data) {
    {
        std::stop_source stop;
        auto fixture = make_reader_fixture(
            {}, [&](const test::FaultFilePhase phase,
                    const std::uint64_t call) {
                if (phase == test::FaultFilePhase::Read && call == 8) {
                    stop.request_stop();
                }
            });
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{stop.get_token()}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::cancelled));
        REQUIRE_EQ(destination, before);
    }
    {
        std::stop_source stop;
        auto fixture = make_reader_fixture();
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, 512> short_destination{};
        stop.request_stop();
        const auto result = reader.value()->read_block(
            OperationContext{stop.get_token()}, 2, short_destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::cancelled));
    }
    {
        std::stop_source stop;
        auto fixture = make_reader_fixture(
            {}, [&](const test::FaultFilePhase phase,
                    const std::uint64_t call) {
                if (phase == test::FaultFilePhase::Read && call == 10) {
                    stop.request_stop();
                }
            });
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        REQUIRE(reader.value()->read_block(
            OperationContext{}, 3, destination).ok());
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        const auto result = reader.value()->read_block(
            OperationContext{stop.get_token()}, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::cancelled));
        REQUIRE_EQ(destination, before);
    }
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        auto fixture = make_reader_fixture(
            {}, [&](const test::FaultFilePhase phase,
                    const std::uint64_t call) {
                if (phase == test::FaultFilePhase::Read && call == 10) {
                    std::this_thread::sleep_until(
                        deadline + std::chrono::milliseconds(2));
                }
            });
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        REQUIRE(reader.value()->read_block(
            OperationContext{}, 3, destination).ok());
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        OperationContext context{};
        context.deadline = deadline;
        const auto result = reader.value()->read_block(
            context, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::timed_out));
        REQUIRE_EQ(destination, before);
    }
    {
        auto fixture = make_reader_fixture();
        auto reader = JournalReader::create(reader_options(fixture));
        REQUIRE(reader.ok());
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xCC});
        const auto before = destination;
        OperationContext expired{};
        expired.deadline = std::chrono::steady_clock::now();
        const auto result = reader.value()->read_block(
            expired, 2, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::timed_out));
        REQUIRE_EQ(destination, before);
    }
}

TEST_CASE(journal_reader_is_concurrent_and_owns_its_immutable_file_view) {
    auto fixture = make_reader_fixture();
    const auto expected = *fixture.payload;
    auto reader_result = JournalReader::create(reader_options(fixture));
    REQUIRE(reader_result.ok());
    auto reader = std::shared_ptr<JournalReader>(
        std::move(reader_result.move_value()));
    fixture.file.reset();

    std::vector<std::future<bool>> futures;
    for (std::size_t worker = 0; worker < 8; ++worker) {
        futures.push_back(std::async(std::launch::async, [reader, expected] {
            for (std::size_t iteration = 0; iteration < 100; ++iteration) {
                std::array<std::byte, kM2BlockSize> destination{};
                const auto data = reader->read_block(
                    OperationContext{}, 2, destination);
                if (!data.ok() || !data.value().has_value() ||
                    data.value()->kind != BlockLoadKind::Data ||
                    destination != expected) {
                    return false;
                }
                const auto absent = reader->read_block(
                    OperationContext{}, 5, destination);
                if (!absent.ok() || absent.value().has_value()) return false;
            }
            return true;
        }));
    }
    for (auto& future : futures) REQUIRE(future.get());
    REQUIRE(!fixture.state->closed());
    reader.reset();
    REQUIRE(fixture.state->closed());
}

TEST_CASE(journal_reader_old_view_ignores_a_later_committed_generation) {
    auto fixture = make_reader_fixture();
    auto old_reader = JournalReader::create(reader_options(fixture));
    REQUIRE(old_reader.ok());

    auto newer_payload = *fixture.payload;
    newer_payload[0] ^= std::byte{0x7F};
    const std::array<JournalCheckpointBlockInput, 2> blocks{{
        {JournalRecordType::Data, 2, 20, 0, 8, newer_payload},
        {JournalRecordType::Data, 6, 21, 0, 8, newer_payload},
    }};
    JournalCheckpointRequest request{};
    request.file = fixture.file;
    request.file_identity = fixture.file_identity;
    request.header = fixture.header;
    request.previous_generation = fixture.prepared.generation;
    request.previous_digest = fixture.prepared.chain_digest;
    request.committed_length = fixture.prepared.committed_length;
    request.root = fixture.prepared.root;
    request.operation_id = 92;
    request.blocks = blocks;
    auto writer = JournalWriter::create(reader_writer_options());
    REQUIRE(writer.ok());
    auto second = writer.value()->prepare(OperationContext{}, request);
    REQUIRE(second.ok());

    auto newer_fixture = fixture;
    newer_fixture.prepared = second.value();
    auto new_reader = JournalReader::create(reader_options(newer_fixture));
    REQUIRE(new_reader.ok());
    std::array<std::byte, kM2BlockSize> old_destination{};
    std::array<std::byte, kM2BlockSize> new_destination{};
    REQUIRE(old_reader.value()->read_block(
        OperationContext{}, 2, old_destination).ok());
    REQUIRE(new_reader.value()->read_block(
        OperationContext{}, 2, new_destination).ok());
    REQUIRE_EQ(old_destination, *fixture.payload);
    REQUIRE_EQ(new_destination, newer_payload);

    old_destination.fill(std::byte{0xCC});
    const auto old_absent = old_reader.value()->read_block(
        OperationContext{}, 6, old_destination);
    REQUIRE(old_absent.ok());
    REQUIRE(!old_absent.value().has_value());
    const auto new_data = new_reader.value()->read_block(
        OperationContext{}, 6, new_destination);
    REQUIRE(new_data.ok());
    REQUIRE(new_data.value().has_value());
    REQUIRE_EQ(new_data.value()->kind, BlockLoadKind::Data);
    REQUIRE_EQ(new_destination, newer_payload);
}

}  // namespace
}  // namespace myrd::persistence
