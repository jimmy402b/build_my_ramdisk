#include "test_framework.h"

#include "src/persistence/composite_backing_store.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_reader.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kCompositeTestMiB = 1024ULL * 1024ULL;

class FakeJournalLayer final : public detail::ICompositeJournalLayer {
public:
    detail::CompositeJournalRead next{};
    std::optional<Error> failure;
    std::byte data_byte{std::byte{0x31}};
    std::stop_source* stop_after_read{};
    std::chrono::milliseconds delay{};
    std::optional<std::uint64_t> candidate;
    mutable std::uint64_t calls{};
    mutable std::uint64_t requested_bytes{};
    mutable std::uint64_t largest_request{};

    Result<detail::CompositeJournalRead> read_block(
        const OperationContext&, std::uint64_t,
        const std::span<std::byte> destination) const override {
        ++calls;
        requested_bytes += destination.size();
        largest_request = (std::max)(largest_request,
                                     std::uint64_t{destination.size()});
        if (delay.count() != 0) std::this_thread::sleep_for(delay);
        if (stop_after_read != nullptr) stop_after_read->request_stop();
        if (failure.has_value()) {
            return Result<detail::CompositeJournalRead>::failure(*failure);
        }
        if (next.kind == detail::CompositeJournalKind::Data) {
            std::fill(destination.begin(), destination.end(), data_byte);
        }
        return Result<detail::CompositeJournalRead>::success(next);
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        if (candidate.has_value() && candidate.value() >= first_block) {
            return Result<std::optional<std::uint64_t>>::success(candidate);
        }
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

class FakeBaseLayer final : public detail::ICompositeBaseLayer {
public:
    detail::CompositeBaseRead next{};
    std::optional<Error> failure;
    std::byte data_byte{std::byte{0x52}};
    std::optional<std::uint64_t> candidate;
    mutable std::uint64_t calls{};
    mutable std::uint64_t requested_bytes{};
    mutable std::uint64_t largest_request{};

    Result<detail::CompositeBaseRead> read_block(
        const OperationContext&, std::uint64_t,
        const std::span<std::byte> destination) const override {
        ++calls;
        requested_bytes += destination.size();
        largest_request = (std::max)(largest_request,
                                     std::uint64_t{destination.size()});
        if (failure.has_value()) {
            return Result<detail::CompositeBaseRead>::failure(*failure);
        }
        if (next.kind == detail::CompositeBaseKind::Data) {
            std::fill(destination.begin(), destination.end(), data_byte);
        }
        return Result<detail::CompositeBaseRead>::success(next);
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        if (candidate.has_value() && candidate.value() >= first_block) {
            return Result<std::optional<std::uint64_t>>::success(candidate);
        }
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

CompositeBackingIdentity valid_identity(const std::uint64_t generation = 7) {
    CompositeBackingIdentity identity{};
    identity.schema_version = 1;
    identity.image_uuid.bytes[0] = std::byte{0x11};
    identity.geometry = {8 * 1024 * 1024, 512, 4096};
    identity.checkpoint_generation = generation;
    identity.base_generation = generation;
    identity.base_windows_identity.volume_serial_number = 0x101;
    identity.base_windows_identity.file_id[0] = std::byte{0x12};
    identity.base_vhdx_identity.file_write_guid[0] = std::byte{0x13};
    identity.base_vhdx_identity.data_write_guid[0] = std::byte{0x14};
    identity.virtual_disk_id[0] = std::byte{0x15};
    identity.base_expected_length = 6 * 1024 * 1024;
    identity.journal_windows_identity.volume_serial_number = 0x201;
    identity.journal_windows_identity.file_id[0] = std::byte{0x22};
    identity.journal_identity.journal_uuid[0] = std::byte{0x23};
    identity.journal_epoch = generation;
    identity.journal_root_generation = generation;
    identity.journal_committed_length = 64 * 1024;
    identity.commit.offset = 56 * 1024;
    identity.commit.length = 4096;
    identity.commit.crc32c = 0x12345678;
    identity.commit.chain_digest[0] = std::byte{0x24};
    identity.root.offset = 48 * 1024;
    identity.root.length = 4096;
    identity.root.crc32c = 0x87654321;
    identity.feature_flags = 0x42;
    return identity;
}

struct Layers final {
    std::unique_ptr<FakeJournalLayer> journal{
        std::make_unique<FakeJournalLayer>()};
    std::unique_ptr<FakeBaseLayer> base{
        std::make_unique<FakeBaseLayer>()};
    FakeJournalLayer* journal_raw{journal.get()};
    FakeBaseLayer* base_raw{base.get()};
};

class CompositePayloadSource final : public IVhdxPayloadSource {
public:
    std::map<std::uint64_t, std::vector<std::byte>> payloads;

    Result<void> read_payload(
        const OperationContext&,
        const std::uint64_t payload_index,
        const std::span<std::byte> destination) const override {
        const auto found = payloads.find(payload_index);
        if (found == payloads.end() ||
            found->second.size() != destination.size()) {
            return Result<void>::failure({
                ErrorDomain::internal, 0x5301,
                L"CompositePayloadSource::read_payload",
                L"test payload is missing"});
        }
        std::copy(found->second.begin(), found->second.end(),
                  destination.begin());
        return Result<void>::success();
    }
};

JournalHeader real_journal_header() {
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] =
            static_cast<std::byte>(0x30U + index);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x50U + index);
    }
    header.epoch = 1;
    header.geometry = {8 * kCompositeTestMiB, 512, 4096};
    header.geometry_digest = compute_geometry_digest(header.geometry).value();
    VhdxFileIdentity base{};
    base.file_write_guid[0] = std::byte{0x71};
    base.data_write_guid[0] = std::byte{0x81};
    header.base_identity_digest = compute_base_identity_digest(base).value();
    header.predecessor_digest = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest).value();
    header.feature_flags = 0;
    return header;
}

