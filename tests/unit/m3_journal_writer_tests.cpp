#include "test_framework.h"

#include "src/persistence/journal_writer.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

JournalHeader make_header() {
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] =
            static_cast<std::byte>(0x10U + index);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x20U + index);
    }
    header.epoch = 1;
    header.geometry = {64ull * 1024 * 1024, 512, 4096};
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

struct WriterFixture final {
    JournalCheckpointRequest request{};
    std::shared_ptr<test::FaultFileState> state{};
    std::shared_ptr<std::array<std::byte, kM2BlockSize>> payload{};
    std::vector<JournalCheckpointBlockInput> blocks{};
};

WriterFixture make_fixture(
    std::vector<test::FaultFileRule> rules = {},
    test::FaultFileObserver observer = {}) {
    WriterFixture fixture{};
    fixture.payload =
        std::make_shared<std::array<std::byte, kM2BlockSize>>();
    fixture.payload->fill(std::byte{0x5A});
    fixture.blocks = {
        {JournalRecordType::Data, 2, 10, 0, 7, *fixture.payload},
        {JournalRecordType::Zero, 3, 11, 0, 7, {}},
        {JournalRecordType::Discard, 4, 12, 0, 7, {}}};
    auto fault = test::make_fault_file(
        kJournalAlignment, std::move(rules), std::move(observer));
    const auto header = make_header();
    const auto bytes = encode_journal_header(header);
    REQUIRE(bytes.ok());
    REQUIRE(fault.file->write_exact(0, bytes.value()).ok());
    fixture.request.file =
        std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    fixture.request.file_identity = make_file_identity();
    fixture.request.header = header;
    fixture.request.previous_generation = 0;
    fixture.request.previous_digest = header.predecessor_digest;
    fixture.request.committed_length = kJournalAlignment;
    fixture.request.operation_id = 71;
    fixture.request.blocks = fixture.blocks;
    fixture.state = std::move(fault.state);
    return fixture;
}

void configure_split_index_checkpoint(WriterFixture& fixture) {
    fixture.blocks.clear();
    fixture.blocks.reserve(kJournalIndexMaxEntries + 1);
    for (std::uint64_t key = 0; key <= kJournalIndexMaxEntries; ++key) {
        fixture.blocks.push_back(
            {JournalRecordType::Zero, key, key + 1, 0, 8, {}});
    }
    fixture.request.blocks = fixture.blocks;
}

std::vector<std::byte> read_committed_prefix(
    const WriterFixture& fixture) {
    std::vector<std::byte> bytes(fixture.request.committed_length);
    REQUIRE(fixture.request.file->read_exact(0, bytes).ok());
    return bytes;
}

void require_committed_prefix(
    const WriterFixture& fixture,
    const std::vector<std::byte>& expected) {
    std::vector<std::byte> actual(expected.size());
    REQUIRE(fixture.request.file->read_exact(0, actual).ok());
    REQUIRE_EQ(actual, expected);
}

enum class WriterTraceKind : std::uint8_t {
    Io,
    Phase,
};

struct WriterTraceEvent final {
    WriterTraceKind kind{};
    std::uint64_t code{};
    std::uint64_t ordinal{};

    bool operator==(const WriterTraceEvent&) const noexcept = default;
};

WriterTraceEvent io_trace(
    const test::FaultFilePhase phase,
    const std::uint64_t call) {
    return {WriterTraceKind::Io, static_cast<std::uint64_t>(phase), call};
}

WriterTraceEvent phase_trace(
    const JournalWriterPhase phase,
    const std::uint64_t ordinal) {
    return {WriterTraceKind::Phase, static_cast<std::uint64_t>(phase),
            ordinal};
}

JournalWriterOptions writer_options(
    std::vector<JournalWriterPhaseEvent>* phases = nullptr) {
    JournalWriterOptions options{};
    options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    options.max_checkpoint_records = 8192;
    options.max_checkpoint_bytes = 64ull * 1024 * 1024;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (phases != nullptr) {
        options.phase_hook = [phases](const JournalWriterPhaseEvent& event) {
            phases->push_back(event);
            return Result<void>::success();
        };
    }
#else
    (void)phases;
#endif
    return options;
}