struct RealJournal final {
    std::unique_ptr<JournalReader> reader;
    std::shared_ptr<test::FaultFileState> state;
    JournalHeader header;
    WindowsFileIdentity windows_identity;
    PreparedCheckpoint prepared;
};

RealJournal make_real_journal_reader() {
    RealJournal result{};
    auto fault = test::make_fault_file(kJournalAlignment);
    auto file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    result.state = std::move(fault.state);
    result.header = real_journal_header();
    result.windows_identity.volume_serial_number = 0x401;
    result.windows_identity.file_id[0] = std::byte{0x41};
    result.windows_identity.attributes = 1;
    result.windows_identity.hard_link_count = 1;
    const auto header_bytes = encode_journal_header(result.header);
    REQUIRE(header_bytes.ok());
    REQUIRE(file->write_exact(0, header_bytes.value()).ok());

    std::array<std::byte, kM2BlockSize> payload{};
    payload.fill(std::byte{0x6a});
    const std::vector<JournalCheckpointBlockInput> blocks{
        {JournalRecordType::Data, 2, 10, 0, 7, payload},
        {JournalRecordType::Zero, 3, 11, 0, 7, {}},
        {JournalRecordType::Discard, 4, 12, 0, 7, {}}};
    JournalCheckpointRequest request{};
    request.file = file;
    request.file_identity = result.windows_identity;
    request.header = result.header;
    request.previous_digest = result.header.predecessor_digest;
    request.committed_length = kJournalAlignment;
    request.operation_id = 91;
    request.blocks = blocks;
    JournalWriterOptions writer_options{};
    writer_options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    writer_options.max_checkpoint_records = 8192;
    writer_options.max_checkpoint_bytes = 64 * kCompositeTestMiB;
    auto writer = JournalWriter::create(writer_options);
    REQUIRE(writer.ok());
    auto prepared = writer.value()->prepare(OperationContext{}, request);
    REQUIRE(prepared.ok());
    result.prepared = prepared.move_value();
    result.windows_identity.size = result.prepared.committed_length;

    result.state->clear_requests_for_test();
    JournalReaderOptions reader_options{};
    reader_options.view.file = std::move(file);
    reader_options.view.file_identity = result.windows_identity;
    reader_options.view.journal_identity = result.header.journal_identity;
    reader_options.view.epoch = result.header.epoch;
    reader_options.view.root_generation = result.prepared.generation;
    reader_options.view.committed_length = result.prepared.committed_length;
    reader_options.view.root = result.prepared.root;
    reader_options.geometry = result.header.geometry;
    reader_options.index_cache = {4 * kIndexNodeBytes, 4};
    auto reader = JournalReader::create(std::move(reader_options));
    REQUIRE(reader.ok());
    result.reader = reader.move_value();
    return result;
}

struct RealBase final {
    std::unique_ptr<VhdxReader> reader;
    VhdxReader* reader_raw{};
    std::shared_ptr<test::FaultFileState> state;
    WindowsFileIdentity windows_identity;
    std::optional<VhdxLayout> layout;
    VhdxWriterIdentity writer_identity;
};

RealBase make_real_vhdx_reader() {
    CompositePayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0x5b}));
    RealBase result{};
    result.writer_identity.file_identity.file_write_guid[0] =
        std::byte{0x71};
    result.writer_identity.file_identity.data_write_guid[0] =
        std::byte{0x81};
    result.writer_identity.virtual_disk_id[0] = std::byte{0x91};
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {8 * kCompositeTestMiB, 512},
        result.writer_identity, candidates, source,
        {8, 8, kVhdxDataBlockBytes});
    REQUIRE(plan.ok());
    auto fault = test::make_fault_file();
    result.state = fault.state;
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(written.ok());
    result.layout = written.value().layout();
    result.windows_identity.volume_serial_number = 0x501;
    result.windows_identity.file_id[0] = std::byte{0x51};
    result.windows_identity.size = result.layout->file_length();
    result.windows_identity.attributes = 1;
    result.windows_identity.hard_link_count = 1;
    result.state->clear_requests_for_test();
    auto reader = VhdxReader::create_for_testing(
        written.value().release_file(), result.windows_identity,
        *result.layout, {{4 * kVhdxBatPageBytes, 4}, 8, 8});
    REQUIRE(reader.ok());
    result.reader = reader.move_value();
    result.reader_raw = result.reader.get();
    return result;
}

std::vector<test::FaultFileRequest> read_requests(
    const std::shared_ptr<test::FaultFileState>& state) {
    auto requests = state->requests();
    requests.erase(std::remove_if(
        requests.begin(), requests.end(), [](const auto& request) {
            return request.phase != test::FaultFilePhase::Read;
        }), requests.end());
    return requests;
}

std::unique_ptr<CompositeBackingStore> create_store(
    Layers& layers, const CompositeBackingIdentity& identity = valid_identity()) {
    auto result = CompositeBackingStore::create_for_testing(
        identity, std::move(layers.journal), std::move(layers.base));
    REQUIRE(result.ok());
    return result.move_value();
}

TEST_CASE(composite_backing_follows_independent_4x3_truth_table) {
    const std::array journal_kinds{
        detail::CompositeJournalKind::Data,
        detail::CompositeJournalKind::Zero,
        detail::CompositeJournalKind::Discard,
        detail::CompositeJournalKind::Absent};
    const std::array base_kinds{
        detail::CompositeBaseKind::Data,
        detail::CompositeBaseKind::Zero,
        detail::CompositeBaseKind::Absent};

    for (const auto journal_kind : journal_kinds) {
        for (const auto base_kind : base_kinds) {
            Layers layers;
            layers.journal_raw->next.kind = journal_kind;
            layers.base_raw->next.kind = base_kind;
            auto store = create_store(layers);
            std::array<std::byte, kM2BlockSize> destination{};
            destination.fill(std::byte{0xa5});

            auto loaded = store->load_block(
                OperationContext{}, 3, destination);

            REQUIRE(loaded.ok());
            const auto journal_hit =
                journal_kind != detail::CompositeJournalKind::Absent;
            REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
            REQUIRE_EQ(layers.base_raw->calls,
                       journal_hit ? std::uint64_t{0} : std::uint64_t{1});
            if (journal_kind == detail::CompositeJournalKind::Data) {
                REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
                REQUIRE(std::all_of(destination.begin(), destination.end(),
                    [](const std::byte value) {
                        return value == std::byte{0x31};
                    }));
            } else if (!journal_hit &&
                       base_kind == detail::CompositeBaseKind::Data) {
                REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
                REQUIRE(std::all_of(destination.begin(), destination.end(),
                    [](const std::byte value) {
                        return value == std::byte{0x52};
                    }));
            } else {
                // VHDX Zero and Absent both mean logical zero to IBackingStore.
                REQUIRE_EQ(loaded.value().kind, BlockLoadKind::AllZero);
                REQUIRE(std::all_of(destination.begin(), destination.end(),
                    [](const std::byte value) {
                        return value == std::byte{0xa5};
                    }));
            }
        }
    }
}