TEST_CASE(journal_writer_prepares_flushes_and_reads_back_checkpoint) {
    auto fixture = make_fixture();
    std::vector<JournalWriterPhaseEvent> phases;
    auto writer = JournalWriter::create(writer_options(&phases));
    REQUIRE(writer.ok());
    const auto old_prefix_length = fixture.request.committed_length;
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    auto prepared = writer.value()->prepare(OperationContext{}, fixture.request);
    REQUIRE(prepared.ok());
    REQUIRE_EQ(prepared.value().generation, 1ull);
    REQUIRE(prepared.value().root.offset >= old_prefix_length);
    REQUIRE_EQ(prepared.value().data_count, 1ull);
    REQUIRE_EQ(prepared.value().zero_count, 1ull);
    REQUIRE_EQ(prepared.value().discard_count, 1ull);
    REQUIRE_EQ(prepared.value().index_node_count, 1ull);
    REQUIRE_EQ(prepared.value().ack_stream.record_count, 3ull);
    REQUIRE_EQ(prepared.value().ack_stream.journal_identity,
               fixture.request.header.journal_identity);
    REQUIRE_EQ(prepared.value().ack_stream.journal_epoch,
               fixture.request.header.epoch);
    REQUIRE_EQ(prepared.value().ack_stream.begin_offset, old_prefix_length);
    REQUIRE(prepared.value().ack_stream.end_offset <=
            prepared.value().root.offset);
    REQUIRE_EQ(prepared.value().committed_length,
               prepared.value().commit.offset + prepared.value().commit.length);
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 1ull);
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write) - writes_before,
               5ull);
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Read), 7ull);
    REQUIRE_EQ(phases.back().phase, JournalWriterPhase::Prepared);
    const std::vector<JournalWriterPhase> expected{
        JournalWriterPhase::BeforeBlockWrite,
        JournalWriterPhase::AfterBlockWrite,
        JournalWriterPhase::BeforeBlockWrite,
        JournalWriterPhase::AfterBlockWrite,
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
        JournalWriterPhase::BeforeReadback,
        JournalWriterPhase::AfterReadback,
        JournalWriterPhase::BeforeReadback,
        JournalWriterPhase::AfterReadback,
        JournalWriterPhase::BeforeReadback,
        JournalWriterPhase::AfterReadback,
        JournalWriterPhase::BeforeReadback,
        JournalWriterPhase::AfterReadback,
        JournalWriterPhase::BeforeIndexCrosscheck,
        JournalWriterPhase::AfterIndexCrosscheck,
        JournalWriterPhase::BeforeIndexCrosscheck,
        JournalWriterPhase::AfterIndexCrosscheck,
        JournalWriterPhase::BeforeIndexCrosscheck,
        JournalWriterPhase::AfterIndexCrosscheck,
        JournalWriterPhase::Prepared};
    REQUIRE_EQ(phases.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(phases[index].phase, expected[index]);
    }

    std::array<std::byte, kJournalAlignment> header_bytes{};
    REQUIRE(fixture.request.file->read_exact(0, header_bytes).ok());
    REQUIRE_EQ(header_bytes,
               encode_journal_header(fixture.request.header).value());
}

TEST_CASE(journal_writer_real_io_is_inside_the_named_durability_phases) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::vector<WriterTraceEvent> trace;
    auto fixture = make_fixture({}, [&](const test::FaultFilePhase phase,
                                        const std::uint64_t call) {
        trace.push_back(io_trace(phase, call));
    });
    // make_fixture writes the initial committed header through the same
    // observer. The checkpoint trace starts after that fixture setup write.
    trace.clear();
    auto options = writer_options();
    options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
        trace.push_back(phase_trace(event.phase, event.ordinal));
        return Result<void>::success();
    };
    auto writer = JournalWriter::create(std::move(options));
    REQUIRE(writer.ok());
    REQUIRE(writer.value()->prepare(OperationContext{}, fixture.request).ok());

    const std::vector<WriterTraceEvent> expected{
        io_trace(test::FaultFilePhase::Read, 1),
        io_trace(test::FaultFilePhase::Length, 1),
        phase_trace(JournalWriterPhase::BeforeBlockWrite, 0),
        io_trace(test::FaultFilePhase::Write, 2),
        phase_trace(JournalWriterPhase::AfterBlockWrite, 0),
        phase_trace(JournalWriterPhase::BeforeBlockWrite, 1),
        io_trace(test::FaultFilePhase::Write, 3),
        phase_trace(JournalWriterPhase::AfterBlockWrite, 1),
        phase_trace(JournalWriterPhase::BeforeBlockWrite, 2),
        io_trace(test::FaultFilePhase::Write, 4),
        phase_trace(JournalWriterPhase::AfterBlockWrite, 2),
        phase_trace(JournalWriterPhase::BeforeIndexWrite, 0),
        io_trace(test::FaultFilePhase::Write, 5),
        phase_trace(JournalWriterPhase::AfterIndexWrite, 0),
        phase_trace(JournalWriterPhase::BeforeCommitWrite, 0),
        io_trace(test::FaultFilePhase::Write, 6),
        phase_trace(JournalWriterPhase::AfterCommitWrite, 0),
        phase_trace(JournalWriterPhase::BeforeFlush, 0),
        io_trace(test::FaultFilePhase::Flush, 1),
        phase_trace(JournalWriterPhase::AfterFlush, 0),
        phase_trace(JournalWriterPhase::BeforeReadback, 0),
        io_trace(test::FaultFilePhase::Read, 2),
        phase_trace(JournalWriterPhase::AfterReadback, 0),
        phase_trace(JournalWriterPhase::BeforeReadback, 1),
        io_trace(test::FaultFilePhase::Read, 3),
        phase_trace(JournalWriterPhase::AfterReadback, 1),
        phase_trace(JournalWriterPhase::BeforeReadback, 2),
        io_trace(test::FaultFilePhase::Read, 4),
        phase_trace(JournalWriterPhase::AfterReadback, 2),
        phase_trace(JournalWriterPhase::BeforeReadback, 3),
        io_trace(test::FaultFilePhase::Read, 5),
        phase_trace(JournalWriterPhase::AfterReadback, 3),
        phase_trace(JournalWriterPhase::BeforeReadback, 4),
        io_trace(test::FaultFilePhase::Read, 6),
        phase_trace(JournalWriterPhase::AfterReadback, 4),
        phase_trace(JournalWriterPhase::BeforeIndexCrosscheck, 0),
        io_trace(test::FaultFilePhase::Read, 7),
        phase_trace(JournalWriterPhase::AfterIndexCrosscheck, 0),
        phase_trace(JournalWriterPhase::BeforeIndexCrosscheck, 1),
        phase_trace(JournalWriterPhase::AfterIndexCrosscheck, 1),
        phase_trace(JournalWriterPhase::BeforeIndexCrosscheck, 2),
        phase_trace(JournalWriterPhase::AfterIndexCrosscheck, 2),
        phase_trace(JournalWriterPhase::Prepared, 0),
    };
    REQUIRE_EQ(trace, expected);
#endif
}

TEST_CASE(journal_writer_indexes_the_frozen_encoded_record_not_later_input) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto fixture = make_fixture();
    auto options = writer_options();
    options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
        if (event.phase == JournalWriterPhase::AfterBlockWrite &&
            event.ordinal == 0) {
            // The first record is already encoded and written. Mutating this
            // test-owned source now must not change its index descriptor.
            fixture.payload->fill(std::byte{0xA5});
        }
        return Result<void>::success();
    };
    auto writer = JournalWriter::create(std::move(options));
    REQUIRE(writer.ok());
    REQUIRE(writer.value()->prepare(OperationContext{}, fixture.request).ok());
#endif
}