TEST_CASE(composite_backing_never_falls_back_on_journal_errors) {
    const std::array errors{
        Error{ErrorDomain::persistence, 0x5101,
              L"journal.corrupt", L"corrupt provenance", ERROR_CRC},
        Error{ErrorDomain::win32, ERROR_READ_FAULT,
              L"journal.read", L"I/O provenance", ERROR_READ_FAULT},
        Error{ErrorDomain::storage,
              static_cast<std::uint32_t>(OperationErrorCode::cancelled),
              L"journal.cancel", L"cancel provenance"},
        Error{ErrorDomain::storage,
              static_cast<std::uint32_t>(OperationErrorCode::timed_out),
              L"journal.deadline", L"deadline provenance"}};
    for (const auto& expected : errors) {
        Layers layers;
        layers.journal_raw->failure = expected;
        layers.base_raw->next.kind = detail::CompositeBaseKind::Data;
        auto store = create_store(layers);
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xa5});

        auto loaded = store->load_block(OperationContext{}, 0, destination);

        REQUIRE(!loaded.ok());
        REQUIRE_EQ(loaded.error().domain, expected.domain);
        REQUIRE_EQ(loaded.error().code, expected.code);
        REQUIRE_EQ(loaded.error().operation, expected.operation);
        REQUIRE_EQ(loaded.error().message, expected.message);
        REQUIRE_EQ(loaded.error().native_code, expected.native_code);
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
        REQUIRE(std::all_of(destination.begin(), destination.end(),
            [](const std::byte value) {
                return value == std::byte{0xa5};
            }));
    }
}

TEST_CASE(composite_backing_preserves_base_error_provenance) {
    Layers layers;
    layers.journal_raw->next.kind = detail::CompositeJournalKind::Absent;
    const Error expected{ErrorDomain::persistence, 0x5201,
                         L"base.corrupt", L"base provenance", ERROR_CRC};
    layers.base_raw->failure = expected;
    auto store = create_store(layers);
    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0xa5});

    auto loaded = store->load_block(OperationContext{}, 0, destination);

    REQUIRE(!loaded.ok());
    REQUIRE_EQ(loaded.error().domain, expected.domain);
    REQUIRE_EQ(loaded.error().code, expected.code);
    REQUIRE_EQ(loaded.error().operation, expected.operation);
    REQUIRE_EQ(loaded.error().message, expected.message);
    REQUIRE_EQ(loaded.error().native_code, expected.native_code);
    REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
    REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{1});
    REQUIRE(std::all_of(destination.begin(), destination.end(),
        [](const std::byte value) { return value == std::byte{0xa5}; }));
}

TEST_CASE(composite_backing_checks_context_before_each_layer) {
    {
        Layers layers;
        auto store = create_store(layers);
        std::stop_source stopped;
        stopped.request_stop();
        std::array<std::byte, kM2BlockSize> destination{};
        auto loaded = store->load_block(
            OperationContext{stopped.get_token()}, 0, destination);
        REQUIRE(!loaded.ok());
        REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{0});
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    }
    {
        Layers layers;
        layers.journal_raw->next.kind =
            detail::CompositeJournalKind::Absent;
        std::stop_source stopped;
        layers.journal_raw->stop_after_read = &stopped;
        auto store = create_store(layers);
        std::array<std::byte, kM2BlockSize> destination{};
        auto loaded = store->load_block(
            OperationContext{stopped.get_token()}, 0, destination);
        REQUIRE(!loaded.ok());
        REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    }
    {
        Layers layers;
        layers.journal_raw->next.kind =
            detail::CompositeJournalKind::Absent;
        layers.journal_raw->delay = std::chrono::milliseconds(10);
        auto store = create_store(layers);
        std::array<std::byte, kM2BlockSize> destination{};
        auto loaded = store->load_block(
            OperationContext{
                {}, std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(2)},
            0, destination);
        REQUIRE(!loaded.ok());
        REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    }
}

TEST_CASE(composite_backing_merges_ordered_journal_and_bat_candidates) {
    Layers layers;
    layers.journal_raw->candidate = 90;
    layers.base_raw->candidate = 10;
    auto store = create_store(layers);

    std::vector<std::uint64_t> candidates;
    auto merged = store->enumerate_candidate_blocks(
        OperationContext{}, [&candidates](const std::uint64_t block) {
            candidates.push_back(block);
            return Result<void>::success();
        });
    REQUIRE(merged.ok());
    REQUIRE(candidates == std::vector<std::uint64_t>({10, 90}));

    layers.base_raw->candidate = 90;
    candidates.clear();
    auto deduplicated = store->enumerate_candidate_blocks(
        OperationContext{}, [&candidates](const std::uint64_t block) {
            candidates.push_back(block);
            return Result<void>::success();
        });
    REQUIRE(deduplicated.ok());
    REQUIRE(candidates == std::vector<std::uint64_t>({90}));

    layers.journal_raw->candidate = 999;
    layers.base_raw->candidate.reset();
    auto invalid = store->enumerate_candidate_blocks(
        OperationContext{}, [](const std::uint64_t) {
            return Result<void>::success();
        });
    REQUIRE(!invalid.ok());
    REQUIRE_EQ(invalid.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(invalid.error().code, static_cast<std::uint32_t>(
                                         PersistenceErrorCode::BackingDataCorrupt));

    std::stop_source stop;
    stop.request_stop();
    auto cancelled = store->enumerate_candidate_blocks(
        OperationContext{stop.get_token()},
        [](const std::uint64_t) { return Result<void>::success(); });
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(cancelled.error().domain, ErrorDomain::storage);
}

TEST_CASE(composite_backing_preserves_immutable_generation_views) {
    auto old_identity = valid_identity(7);
    auto new_identity = valid_identity(8);
    Layers old_layers;
    old_layers.journal_raw->next.kind =
        detail::CompositeJournalKind::Data;
    old_layers.journal_raw->data_byte = std::byte{0x17};
    Layers new_layers;
    new_layers.journal_raw->next.kind =
        detail::CompositeJournalKind::Data;
    new_layers.journal_raw->data_byte = std::byte{0x28};
    auto old_store = create_store(old_layers, old_identity);
    auto new_store = create_store(new_layers, new_identity);
    std::array<std::byte, kM2BlockSize> old_bytes{};
    std::array<std::byte, kM2BlockSize> new_bytes{};

    REQUIRE(old_store->load_block(
        OperationContext{}, 0, old_bytes).ok());
    REQUIRE(new_store->load_block(
        OperationContext{}, 0, new_bytes).ok());
    REQUIRE_EQ(old_store->identity(), old_identity);
    REQUIRE_EQ(new_store->identity(), new_identity);
    REQUIRE(std::all_of(old_bytes.begin(), old_bytes.end(),
        [](const std::byte value) { return value == std::byte{0x17}; }));
    REQUIRE(std::all_of(new_bytes.begin(), new_bytes.end(),
        [](const std::byte value) { return value == std::byte{0x28}; }));
}

TEST_CASE(composite_backing_generation_views_remain_isolated_concurrently) {
    Layers old_layers;
    old_layers.journal_raw->next.kind = detail::CompositeJournalKind::Data;
    old_layers.journal_raw->data_byte = std::byte{0x17};
    Layers new_layers;
    new_layers.journal_raw->next.kind = detail::CompositeJournalKind::Data;
    new_layers.journal_raw->data_byte = std::byte{0x28};
    auto old_store = create_store(old_layers, valid_identity(7));
    auto new_store = create_store(new_layers, valid_identity(8));
    std::atomic_bool old_ok{true};
    std::atomic_bool new_ok{true};

    std::jthread old_thread([&] {
        for (std::uint64_t i = 0; i < 64; ++i) {
            std::array<std::byte, kM2BlockSize> bytes{};
            const auto loaded = old_store->load_block(
                OperationContext{}, i, bytes);
            if (!loaded.ok() || loaded.value().kind != BlockLoadKind::Data ||
                !std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
                    return value == std::byte{0x17};
                })) {
                old_ok.store(false);
                return;
            }
        }
    });
    std::jthread new_thread([&] {
        for (std::uint64_t i = 0; i < 64; ++i) {
            std::array<std::byte, kM2BlockSize> bytes{};
            const auto loaded = new_store->load_block(
                OperationContext{}, i, bytes);
            if (!loaded.ok() || loaded.value().kind != BlockLoadKind::Data ||
                !std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
                    return value == std::byte{0x28};
                })) {
                new_ok.store(false);
                return;
            }
        }
    });
    old_thread.join();
    new_thread.join();

    REQUIRE(old_ok.load());
    REQUIRE(new_ok.load());
    REQUIRE_EQ(old_store->stats().load_calls, std::uint64_t{64});
    REQUIRE_EQ(new_store->stats().load_calls, std::uint64_t{64});
}