TEST_CASE(journal_writer_rejects_corruption_seen_only_by_index_crosscheck) {
    // Read #1 snapshots the old prefix, #2 is the writer's header,
    // #3..#7 are sequence readback, and #8 is the independent root lookup.
    // Corrupt only that final lookup.
    auto fixture = make_fixture({
        {test::FaultFilePhase::Read, 8, 0x14010,
         test::FaultFileAction::ReadbackCorruption}});
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    const auto old_prefix = read_committed_prefix(fixture);
    const auto result = writer.value()->prepare(
        OperationContext{}, fixture.request);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::VerificationFailed));
    require_committed_prefix(fixture, old_prefix);
}

TEST_CASE(journal_writer_prepares_consecutive_checkpoint_from_old_root) {
    auto fixture = make_fixture();
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    auto first = writer.value()->prepare(OperationContext{}, fixture.request);
    REQUIRE(first.ok());
    std::vector<std::byte> old_prefix(first.value().committed_length);
    REQUIRE(fixture.request.file->read_exact(0, old_prefix).ok());

    fixture.request.previous_generation = first.value().generation;
    fixture.request.previous_digest = first.value().chain_digest;
    fixture.request.committed_length = first.value().committed_length;
    fixture.request.root = first.value().root;
    fixture.request.operation_id = 72;
    fixture.blocks[0].type = JournalRecordType::Zero;
    fixture.blocks[0].payload = {};
    fixture.blocks[1].type = JournalRecordType::Data;
    fixture.blocks[1].payload = *fixture.payload;
    fixture.request.blocks = fixture.blocks;
    auto second = writer.value()->prepare(OperationContext{}, fixture.request);
    REQUIRE(second.ok());
    REQUIRE_EQ(second.value().generation, 2ull);
    REQUIRE(second.value().committed_length > first.value().committed_length);
    REQUIRE(second.value().root.offset > first.value().root.offset);
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 2ull);
    std::vector<std::byte> retained(old_prefix.size());
    REQUIRE(fixture.request.file->read_exact(0, retained).ok());
    REQUIRE_EQ(retained, old_prefix);
}

TEST_CASE(journal_writer_second_checkpoint_failure_preserves_generation_one) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto fixture = make_fixture();
    bool fail_second = false;
    auto options = writer_options();
    options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
        if (fail_second && event.phase == JournalWriterPhase::BeforeFlush) {
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"test.journal_writer", L"injected second checkpoint stop",
                0});
        }
        return Result<void>::success();
    };
    auto writer = JournalWriter::create(std::move(options));
    REQUIRE(writer.ok());
    const auto first = writer.value()->prepare(
        OperationContext{}, fixture.request);
    REQUIRE(first.ok());

    fixture.request.previous_generation = first.value().generation;
    fixture.request.previous_digest = first.value().chain_digest;
    fixture.request.committed_length = first.value().committed_length;
    fixture.request.root = first.value().root;
    fixture.request.operation_id = 72;
    const auto generation_one_prefix = read_committed_prefix(fixture);
    fail_second = true;
    REQUIRE(!writer.value()->prepare(OperationContext{}, fixture.request).ok());
    require_committed_prefix(fixture, generation_one_prefix);
#endif
}

TEST_CASE(journal_writer_write_faults_leave_only_retryable_tail) {
    for (std::uint64_t occurrence = 2; occurrence <= 6; ++occurrence) {
        auto fixture = make_fixture({
            {test::FaultFilePhase::Write, occurrence, std::nullopt,
             test::FaultFileAction::DiskFull}});
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        const auto old_prefix = read_committed_prefix(fixture);
        const auto old_committed = fixture.request.committed_length;
        auto failed = writer.value()->prepare(OperationContext{},
                                               fixture.request);
        REQUIRE(!failed.ok());
        REQUIRE_EQ(fixture.request.committed_length, old_committed);
        REQUIRE_EQ(fixture.request.root, PersistentIndexRootDescriptor{});
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 0ull);
        REQUIRE(fixture.state->logical_length() >= old_committed);
        require_committed_prefix(fixture, old_prefix);
    }

    auto torn = make_fixture({
        {test::FaultFilePhase::Write, 5, 0x14010,
         test::FaultFileAction::TornWrite}});
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    const auto torn_prefix = read_committed_prefix(torn);
    REQUIRE(!writer.value()->prepare(OperationContext{}, torn.request).ok());
    REQUIRE_EQ(torn.state->calls(test::FaultFilePhase::Flush), 0ull);
    require_committed_prefix(torn, torn_prefix);
    auto retried = writer.value()->prepare(OperationContext{}, torn.request);
    REQUIRE(retried.ok());
    REQUIRE_EQ(retried.value().generation, 1ull);
}