TEST_CASE(composite_backing_identity_compares_every_binding_field) {
    const auto original = valid_identity();
    auto changed = original;
    changed.schema_version += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.image_uuid.bytes[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.geometry.virtual_bytes += 512;
    REQUIRE(!(changed == original));
    changed = original; changed.geometry.logical_sector_size = 4096;
    REQUIRE(!(changed == original));
    changed = original; changed.geometry.physical_sector_size = 8192;
    REQUIRE(!(changed == original));
    changed = original; changed.checkpoint_generation += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.base_generation += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.base_windows_identity.file_id[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.base_vhdx_identity.file_write_guid[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.virtual_disk_id[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.base_expected_length += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.journal_windows_identity.file_id[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.journal_identity.journal_uuid[1] = std::byte{1};
    REQUIRE(!(changed == original));
    changed = original; changed.journal_epoch += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.journal_root_generation += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.journal_committed_length += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.commit.crc32c += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.root.crc32c += 1;
    REQUIRE(!(changed == original));
    changed = original; changed.feature_flags += 1;
    REQUIRE(!(changed == original));
}

TEST_CASE(composite_backing_create_and_load_are_strictly_lazy) {
    Layers layers;
    layers.journal_raw->next.kind = detail::CompositeJournalKind::Absent;
    layers.base_raw->next.kind = detail::CompositeBaseKind::Data;
    auto store = create_store(layers);
    REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{0});
    REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    REQUIRE_EQ(layers.journal_raw->requested_bytes, std::uint64_t{0});
    REQUIRE_EQ(layers.base_raw->requested_bytes, std::uint64_t{0});
    auto before = store->stats();
    REQUIRE_EQ(before.load_calls, std::uint64_t{0});
    std::array<std::byte, kM2BlockSize> destination{};

    auto loaded = store->load_block(OperationContext{}, 1, destination);

    REQUIRE(loaded.ok());
    REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
    REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{1});
    REQUIRE_EQ(layers.journal_raw->requested_bytes, kM2BlockSize);
    REQUIRE_EQ(layers.base_raw->requested_bytes, kM2BlockSize);
    REQUIRE_EQ(layers.journal_raw->largest_request, kM2BlockSize);
    REQUIRE_EQ(layers.base_raw->largest_request, kM2BlockSize);
    const auto after = store->stats();
    REQUIRE_EQ(after.load_calls, std::uint64_t{1});
    REQUIRE_EQ(after.journal_read_calls, std::uint64_t{1});
    REQUIRE_EQ(after.base_read_calls, std::uint64_t{1});
    REQUIRE_EQ(after.fallback_calls, std::uint64_t{1});
}

TEST_CASE(composite_backing_real_readers_touch_only_relevant_lazy_ranges) {
    auto journal = make_real_journal_reader();
    auto base = make_real_vhdx_reader();

    // Reader construction may query bounded metadata, but it must not read an
    // index, BAT or payload.  Composite construction itself performs no I/O.
    REQUIRE(read_requests(journal.state).empty());
    const auto base_open_reads = read_requests(base.state);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 11>
        expected_open_reads{{
            {0, 8},
            {64 * 1024, 4096},
            {128 * 1024, 4096},
            {192 * 1024, 64 * 1024},
            {256 * 1024, 64 * 1024},
            {2 * kCompositeTestMiB, 64 * 1024},
            {2 * kCompositeTestMiB + 64 * 1024, 8},
            {2 * kCompositeTestMiB + 64 * 1024 + 8, 8},
            {2 * kCompositeTestMiB + 64 * 1024 + 16, 16},
            {2 * kCompositeTestMiB + 64 * 1024 + 32, 4},
            {2 * kCompositeTestMiB + 64 * 1024 + 36, 4}}};
    REQUIRE_EQ(base_open_reads.size(), expected_open_reads.size());
    std::uint64_t open_read_bytes{};
    for (std::size_t index = 0; index < base_open_reads.size(); ++index) {
        REQUIRE_EQ(base_open_reads[index].offset,
                   expected_open_reads[index].first);
        REQUIRE_EQ(base_open_reads[index].length,
                   expected_open_reads[index].second);
        open_read_bytes += base_open_reads[index].length;
    }
    REQUIRE_EQ(open_read_bytes, std::uint64_t{204848});
    const auto journal_open_requests = journal.state->requests().size();
    const auto base_open_requests = base.state->requests().size();
    REQUIRE(journal_open_requests <= 1);
    REQUIRE(base_open_requests <= 12);
    journal.state->clear_requests_for_test();
    base.state->clear_requests_for_test();

    CompositeBackingIdentity identity{};
    identity.image_uuid = journal.header.image_uuid;
    identity.geometry = journal.header.geometry;
    identity.checkpoint_generation = journal.prepared.generation;
    identity.base_generation = 0;
    identity.base_windows_identity = base.windows_identity;
    identity.base_vhdx_identity = base.writer_identity.file_identity;
    identity.virtual_disk_id = base.writer_identity.virtual_disk_id;
    identity.base_expected_length = base.layout->file_length();
    identity.journal_windows_identity = journal.windows_identity;
    identity.journal_identity = journal.header.journal_identity;
    identity.journal_epoch = journal.header.epoch;
    identity.journal_root_generation = journal.prepared.generation;
    identity.journal_committed_length = journal.prepared.committed_length;
    identity.commit = journal.prepared.commit;
    identity.root = journal.prepared.root;
    identity.feature_flags = journal.header.feature_flags;
    auto store_result = CompositeBackingStore::create_for_testing(
        identity, std::move(journal.reader), std::move(base.reader));
    REQUIRE(store_result.ok());
    auto store = store_result.move_value();
    REQUIRE(journal.state->requests().empty());
    REQUIRE(base.state->requests().empty());
    std::array<std::byte, kM2BlockSize> destination{};

    auto loaded = store->load_block(OperationContext{}, 0, destination);

    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
    REQUIRE(std::all_of(destination.begin(), destination.end(),
        [](const std::byte value) { return value == std::byte{0x5b}; }));
    const auto journal_reads = read_requests(journal.state);
    REQUIRE_EQ(journal_reads.size(), std::size_t{1});
    REQUIRE_EQ(journal_reads[0].offset, journal.prepared.root.offset);
    REQUIRE_EQ(journal_reads[0].length,
               std::uint64_t{kIndexNodeBytes});
    const auto base_reads = read_requests(base.state);
    REQUIRE_EQ(base_reads.size(), std::size_t{2});
    REQUIRE_EQ(base_reads[0].offset, base.layout->bat_offset());
    REQUIRE_EQ(base_reads[0].length, kVhdxBatPageBytes);
    REQUIRE_EQ(base_reads[1].offset,
               base.layout->file_length() - kVhdxDataBlockBytes);
    REQUIRE_EQ(base_reads[1].length, kM2BlockSize);
    const auto base_stats = base.reader_raw->stats();
    REQUIRE_EQ(base_stats.bat_read_calls, std::uint64_t{1});
    REQUIRE_EQ(base_stats.payload_read_calls, std::uint64_t{1});
    REQUIRE_EQ(base_stats.peak_cache_entries, std::uint64_t{1});
    REQUIRE_EQ(base_stats.peak_cache_bytes, kVhdxBatPageBytes);
    REQUIRE(!journal.state->request_trace_overflowed());
    REQUIRE(!base.state->request_trace_overflowed());

    // The accepted JournalReader API intentionally collapses ZERO/DISCARD to
    // logical AllZero.  Both real record types must nevertheless terminate
    // resolution and suppress older VHDX payload data.
    journal.state->clear_requests_for_test();
    base.state->clear_requests_for_test();
    destination.fill(std::byte{0xa5});
    const auto zero = store->load_block(OperationContext{}, 3, destination);
    REQUIRE(zero.ok());
    REQUIRE_EQ(zero.value().kind, BlockLoadKind::AllZero);
    REQUIRE(std::all_of(destination.begin(), destination.end(),
        [](const std::byte value) { return value == std::byte{0xa5}; }));
    const auto discard = store->load_block(
        OperationContext{}, 4, destination);
    REQUIRE(discard.ok());
    REQUIRE_EQ(discard.value().kind, BlockLoadKind::AllZero);
    REQUIRE(base.state->requests().empty());
    REQUIRE_EQ(base.reader_raw->stats().payload_read_calls,
               std::uint64_t{1});
}

TEST_CASE(composite_backing_accepts_only_the_last_partial_logical_block) {
    auto identity = valid_identity();
    identity.geometry.virtual_bytes = kM2BlockSize + 512;
    Layers layers;
    layers.journal_raw->next.kind = detail::CompositeJournalKind::Zero;
    auto store = create_store(layers, identity);
    std::array<std::byte, kM2BlockSize> destination{};

    REQUIRE(store->load_block(OperationContext{}, 1, destination).ok());
    auto outside = store->load_block(OperationContext{}, 2, destination);
    REQUIRE(!outside.ok());
    REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{1});
}

TEST_CASE(composite_backing_rejects_invalid_construction_and_requests) {
    {
        auto invalid = valid_identity();
        invalid.image_uuid = {};
        Layers layers;
        auto result = CompositeBackingStore::create_for_testing(
            invalid, std::move(layers.journal), std::move(layers.base));
        REQUIRE(!result.ok());
    }
    {
        auto invalid = valid_identity();
        invalid.journal_root_generation += 1;
        Layers layers;
        auto result = CompositeBackingStore::create_for_testing(
            invalid, std::move(layers.journal), std::move(layers.base));
        REQUIRE(!result.ok());
    }
    {
        Layers layers;
        auto result = CompositeBackingStore::create_for_testing(
            valid_identity(), nullptr, std::move(layers.base));
        REQUIRE(!result.ok());
    }
    {
        Layers layers;
        auto store = create_store(layers);
        std::array<std::byte, 4096> too_small{};
        auto loaded = store->load_block(
            OperationContext{}, 0, too_small);
        REQUIRE(!loaded.ok());
        REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{0});
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    }
    {
        Layers layers;
        auto store = create_store(layers);
        std::array<std::byte, kM2BlockSize> destination{};
        auto loaded = store->load_block(
            OperationContext{}, 128, destination);
        REQUIRE(!loaded.ok());
        REQUIRE_EQ(layers.journal_raw->calls, std::uint64_t{0});
        REQUIRE_EQ(layers.base_raw->calls, std::uint64_t{0});
    }
}

TEST_CASE(composite_backing_rejects_readers_bound_to_different_guards) {
    const auto root = std::filesystem::temp_directory_path() /
        (L"myrd-composite-binding-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    struct Cleanup final {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};
    const auto make_file = [&](const wchar_t* name, std::uint64_t size) {
        std::ofstream output(
            root / name, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.seekp(static_cast<std::streamoff>(size - 1));
        output.put('\0');
        REQUIRE(output.good());
    };

    auto layout_source = make_real_vhdx_reader();
    const auto base_length = layout_source.layout->file_length();
    make_file(L"journal-a.rdj", 64 * 1024);
    make_file(L"journal-b.rdj", 64 * 1024);
    CompositePayloadSource physical_source;
    physical_source.payloads.emplace(
        0, std::vector<std::byte>(
               kVhdxDataBlockBytes, std::byte{0x5b}));
    const std::vector<std::uint64_t> physical_candidates{0};
    auto physical_plan = plan_sparse_vhdx(
        OperationContext{}, {8 * kCompositeTestMiB, 512},
        layout_source.writer_identity, physical_candidates,
        physical_source, {8, 8, kVhdxDataBlockBytes});
    REQUIRE(physical_plan.ok());
    auto physical_file = open_windows_persistence_file(
        root / L"base-b.vhdx", FileOpenMode::CreateNewReadWrite);
    REQUIRE(physical_file.ok());
    auto physical_written = write_sparse_vhdx_file(
        OperationContext{}, physical_plan.value(), physical_source,
        physical_file.move_value());
    REQUIRE(physical_written.ok());
    auto closed_physical = physical_written.value().release_file();
    REQUIRE(closed_physical->close().ok());

    auto journal_a = open_bundle_file_for_backing_view(
        root, "journal-a.rdj");
    auto journal_b = open_bundle_file_for_backing_view(
        root, "journal-b.rdj");
    auto base_b = open_bundle_file_for_backing_view(root, "base-b.vhdx");
    REQUIRE(journal_a.ok());
    REQUIRE(journal_b.ok());
    REQUIRE(base_b.ok());
    auto journal_a_opened = journal_a.move_value();
    auto journal_b_opened = journal_b.move_value();
    auto base_b_opened = base_b.move_value();

    auto journal_file = open_validated_persistence_file(
        journal_a_opened.take_file());
    REQUIRE(journal_file.ok());
    JournalReaderOptions journal_options{};
    journal_options.view.file = std::shared_ptr<IPersistenceFile>(
        journal_file.move_value());
    journal_options.view.file_identity = journal_a_opened.identity();
    journal_options.view.journal_identity.journal_uuid[0] = std::byte{0x71};
    journal_options.view.epoch = 7;
    journal_options.view.root_generation = 7;
    journal_options.view.committed_length = 64 * 1024;
    journal_options.geometry = {8 * 1024 * 1024, 512, 4096};
    journal_options.index_cache = {4 * kIndexNodeBytes, 4};
    auto journal_reader = JournalReader::create(std::move(journal_options));
    REQUIRE(journal_reader.ok());

    auto base_file = open_validated_persistence_file(
        base_b_opened.take_file());
    REQUIRE(base_file.ok());
    auto base_reader = VhdxReader::create_for_testing(
        base_file.move_value(), base_b_opened.identity(),
        *layout_source.layout,
        {{4 * kVhdxBatPageBytes, 4}, 8, 8});
    REQUIRE(base_reader.ok());

    auto identity = valid_identity();
    identity.base_windows_identity = base_b_opened.identity();
    identity.base_expected_length = base_length;
    identity.base_vhdx_identity =
        layout_source.layout->file_identity();
    identity.virtual_disk_id = layout_source.layout->virtual_disk_id();
    identity.journal_windows_identity = journal_b_opened.identity();
    identity.journal_committed_length = 64 * 1024;
    auto rejected = CompositeBackingStore::create(
        identity, journal_reader.move_value(), base_reader.move_value(),
        journal_b_opened.take_delete_guard(),
        base_b_opened.take_delete_guard());
    REQUIRE(!rejected.ok());
}

}  // namespace
}  // namespace myrd::persistence