TEST_CASE(journal_writer_faults_each_node_in_split_checkpoint) {
    // Header is write #1, 83 block records are #2..#84, the two leaves and
    // new root are #85..#87, and the commit is #88.
    for (std::uint64_t occurrence = 85; occurrence <= 88; ++occurrence) {
        auto fixture = make_fixture({
            {test::FaultFilePhase::Write, occurrence, std::nullopt,
             test::FaultFileAction::DiskFull}});
        configure_split_index_checkpoint(fixture);
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        const auto old_prefix = read_committed_prefix(fixture);
        REQUIRE(!writer.value()->prepare(OperationContext{},
                                         fixture.request).ok());
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 0ull);
        require_committed_prefix(fixture, old_prefix);
    }

    constexpr std::uint64_t corrupt_offsets[]{
        0x54000, 0x55000, 0x56000, 0x57000};
    for (const auto offset : corrupt_offsets) {
        auto fixture = make_fixture({
            {test::FaultFilePhase::Read, 1, offset,
             test::FaultFileAction::ReadbackCorruption}});
        configure_split_index_checkpoint(fixture);
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        const auto old_prefix = read_committed_prefix(fixture);
        const auto result = writer.value()->prepare(
            OperationContext{}, fixture.request);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::VerificationFailed));
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 1ull);
        require_committed_prefix(fixture, old_prefix);
    }
}

TEST_CASE(journal_writer_flush_and_readback_faults_never_return_prepared) {
    auto flush_fixture = make_fixture({
        {test::FaultFilePhase::Flush, 1, std::nullopt,
         test::FaultFileAction::FlushError}});
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    const auto flush_prefix = read_committed_prefix(flush_fixture);
    auto flush_failed = writer.value()->prepare(OperationContext{},
                                                 flush_fixture.request);
    REQUIRE(!flush_failed.ok());
    REQUIRE_EQ(flush_fixture.state->calls(test::FaultFilePhase::Flush), 1ull);
    REQUIRE_EQ(flush_fixture.state->calls(test::FaultFilePhase::Read), 2ull);
    require_committed_prefix(flush_fixture, flush_prefix);
    REQUIRE(writer.value()->prepare(OperationContext{},
                                    flush_fixture.request).ok());

    constexpr std::uint64_t corrupt_offsets[]{
        0x1000, 0x2000, 0x12000, 0x13000, 0x14000, 0x15000};
    for (const auto corrupt_offset : corrupt_offsets) {
        auto corrupt_fixture = make_fixture({
            {test::FaultFilePhase::Read, 1, corrupt_offset,
             test::FaultFileAction::ReadbackCorruption}});
        writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        const auto old_prefix = read_committed_prefix(corrupt_fixture);
        auto corrupt = writer.value()->prepare(OperationContext{},
                                                corrupt_fixture.request);
        REQUIRE(!corrupt.ok());
        REQUIRE_EQ(corrupt.error().domain, ErrorDomain::persistence);
        REQUIRE_EQ(corrupt.error().code,
                   static_cast<std::uint32_t>(
                       PersistenceErrorCode::VerificationFailed));
        REQUIRE_EQ(corrupt_fixture.state->calls(
                       test::FaultFilePhase::Flush), 1ull);
        require_committed_prefix(corrupt_fixture, old_prefix);
    }
}

TEST_CASE(journal_writer_exact_io_handles_short_progress_and_rejects_zero) {
    constexpr std::uint64_t record_offsets[]{
        0x1010, 0x12010, 0x13010, 0x14010, 0x15010};
    for (const auto offset : record_offsets) {
        auto short_write = make_fixture({
            {test::FaultFilePhase::Write, 2, offset,
             test::FaultFileAction::ShortWrite}});
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        REQUIRE(writer.value()->prepare(OperationContext{},
                                        short_write.request).ok());
        REQUIRE(short_write.state->calls(test::FaultFilePhase::Write) > 6);
    }

    for (const auto offset : record_offsets) {
        auto short_read = make_fixture({
            {test::FaultFilePhase::Read, 1, offset,
             test::FaultFileAction::ShortRead}});
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        REQUIRE(writer.value()->prepare(OperationContext{},
                                        short_read.request).ok());
        REQUIRE(short_read.state->calls(test::FaultFilePhase::Read) > 7);
    }

    auto zero_write = make_fixture({
        {test::FaultFilePhase::Write, 2, std::nullopt,
         test::FaultFileAction::ZeroProgress}});
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    REQUIRE(!writer.value()->prepare(OperationContext{},
                                     zero_write.request).ok());
    REQUIRE_EQ(zero_write.state->calls(test::FaultFilePhase::Flush), 0ull);

    auto zero_read = make_fixture({
        {test::FaultFilePhase::Read, 2, std::nullopt,
         test::FaultFileAction::ZeroProgress}});
    writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    REQUIRE(!writer.value()->prepare(OperationContext{},
                                     zero_read.request).ok());
    REQUIRE_EQ(zero_read.state->calls(test::FaultFilePhase::Flush), 1ull);
}

TEST_CASE(journal_writer_rejects_noncanonical_or_duplicate_input_before_write) {
    for (const bool duplicate : {false, true}) {
        auto fixture = make_fixture();
        if (duplicate) {
            fixture.blocks[1].block_index = fixture.blocks[0].block_index;
        } else {
            std::swap(fixture.blocks[0], fixture.blocks[1]);
        }
        fixture.request.blocks = fixture.blocks;
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        const auto writes_before = fixture.state->calls(
            test::FaultFilePhase::Write);
        REQUIRE(!writer.value()->prepare(OperationContext{},
                                         fixture.request).ok());
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write),
                   writes_before);
        REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 0ull);
    }
}

TEST_CASE(journal_writer_enforces_finite_record_and_byte_budgets) {
    auto invalid_options = writer_options();
    invalid_options.max_checkpoint_records = 4;
    invalid_options.max_checkpoint_bytes = 3 * kJournalAlignment;
    REQUIRE(!JournalWriter::create(std::move(invalid_options)).ok());

    auto fixture = make_fixture();
    auto byte_limited = writer_options();
    byte_limited.max_checkpoint_bytes = 3 * kJournalAlignment;
    byte_limited.max_checkpoint_records = 2;
    auto writer = JournalWriter::create(std::move(byte_limited));
    REQUIRE(writer.ok());
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    REQUIRE(!writer.value()->prepare(OperationContext{}, fixture.request).ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);

    auto record_limited = writer_options();
    record_limited.max_checkpoint_records = 3;
    writer = JournalWriter::create(std::move(record_limited));
    REQUIRE(writer.ok());
    REQUIRE(!writer.value()->prepare(OperationContext{}, fixture.request).ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Flush), 0ull);
}

TEST_CASE(journal_writer_handles_final_partial_data_block_and_rejects_dirty_tail) {
    const auto configure_final_block = [](WriterFixture& fixture,
                                          const bool dirty_tail) {
        fixture.request.header.geometry.virtual_bytes =
            64ull * 1024 * 1024 + 512;
        fixture.request.header.geometry_digest = compute_geometry_digest(
            fixture.request.header.geometry).value();
        fixture.request.header.predecessor_digest = compute_genesis_digest(
            fixture.request.header.image_uuid,
            fixture.request.header.geometry_digest,
            fixture.request.header.base_identity_digest).value();
        fixture.request.previous_digest =
            fixture.request.header.predecessor_digest;
        fixture.payload->fill(std::byte{});
        std::fill_n(fixture.payload->begin(), 512, std::byte{0x6A});
        if (dirty_tail) (*fixture.payload)[512] = std::byte{1};
        fixture.blocks = {
            {JournalRecordType::Data, 1024, 20, 0, 9,
             *fixture.payload}};
        fixture.request.blocks = fixture.blocks;
        const auto encoded = encode_journal_header(fixture.request.header);
        REQUIRE(encoded.ok());
        REQUIRE(fixture.request.file->write_exact(0, encoded.value()).ok());
    };

    auto valid = make_fixture();
    configure_final_block(valid, false);
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    REQUIRE(writer.value()->prepare(OperationContext{}, valid.request).ok());

    auto invalid = make_fixture();
    configure_final_block(invalid, true);
    writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    const auto writes_before = invalid.state->calls(test::FaultFilePhase::Write);
    REQUIRE(!writer.value()->prepare(OperationContext{}, invalid.request).ok());
    REQUIRE_EQ(invalid.state->calls(test::FaultFilePhase::Write), writes_before);
}

TEST_CASE(journal_writer_split_nodes_check_cancel_and_deadline_per_ordinal) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    constexpr auto first_node_readback =
        static_cast<std::uint64_t>(kJournalIndexMaxEntries) + 1;
    constexpr JournalWriterPhaseEvent targets[]{
        {JournalWriterPhase::BeforeIndexWrite, 0},
        {JournalWriterPhase::AfterIndexWrite, 0},
        {JournalWriterPhase::BeforeIndexWrite, 1},
        {JournalWriterPhase::AfterIndexWrite, 1},
        {JournalWriterPhase::BeforeIndexWrite, 2},
        {JournalWriterPhase::AfterIndexWrite, 2},
        {JournalWriterPhase::BeforeReadback, first_node_readback},
        {JournalWriterPhase::AfterReadback, first_node_readback},
        {JournalWriterPhase::BeforeReadback, first_node_readback + 1},
        {JournalWriterPhase::AfterReadback, first_node_readback + 1},
        {JournalWriterPhase::BeforeReadback, first_node_readback + 2},
        {JournalWriterPhase::AfterReadback, first_node_readback + 2},
    };
    for (const auto target : targets) {
        auto fixture = make_fixture();
        configure_split_index_checkpoint(fixture);
        const auto old_prefix = read_committed_prefix(fixture);
        std::stop_source stop;
        bool hook_hit = false;
        auto options = writer_options();
        options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
            if (event.phase == target.phase &&
                event.ordinal == target.ordinal) {
                hook_hit = true;
                stop.request_stop();
            }
            return Result<void>::success();
        };
        auto writer = JournalWriter::create(std::move(options));
        REQUIRE(writer.ok());
        const auto result = writer.value()->prepare(
            OperationContext{stop.get_token()}, fixture.request);
        REQUIRE(hook_hit);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::cancelled));
        require_committed_prefix(fixture, old_prefix);
    }

    for (const auto target : targets) {
        auto fixture = make_fixture();
        configure_split_index_checkpoint(fixture);
        const auto old_prefix = read_committed_prefix(fixture);
        bool hook_hit = false;
        auto options = writer_options();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
            if (event.phase == target.phase &&
                event.ordinal == target.ordinal) {
                hook_hit = true;
                std::this_thread::sleep_until(
                    deadline + std::chrono::milliseconds(2));
            }
            return Result<void>::success();
        };
        auto writer = JournalWriter::create(std::move(options));
        REQUIRE(writer.ok());
        OperationContext context{};
        context.deadline = deadline;
        const auto result = writer.value()->prepare(context, fixture.request);
        REQUIRE(hook_hit);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::timed_out));
        require_committed_prefix(fixture, old_prefix);
    }
#endif
}

TEST_CASE(journal_writer_checks_cancellation_at_every_named_phase) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    constexpr JournalWriterPhaseEvent targets[]{
        {JournalWriterPhase::BeforeBlockWrite, 0},
        {JournalWriterPhase::AfterBlockWrite, 0},
        {JournalWriterPhase::BeforeBlockWrite, 1},
        {JournalWriterPhase::AfterBlockWrite, 1},
        {JournalWriterPhase::BeforeBlockWrite, 2},
        {JournalWriterPhase::AfterBlockWrite, 2},
        {JournalWriterPhase::BeforeIndexWrite, 0},
        {JournalWriterPhase::AfterIndexWrite, 0},
        {JournalWriterPhase::BeforeCommitWrite, 0},
        {JournalWriterPhase::AfterCommitWrite, 0},
        {JournalWriterPhase::BeforeFlush, 0},
        {JournalWriterPhase::AfterFlush, 0},
        {JournalWriterPhase::BeforeReadback, 0},
        {JournalWriterPhase::AfterReadback, 0},
        {JournalWriterPhase::BeforeReadback, 1},
        {JournalWriterPhase::AfterReadback, 1},
        {JournalWriterPhase::BeforeReadback, 2},
        {JournalWriterPhase::AfterReadback, 2},
        {JournalWriterPhase::BeforeReadback, 3},
        {JournalWriterPhase::AfterReadback, 3},
        {JournalWriterPhase::BeforeReadback, 4},
        {JournalWriterPhase::AfterReadback, 4},
        {JournalWriterPhase::BeforeIndexCrosscheck, 0},
        {JournalWriterPhase::AfterIndexCrosscheck, 0},
        {JournalWriterPhase::BeforeIndexCrosscheck, 1},
        {JournalWriterPhase::AfterIndexCrosscheck, 1},
        {JournalWriterPhase::BeforeIndexCrosscheck, 2},
        {JournalWriterPhase::AfterIndexCrosscheck, 2},
        {JournalWriterPhase::Prepared, 0},
    };
    for (const auto target : targets) {
        auto fixture = make_fixture();
        const auto old_prefix = read_committed_prefix(fixture);
        std::stop_source stop;
        bool hook_hit = false;
        auto options = writer_options();
        options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
            if (event.phase == target.phase &&
                event.ordinal == target.ordinal) {
                hook_hit = true;
                stop.request_stop();
            }
            return Result<void>::success();
        };
        auto writer = JournalWriter::create(std::move(options));
        REQUIRE(writer.ok());
        auto result = writer.value()->prepare(
            OperationContext{stop.get_token()}, fixture.request);
        REQUIRE(hook_hit);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::cancelled));
        require_committed_prefix(fixture, old_prefix);
    }


    for (const auto target : targets) {
        auto fixture = make_fixture();
        const auto old_prefix = read_committed_prefix(fixture);
        bool hook_hit = false;
        auto options = writer_options();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        options.phase_hook = [&](const JournalWriterPhaseEvent& event) {
            if (event.phase == target.phase &&
                event.ordinal == target.ordinal) {
                hook_hit = true;
                std::this_thread::sleep_until(
                    deadline + std::chrono::milliseconds(2));
            }
            return Result<void>::success();
        };
        auto writer = JournalWriter::create(std::move(options));
        REQUIRE(writer.ok());
        OperationContext context{};
        context.deadline = deadline;
        auto result = writer.value()->prepare(context, fixture.request);
        REQUIRE(hook_hit);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       OperationErrorCode::timed_out));
        require_committed_prefix(fixture, old_prefix);
    }
#endif

    auto fixture = make_fixture();
    const auto old_prefix = read_committed_prefix(fixture);
    auto writer = JournalWriter::create(writer_options());
    REQUIRE(writer.ok());
    OperationContext expired{};
    expired.deadline = std::chrono::steady_clock::now();
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    auto result = writer.value()->prepare(expired, fixture.request);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(OperationErrorCode::timed_out));
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);
    require_committed_prefix(fixture, old_prefix);
}

}  // namespace
}  // namespace myrd::persistence
