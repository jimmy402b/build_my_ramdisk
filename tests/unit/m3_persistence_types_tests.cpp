/// @file m3_persistence_types_tests.cpp
/// @brief T13.1–T13.7 — Frozen enum values, stable strings, geometry constants,
///        phase/session names, geometry validation, checked increment,
///        PersistenceLimits, CheckpointOutcome, VolumeBarrierKind validation,
///        error context, exhaustive enum coverage per SPEC and PLAN.

#include "test_framework.h"
#include "test_paths.h"

#include "src/common/sha256.h"
#include "src/persistence/persistence_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace myrd {
namespace persistence {
namespace {

using std::string_view;

template <typename Enum, typename Wire>
struct WireCase final {
    Enum value;
    Wire wire;
};

struct FaultPhaseFixture final {
    std::string id;
    std::string boundary;
    bool supports_short{false};
    bool supports_torn{false};
    bool supports_kill{false};
    std::string gate;
    std::string state_disposition;
    std::string error_disposition;
    std::string kill_recovery;
};

PersistenceLimits valid_limits() {
    PersistenceLimits limits{};
    limits.max_virtual_bytes = 64ULL * 1024 * 1024;
    limits.max_journal_committed_bytes = kMinimumDataRecordBytes;
    limits.max_journal_records = 1;
    limits.max_index_cache_bytes = kIndexNodeBytes;
    limits.max_index_cache_entries = 1;
    limits.max_bat_cache_bytes = sizeof(std::uint64_t);
    limits.max_bat_cache_entries = 1;
    limits.max_io_scratch_bytes = kM2BlockSize;
    limits.max_verify_batch_bytes = kM2BlockSize;
    return limits;
}

PersistenceErrorContext complete_error_context(
    std::string operation,
    PersistenceErrorCode code,
    SessionPhase phase,
    std::uint64_t generation,
    CommitDisposition disposition)
{
    PersistenceErrorContext context{};
    context.operation = std::move(operation);
    context.code = code;
    context.phase = phase;
    context.operation_id = 42;
    context.image_uuid.bytes[0] = std::byte{0xA5};
    context.generation = generation;
    context.disposition = disposition;
    return context;
}

std::vector<FaultPhaseFixture> parse_fault_phase_fixtures(
    const std::string& json) {
    const std::regex phase_pattern{
        R"json(\{\s*"id"\s*:\s*"([^"]+)"\s*,\s*"boundary"\s*:\s*"([^"]+)"\s*,\s*"short"\s*:\s*(true|false)\s*,\s*"torn"\s*:\s*(true|false)\s*,\s*"kill"\s*:\s*(true|false)\s*,\s*"gate"\s*:\s*"([^"]+)"\s*,\s*"state_disposition"\s*:\s*"([^"]+)"\s*,\s*"error_disposition"\s*:\s*"([^"]+)"\s*,\s*"kill_recovery"\s*:\s*"([^"]+)"\s*\})json"};

    std::vector<FaultPhaseFixture> phases;
    for (auto it = std::sregex_iterator(json.begin(), json.end(), phase_pattern);
         it != std::sregex_iterator{};
         ++it) {
        phases.push_back(FaultPhaseFixture{
            (*it)[1].str(),
            (*it)[2].str(),
            (*it)[3].str() == "true",
            (*it)[4].str() == "true",
            (*it)[5].str() == "true",
            (*it)[6].str(),
            (*it)[7].str(),
            (*it)[8].str(),
            (*it)[9].str()});
    }
    return phases;
}

// ============================================================================
// T13.1: Geometry constants (PLAN §1.4, frozen)
// ============================================================================

TEST_CASE(geometry_constants_are_frozen) {
    REQUIRE_EQ(kManifestSlotBytes, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kManifestFileBytes, static_cast<std::size_t>(8192));
    REQUIRE_EQ(kJournalAlignment, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kIndexNodeBytes, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kMaxRelativeNameBytes, static_cast<std::size_t>(255));
    REQUIRE_EQ(kM3FormatMajor, static_cast<std::uint32_t>(1));
    REQUIRE_EQ(kM3FormatMinor, static_cast<std::uint32_t>(0));
    REQUIRE_EQ(kVhdxDataBlockBytes,
              static_cast<std::uint32_t>(2U * 1024U * 1024U));
    REQUIRE_EQ(kMaxIndexDepth, static_cast<std::uint32_t>(8));
}

// ============================================================================
// T13.2: Sector / block constants (SPEC §8.6, frozen)
// ============================================================================

TEST_CASE(sector_block_constants_are_frozen) {
    REQUIRE_EQ(kLogicalSectorSize512, 512U);
    REQUIRE_EQ(kLogicalSectorSize4096, 4096U);
    REQUIRE_EQ(kPhysicalSectorSize, 4096U);
    REQUIRE_EQ(kM2BlockSize, static_cast<std::uint64_t>(64ULL * 1024ULL));
}

// ============================================================================
// T13.2: ImageGeometry default & validation
// ============================================================================

TEST_CASE(image_geometry_default_is_zero) {
    ImageGeometry g{};
    REQUIRE_EQ(g.virtual_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(g.logical_sector_size, 0U);
    REQUIRE_EQ(g.physical_sector_size, 0U);
}

TEST_CASE(validate_geometry_rejects_zero_virtual_bytes) {
    ImageGeometry g{};
    g.virtual_bytes = 0;
    g.logical_sector_size = 512;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::VirtualBytesZero);
}

TEST_CASE(validate_geometry_rejects_invalid_logical_sector) {
    ImageGeometry g{};
    g.virtual_bytes = 65536;
    g.logical_sector_size = 1024; // not 512 or 4096
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::LogicalSectorInvalid);
}

TEST_CASE(validate_geometry_rejects_logical_sector_2048) {
    ImageGeometry g{};
    g.virtual_bytes = 65536;
    g.logical_sector_size = 2048;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::LogicalSectorInvalid);
}

TEST_CASE(validate_geometry_rejects_invalid_physical_sector) {
    ImageGeometry g{};
    g.virtual_bytes = 65536;
    g.logical_sector_size = 512;
    g.physical_sector_size = 512; // not 4096
    REQUIRE(validate_geometry(g) == GeometryStatus::PhysicalSectorInvalid);
}

TEST_CASE(validate_geometry_rejects_not_aligned_to_logical_sector) {
    ImageGeometry g{};
    g.virtual_bytes = 513; // not multiple of 512
    g.logical_sector_size = 512;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::NotAlignedToLogicalSector);
}

TEST_CASE(validate_geometry_accepts_512_sector_valid_geometry) {
    ImageGeometry g{};
    g.virtual_bytes = 64ULL * 1024ULL * 1024ULL; // 64 MiB
    g.logical_sector_size = 512;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::Ok);
}

TEST_CASE(validate_geometry_accepts_4096_sector_valid_geometry) {
    ImageGeometry g{};
    g.virtual_bytes = 64ULL * 1024ULL * 1024ULL;
    g.logical_sector_size = 4096;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::Ok);
}

TEST_CASE(validate_geometry_accepts_one_logical_sector) {
    ImageGeometry sector_512{512, 512, 4096};
    REQUIRE_EQ(validate_geometry(sector_512), GeometryStatus::Ok);

    ImageGeometry sector_4096{4096, 4096, 4096};
    REQUIRE_EQ(validate_geometry(sector_4096), GeometryStatus::Ok);
}

TEST_CASE(validate_geometry_accepts_large_geometry) {
    ImageGeometry g{};
    g.virtual_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL; // 1 TiB
    g.logical_sector_size = 4096;
    g.physical_sector_size = 4096;
    REQUIRE(validate_geometry(g) == GeometryStatus::Ok);
}

// ============================================================================
// T13.2: Checked increment (PLAN §1.4)
// ============================================================================

TEST_CASE(checked_increment_increments_value) {
    std::uint64_t val = 0;
    REQUIRE(checked_increment(val));
    REQUIRE_EQ(val, static_cast<std::uint64_t>(1));
}

TEST_CASE(checked_increment_increments_large_value) {
    std::uint64_t val = 100;
    REQUIRE(checked_increment(val));
    REQUIRE_EQ(val, static_cast<std::uint64_t>(101));
}

TEST_CASE(checked_increment_rejects_uint64_max) {
    std::uint64_t val = UINT64_MAX;
    REQUIRE(!checked_increment(val));
    REQUIRE_EQ(val, UINT64_MAX); // value unchanged
}

TEST_CASE(checked_increment_rejects_uint64_max_minus_one_then_wraps) {
    std::uint64_t val = UINT64_MAX - 1;
    REQUIRE(!checked_increment(val));
    REQUIRE_EQ(val, UINT64_MAX - 1);
}

TEST_CASE(checked_increment_or_fatal_returns_stable_persistence_error_on_wrap) {
    auto context = complete_error_context(
        "checkpoint-generation",
        PersistenceErrorCode::FatalInternalInvariant,
        SessionPhase::Checkpointing,
        7,
        CommitDisposition::NotCommitted);
    const auto complete = CompletePersistenceErrorContext::create(context);
    REQUIRE(complete.has_value());

    auto success = checked_increment_or_fatal(0, *complete);
    REQUIRE(success.ok());
    REQUIRE_EQ(success.value(), 1ull);

    auto failure = checked_increment_or_fatal(
        UINT64_MAX - 1, *complete);
    REQUIRE(!failure.ok());
    REQUIRE_EQ(failure.error().common_error.domain,
               ErrorDomain::persistence);
    REQUIRE_EQ(failure.error().common_error.code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::FatalInternalInvariant));
    REQUIRE_EQ(failure.error().common_error.operation,
               std::wstring{L"checkpoint-generation"});
    REQUIRE(is_complete_error_context(failure.error().context));

    context.image_uuid = {};
    REQUIRE(!CompletePersistenceErrorContext::create(context).has_value());
}

TEST_CASE(persistence_generation_descriptors_have_owned_stable_fields) {
    ManifestGenerationDescriptor descriptor{};
    descriptor.checkpoint_generation = 9;
    descriptor.base_generation = 8;
    descriptor.journal.epoch = 3;
    descriptor.journal.committed_length = 8192;
    descriptor.journal.commit.offset = 4096;
    descriptor.journal.commit.length = 4096;
    descriptor.journal.commit.crc32c = 0x12345678U;
    descriptor.journal.commit.chain_digest[0] = std::byte{0xA5};
    descriptor.journal.root.offset = 12288;
    descriptor.journal.root.length = kIndexNodeBytes;
    descriptor.journal.root.crc32c = 0x90ABCDEFU;

    const auto copy = descriptor;
    REQUIRE_EQ(copy, descriptor);
    REQUIRE_EQ(copy.journal.commit.chain_digest.size(), 32ull);
}

// ============================================================================
// T13.3: PersistenceLimits default & validation
// ============================================================================

TEST_CASE(persistence_limits_default_is_zero) {
    PersistenceLimits lim{};
    REQUIRE_EQ(lim.max_virtual_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_journal_committed_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_journal_records, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_index_cache_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_index_cache_entries, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_bat_cache_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_bat_cache_entries, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_io_scratch_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(lim.max_verify_batch_bytes, static_cast<std::uint64_t>(0));
}

TEST_CASE(validate_limits_rejects_all_zero) {
    PersistenceLimits lim{};
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxVirtualBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_max_virtual_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 0;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxVirtualBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_max_journal_committed_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 0;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxJournalCommittedBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_max_journal_records) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 0;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxJournalRecordsZero);
}

TEST_CASE(validate_limits_rejects_committed_bytes_smaller_than_record) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 2048; // less than kJournalAlignment
    lim.max_journal_records = 10;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::SingleRecordExceedsJournalBytes);
}

TEST_CASE(validate_limits_rejects_zero_index_cache_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 0;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxIndexCacheBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_index_cache_entries) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 0;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxIndexCacheEntriesZero);
}

TEST_CASE(validate_limits_rejects_index_entries_exceeding_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 100; // 100 * 4096 > 4096
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::CacheEntriesExceedIndexBytes);
}

TEST_CASE(validate_limits_rejects_zero_bat_cache_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 0;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxBatCacheBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_bat_cache_entries) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 0;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxBatCacheEntriesZero);
}

TEST_CASE(validate_limits_rejects_bat_entries_exceeding_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 16;
    lim.max_bat_cache_entries = 10; // 10 * 8 = 80 > 16
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::CacheEntriesExceedBatBytes);
}

TEST_CASE(validate_limits_rejects_zero_io_scratch_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 0;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxIoScratchBytesZero);
}

TEST_CASE(validate_limits_rejects_zero_verify_batch_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 65536;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 0;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxVerifyBatchBytesZero);
}

TEST_CASE(validate_limits_detects_arithmetic_overflow) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = UINT64_MAX - 4095;
    lim.max_journal_committed_bytes = kMinimumDataRecordBytes;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize; // will overflow with max_virtual_bytes
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::ArithmeticOverflow);
}

TEST_CASE(validate_limits_accepts_valid_limits) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kMinimumDataRecordBytes;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::Ok);
}

TEST_CASE(validate_limits_rejects_unaligned_virtual_limit) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024 + 1;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxVirtualBytesNotSectorAligned);
}

TEST_CASE(validate_limits_rejects_hidden_unlimited_virtual_limit) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = UINT64_MAX;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::MaxVirtualBytesUnlimited);
}

TEST_CASE(validate_limits_rejects_hidden_unlimited_non_virtual_limits) {
    using Field = std::uint64_t PersistenceLimits::*;
    constexpr std::array<Field, 8> fields{
        &PersistenceLimits::max_journal_committed_bytes,
        &PersistenceLimits::max_journal_records,
        &PersistenceLimits::max_index_cache_bytes,
        &PersistenceLimits::max_index_cache_entries,
        &PersistenceLimits::max_bat_cache_bytes,
        &PersistenceLimits::max_bat_cache_entries,
        &PersistenceLimits::max_io_scratch_bytes,
        &PersistenceLimits::max_verify_batch_bytes,
    };

    for (const auto field : fields) {
        auto limits = valid_limits();
        limits.*field = UINT64_MAX;
        REQUIRE_EQ(validate_limits(limits), LimitsStatus::HiddenUnlimited);
    }
}

TEST_CASE(validate_limits_rejects_journal_without_complete_data_record) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment - 1;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::SingleRecordExceedsJournalBytes);
}

TEST_CASE(validate_limits_rejects_more_records_than_committed_bytes_allow) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment;
    lim.max_journal_records =
        lim.max_journal_committed_bytes / kJournalAlignment + 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::JournalRecordsExceedCommittedBytes);
}

TEST_CASE(validate_limits_allows_aligned_metadata_records_within_journal_bytes) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kMinimumDataRecordBytes;
    lim.max_journal_records =
        kMinimumDataRecordBytes / kJournalAlignment;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE_EQ(validate_limits(lim), LimitsStatus::Ok);

    ++lim.max_journal_records;
    REQUIRE_EQ(validate_limits(lim),
               LimitsStatus::JournalRecordsExceedCommittedBytes);
}

TEST_CASE(validate_limits_rejects_io_scratch_smaller_than_block) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize - 1;
    lim.max_verify_batch_bytes = kM2BlockSize;
    REQUIRE(validate_limits(lim) == LimitsStatus::IoScratchTooSmall);
}

TEST_CASE(validate_limits_rejects_verify_batch_smaller_than_block) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = kM2BlockSize + kJournalAlignment;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = kIndexNodeBytes;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = kM2BlockSize;
    lim.max_verify_batch_bytes = kM2BlockSize - 1;
    REQUIRE(validate_limits(lim) == LimitsStatus::VerifyBatchTooSmall);
}

// ============================================================================
// T13.1: PersistenceErrorCode — SPEC §14 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(persistence_error_code_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat), 1U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::UnsupportedVhdx), 2U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::ManifestCorrupt), 3U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::JournalCorrupt), 4U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::BackingDataCorrupt), 5U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::ImageInUse), 6U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy), 7U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::GenerationConflict), 8U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotBusy), 9U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotAborted), 10U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotResourceExhausted), 11U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::CommitIndeterminate), 12U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::DiskFull), 13U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::AccessDenied), 14U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::IoError), 15U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::Cancelled), 16U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::DeadlineExceeded), 17U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed), 18U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::FatalInternalInvariant), 19U);
}

TEST_CASE(persistence_error_code_stable_strings) {
    REQUIRE_EQ(to_string(PersistenceErrorCode::InvalidFormat), string_view("InvalidFormat"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::UnsupportedVhdx), string_view("UnsupportedVhdx"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::ManifestCorrupt), string_view("ManifestCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::JournalCorrupt), string_view("JournalCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::BackingDataCorrupt), string_view("BackingDataCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::ImageInUse), string_view("ImageInUse"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::OperationBusy), string_view("OperationBusy"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::GenerationConflict), string_view("GenerationConflict"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotBusy), string_view("SnapshotBusy"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotAborted), string_view("SnapshotAborted"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotResourceExhausted), string_view("SnapshotResourceExhausted"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::CommitIndeterminate), string_view("CommitIndeterminate"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::DiskFull), string_view("DiskFull"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::AccessDenied), string_view("AccessDenied"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::IoError), string_view("IoError"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::Cancelled), string_view("Cancelled"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::DeadlineExceeded), string_view("DeadlineExceeded"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::VerificationFailed), string_view("VerificationFailed"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::FatalInternalInvariant), string_view("FatalInternalInvariant"));
}

// ============================================================================
// T13.1: CommitDisposition — PLAN §1.5 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(commit_disposition_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::NotCommitted), static_cast<std::uint8_t>(0));
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::CommitIndeterminate), static_cast<std::uint8_t>(1));
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::Committed), static_cast<std::uint8_t>(2));
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::CommittedNeedsReconciliation), static_cast<std::uint8_t>(3));
}

TEST_CASE(commit_disposition_stable_strings) {
    REQUIRE_EQ(to_string(CommitDisposition::NotCommitted), string_view("NotCommitted"));
    REQUIRE_EQ(to_string(CommitDisposition::CommitIndeterminate), string_view("CommitIndeterminate"));
    REQUIRE_EQ(to_string(CommitDisposition::Committed), string_view("Committed"));
    REQUIRE_EQ(to_string(CommitDisposition::CommittedNeedsReconciliation), string_view("CommittedNeedsReconciliation"));
}

// ============================================================================
// T13.4: CommitDisposition exhaustive transition table
// ============================================================================

TEST_CASE(not_committed_can_transition_to_any_disposition) {
    REQUIRE(is_valid_disposition_transition(CommitDisposition::NotCommitted, CommitDisposition::NotCommitted));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::NotCommitted, CommitDisposition::CommitIndeterminate));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::NotCommitted, CommitDisposition::Committed));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::NotCommitted, CommitDisposition::CommittedNeedsReconciliation));
}

TEST_CASE(commit_indeterminate_can_only_resolve_to_not_committed_or_reconciliation) {
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::NotCommitted));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::CommitIndeterminate));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::Committed));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::CommittedNeedsReconciliation));
}

TEST_CASE(committed_can_only_stay_committed_or_require_reconciliation) {
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::NotCommitted));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::CommitIndeterminate));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::Committed));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::CommittedNeedsReconciliation));
}

TEST_CASE(committed_needs_reconciliation_can_become_committed_or_stay) {
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::CommittedNeedsReconciliation, CommitDisposition::NotCommitted));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::CommittedNeedsReconciliation, CommitDisposition::CommitIndeterminate));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommittedNeedsReconciliation, CommitDisposition::Committed));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommittedNeedsReconciliation, CommitDisposition::CommittedNeedsReconciliation));
}

// ============================================================================
// T13.4: CheckpointOutcome default values
// ============================================================================

TEST_CASE(checkpoint_outcome_default_is_not_committed) {
    CheckpointOutcome outcome{};
    REQUIRE_EQ(outcome.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(outcome.generation, static_cast<std::uint64_t>(0));
    REQUIRE(!outcome.manifest_candidate_verified);
    REQUIRE(!outcome.backing_view_installed);
    REQUIRE_EQ(outcome.acknowledged_blocks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(outcome.still_dirty_blocks, static_cast<std::uint64_t>(0));
    REQUIRE(!outcome.error.has_value());
    REQUIRE(!outcome.error_context.has_value());
}

static_assert(std::is_same_v<
              decltype(CheckpointOutcome::error),
              std::optional<myrd::Error>>);
static_assert(std::is_same_v<
              decltype(CheckpointOutcome::error_context),
              std::optional<PersistenceErrorContext>>);

TEST_CASE(checkpoint_outcome_committed_path) {
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::Committed;
    outcome.generation = 5;
    outcome.manifest_candidate_verified = true;
    outcome.backing_view_installed = true;
    outcome.acknowledged_blocks = 100;
    outcome.still_dirty_blocks = 3;
    REQUIRE_EQ(outcome.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(outcome.generation, static_cast<std::uint64_t>(5));
    REQUIRE(outcome.manifest_candidate_verified);
    REQUIRE(outcome.backing_view_installed);
    REQUIRE_EQ(outcome.acknowledged_blocks, static_cast<std::uint64_t>(100));
    REQUIRE_EQ(outcome.still_dirty_blocks, static_cast<std::uint64_t>(3));
}

// ============================================================================
// T13.1: VolumeBarrierKind — PLAN §1.5 (frozen values + stable strings)
// ============================================================================

TEST_CASE(volume_barrier_kind_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint8_t>(VolumeBarrierKind::WindowsVolumeFlushed), static_cast<std::uint8_t>(0));
    REQUIRE_EQ(static_cast<std::uint8_t>(VolumeBarrierKind::RawBlockTestNoFilesystemCache), static_cast<std::uint8_t>(1));
}

TEST_CASE(volume_barrier_kind_stable_strings) {
    REQUIRE_EQ(to_string(VolumeBarrierKind::WindowsVolumeFlushed), string_view("WindowsVolumeFlushed"));
    REQUIRE_EQ(to_string(VolumeBarrierKind::RawBlockTestNoFilesystemCache), string_view("RawBlockTestNoFilesystemCache"));
}

// ============================================================================
// T13.5: VolumeBarrierKind validation
// ============================================================================

TEST_CASE(is_valid_volume_barrier_kind_recognizes_valid_values) {
    REQUIRE(is_valid_volume_barrier_kind(VolumeBarrierKind::WindowsVolumeFlushed));
    REQUIRE(is_valid_volume_barrier_kind(VolumeBarrierKind::RawBlockTestNoFilesystemCache));
}

TEST_CASE(is_valid_volume_barrier_kind_rejects_out_of_range) {
    REQUIRE(!is_valid_volume_barrier_kind(static_cast<VolumeBarrierKind>(2)));
    REQUIRE(!is_valid_volume_barrier_kind(static_cast<VolumeBarrierKind>(255)));
}

TEST_CASE(permits_filesystem_durability_claim_only_for_windows_volume_flushed) {
    REQUIRE(permits_filesystem_durability_claim(VolumeBarrierKind::WindowsVolumeFlushed));
    REQUIRE(!permits_filesystem_durability_claim(VolumeBarrierKind::RawBlockTestNoFilesystemCache));
}

TEST_CASE(raw_test_barrier_explicitly_no_filesystem_durability) {
    REQUIRE(!permits_filesystem_durability_claim(VolumeBarrierKind::RawBlockTestNoFilesystemCache));
}

TEST_CASE(is_valid_barrier_for_checkpoint_rejects_unknown) {
    REQUIRE(!is_valid_barrier_for_checkpoint(static_cast<VolumeBarrierKind>(3)));
    REQUIRE(!is_valid_barrier_for_checkpoint(static_cast<VolumeBarrierKind>(254)));
}

TEST_CASE(checkpoint_barrier_validation_rejects_missing_optional) {
    const std::optional<VolumeBarrierKind> missing;
    REQUIRE(!is_valid_barrier_for_checkpoint(missing));
    REQUIRE(is_valid_barrier_for_checkpoint(
        std::optional{VolumeBarrierKind::WindowsVolumeFlushed}));
    REQUIRE(!is_valid_barrier_for_checkpoint(std::optional{
        static_cast<VolumeBarrierKind>(0xFF)}));
}

// ============================================================================
// T13.1: SessionPhase — SPEC §12 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(session_phase_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Closed), static_cast<std::uint8_t>(0));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Opening), static_cast<std::uint8_t>(1));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Ready), static_cast<std::uint8_t>(2));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Checkpointing), static_cast<std::uint8_t>(3));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Compacting), static_cast<std::uint8_t>(4));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Verifying), static_cast<std::uint8_t>(5));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::ResolvingCommit), static_cast<std::uint8_t>(6));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Reconciling), static_cast<std::uint8_t>(7));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Closing), static_cast<std::uint8_t>(8));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::PersistenceFaulted), static_cast<std::uint8_t>(9));
}

TEST_CASE(session_phase_stable_strings) {
    REQUIRE_EQ(to_string(SessionPhase::Closed), string_view("Closed"));
    REQUIRE_EQ(to_string(SessionPhase::Opening), string_view("Opening"));
    REQUIRE_EQ(to_string(SessionPhase::Ready), string_view("Ready"));
    REQUIRE_EQ(to_string(SessionPhase::Checkpointing), string_view("Checkpointing"));
    REQUIRE_EQ(to_string(SessionPhase::Compacting), string_view("Compacting"));
    REQUIRE_EQ(to_string(SessionPhase::Verifying), string_view("Verifying"));
    REQUIRE_EQ(to_string(SessionPhase::ResolvingCommit), string_view("ResolvingCommit"));
    REQUIRE_EQ(to_string(SessionPhase::Reconciling), string_view("Reconciling"));
    REQUIRE_EQ(to_string(SessionPhase::Closing), string_view("Closing"));
    REQUIRE_EQ(to_string(SessionPhase::PersistenceFaulted), string_view("PersistenceFaulted"));
}

// ============================================================================
// T13.6: PersistenceErrorContext default values and required fields
// ============================================================================

TEST_CASE(error_context_default_is_empty) {
    PersistenceErrorContext ctx{};
    REQUIRE(ctx.operation.empty());
    REQUIRE_EQ(ctx.code, PersistenceErrorCode::FatalInternalInvariant);
    REQUIRE_EQ(ctx.phase, SessionPhase::Closed);
    REQUIRE_EQ(ctx.native_code, 0U);
    REQUIRE_EQ(ctx.operation_id, static_cast<std::uint64_t>(0));
    REQUIRE(ctx.image_uuid.is_nil());
    REQUIRE_EQ(ctx.generation, kInvalidGeneration);
    REQUIRE_EQ(ctx.disposition, CommitDisposition::NotCommitted);
    REQUIRE(ctx.safe_context.empty());
}

static_assert(std::is_same_v<
              decltype(PersistenceErrorContext::operation), std::string>);
static_assert(std::is_same_v<
              decltype(PersistenceErrorContext::image_uuid), ImageUuid>);

TEST_CASE(error_context_default_passes_is_default_check) {
    PersistenceErrorContext ctx{};
    REQUIRE(is_default_context(ctx));
}

TEST_CASE(error_context_with_data_is_not_default) {
    PersistenceErrorContext ctx{};
    ctx.operation = "checkpoint";
    REQUIRE(!is_default_context(ctx));
}

TEST_CASE(error_context_holds_all_required_fields) {
    PersistenceErrorContext ctx{};
    ctx.operation = "recovery";
    ctx.code = PersistenceErrorCode::JournalCorrupt;
    ctx.phase = SessionPhase::Opening;
    ctx.native_code = 5;
    ctx.operation_id = 42;
    ctx.image_uuid.bytes[0] = std::byte{0x01};
    ctx.image_uuid.bytes[15] = std::byte{0xEF};
    ctx.generation = 3;
    ctx.disposition = CommitDisposition::CommitIndeterminate;
    ctx.safe_context = "journal header verification";

    REQUIRE_EQ(ctx.operation, string_view("recovery"));
    REQUIRE_EQ(ctx.code, PersistenceErrorCode::JournalCorrupt);
    REQUIRE_EQ(ctx.phase, SessionPhase::Opening);
    REQUIRE_EQ(ctx.native_code, 5U);
    REQUIRE_EQ(ctx.operation_id, static_cast<std::uint64_t>(42));
    REQUIRE_EQ(ctx.image_uuid.bytes[0], std::byte{0x01});
    REQUIRE_EQ(ctx.image_uuid.bytes[15], std::byte{0xEF});
    REQUIRE_EQ(ctx.generation, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(ctx.disposition, CommitDisposition::CommitIndeterminate);
    REQUIRE_EQ(ctx.safe_context,
               string_view("journal header verification"));
}

TEST_CASE(error_context_owns_operation_and_safe_context_strings) {
    std::string operation = "checkpoint";
    std::string safe_context = "manifest flush";
    PersistenceErrorContext ctx{};
    ctx.operation = operation;
    ctx.safe_context = safe_context;

    operation.assign("changed");
    safe_context.assign("changed");
    REQUIRE_EQ(ctx.operation, string_view("checkpoint"));
    REQUIRE_EQ(ctx.safe_context, string_view("manifest flush"));
}

TEST_CASE(checkpoint_outcome_carries_error_and_matching_context) {
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::CommitIndeterminate;
    outcome.generation = 9;
    outcome.error = myrd::Error{
        myrd::ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::CommitIndeterminate),
        L"checkpoint",
        L"manifest publish result is unknown",
        1117};
    PersistenceErrorContext context{};
    context.operation = "checkpoint";
    context.code = PersistenceErrorCode::CommitIndeterminate;
    context.phase = SessionPhase::ResolvingCommit;
    context.native_code = 1117;
    context.operation_id = 42;
    context.generation = 9;
    context.disposition = CommitDisposition::CommitIndeterminate;
    outcome.error_context = context;

    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(outcome.error->domain, myrd::ErrorDomain::persistence);
    REQUIRE(outcome.error_context.has_value());
    REQUIRE_EQ(outcome.error_context->code,
               PersistenceErrorCode::CommitIndeterminate);
    REQUIRE_EQ(outcome.error_context->generation, 9ull);
}

TEST_CASE(public_error_context_requires_stable_correlation_fields) {
    PersistenceErrorContext context{};
    REQUIRE(!is_complete_error_context(context));

    context.operation = "checkpoint";
    context.operation_id = 42;
    context.phase = SessionPhase::Checkpointing;
    context.generation = 9;
    REQUIRE(!is_complete_error_context(context));

    context.image_uuid.bytes[0] = std::byte{0x01};
    REQUIRE(is_complete_error_context(context));

    context.phase = static_cast<SessionPhase>(0xFF);
    REQUIRE(!is_complete_error_context(context));
}

TEST_CASE(checkpoint_outcome_validation_rejects_split_error_context) {
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::CommitIndeterminate;
    outcome.generation = 9;
    outcome.error = myrd::Error{
        myrd::ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::CommitIndeterminate),
        L"checkpoint",
        L"manifest publish result is unknown",
        1117};
    REQUIRE(!is_valid_checkpoint_outcome(outcome));

    PersistenceErrorContext context{};
    context.operation = "checkpoint";
    context.code = PersistenceErrorCode::CommitIndeterminate;
    context.phase = SessionPhase::ResolvingCommit;
    context.native_code = 1117;
    context.operation_id = 42;
    context.image_uuid.bytes[0] = std::byte{0x01};
    context.generation = 9;
    context.disposition = CommitDisposition::NotCommitted;
    outcome.error_context = context;
    REQUIRE(!is_valid_checkpoint_outcome(outcome));

    outcome.error_context->disposition = CommitDisposition::CommitIndeterminate;
    REQUIRE(is_valid_checkpoint_outcome(outcome));

    outcome.error_context->operation = "compact";
    REQUIRE(!is_valid_checkpoint_outcome(outcome));
}

TEST_CASE(checkpoint_outcome_validation_rejects_post_commit_cancel_or_timeout) {
    for (const auto code : {
             PersistenceErrorCode::Cancelled,
             PersistenceErrorCode::DeadlineExceeded}) {
        CheckpointOutcome outcome{};
        outcome.disposition = CommitDisposition::CommittedNeedsReconciliation;
        outcome.generation = 9;
        outcome.error = myrd::Error{
            myrd::ErrorDomain::persistence,
            static_cast<std::uint32_t>(code),
            L"checkpoint",
            L"post-commit failure",
            0};
        outcome.error_context = complete_error_context(
            "checkpoint",
            code,
            SessionPhase::Reconciling,
            9,
            CommitDisposition::CommittedNeedsReconciliation);
        REQUIRE(!is_valid_checkpoint_outcome(outcome));
    }
}

TEST_CASE(checkpoint_reconciliation_may_preserve_first_indeterminate_error) {
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::CommittedNeedsReconciliation;
    outcome.generation = 9;
    outcome.error = myrd::Error{
        myrd::ErrorDomain::persistence,
        static_cast<std::uint32_t>(
            PersistenceErrorCode::CommitIndeterminate),
        L"checkpoint",
        L"first manifest flush failure preserved after resolution",
        1117};
    outcome.error_context = complete_error_context(
        "checkpoint",
        PersistenceErrorCode::CommitIndeterminate,
        SessionPhase::Reconciling,
        9,
        CommitDisposition::CommittedNeedsReconciliation);
    outcome.error_context->native_code = 1117;
    REQUIRE(is_valid_checkpoint_outcome(outcome));
}

TEST_CASE(checkpoint_outcome_validation_rejects_success_shaped_failure) {
    CheckpointOutcome outcome{};
    outcome.disposition = CommitDisposition::Committed;
    REQUIRE(!is_valid_checkpoint_outcome(outcome));

    outcome.manifest_candidate_verified = true;
    outcome.backing_view_installed = true;
    REQUIRE(is_valid_checkpoint_outcome(outcome));

    outcome.generation = kInvalidGeneration;
    REQUIRE(!is_valid_checkpoint_outcome(outcome));
}

TEST_CASE(checkpoint_outcome_validation_rejects_precommit_install_or_ack) {
    for (const auto disposition : {
             CommitDisposition::NotCommitted,
             CommitDisposition::CommitIndeterminate}) {
        const auto code = disposition == CommitDisposition::NotCommitted
            ? PersistenceErrorCode::IoError
            : PersistenceErrorCode::CommitIndeterminate;
        CheckpointOutcome outcome{};
        outcome.disposition = disposition;
        outcome.generation = 9;
        outcome.error = myrd::Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(code),
            L"checkpoint",
            L"precommit failure",
            0};
        outcome.error_context = complete_error_context(
            "checkpoint", code, SessionPhase::Checkpointing, 9, disposition);
        REQUIRE(is_valid_checkpoint_outcome(outcome));

        outcome.backing_view_installed = true;
        REQUIRE(!is_valid_checkpoint_outcome(outcome));
        outcome.backing_view_installed = false;
        outcome.acknowledged_blocks = 1;
        REQUIRE(!is_valid_checkpoint_outcome(outcome));
    }
}

TEST_CASE(error_context_phase_covers_all_session_phases) {
    // Each session phase should be storable in the error context.
    PersistenceErrorContext ctx{};
    ctx.phase = SessionPhase::Checkpointing;
    REQUIRE_EQ(ctx.phase, SessionPhase::Checkpointing);
    ctx.phase = SessionPhase::Compacting;
    REQUIRE_EQ(ctx.phase, SessionPhase::Compacting);
    ctx.phase = SessionPhase::ResolvingCommit;
    REQUIRE_EQ(ctx.phase, SessionPhase::ResolvingCommit);
    ctx.phase = SessionPhase::Reconciling;
    REQUIRE_EQ(ctx.phase, SessionPhase::Reconciling);
    ctx.phase = SessionPhase::PersistenceFaulted;
    REQUIRE_EQ(ctx.phase, SessionPhase::PersistenceFaulted);
}

// ============================================================================
// T13.7: Exhaustive enum coverage — every enum value has a non-empty to_string
// ============================================================================

TEST_CASE(all_persistence_error_codes_have_non_empty_to_string) {
    for (std::uint32_t i = 1; i <= 19; ++i) {
        auto code = static_cast<PersistenceErrorCode>(i);
        auto s = to_string(code);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownPersistenceErrorCode"));
    }
}

TEST_CASE(all_commit_dispositions_have_non_empty_to_string) {
    for (std::uint8_t i = 0; i <= 3; ++i) {
        auto d = static_cast<CommitDisposition>(i);
        auto s = to_string(d);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownCommitDisposition"));
    }
}

TEST_CASE(all_volume_barrier_kinds_have_non_empty_to_string) {
    for (std::uint8_t i = 0; i <= 1; ++i) {
        auto k = static_cast<VolumeBarrierKind>(i);
        auto s = to_string(k);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownVolumeBarrierKind"));
    }
}

TEST_CASE(all_session_phases_have_non_empty_to_string) {
    for (std::uint8_t i = 0; i <= 9; ++i) {
        auto p = static_cast<SessionPhase>(i);
        auto s = to_string(p);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownSessionPhase"));
    }
}

TEST_CASE(all_geometry_status_values_have_non_empty_to_string) {
    for (std::uint8_t i = 0; i <= 5; ++i) {
        auto gs = static_cast<GeometryStatus>(i);
        auto s = to_string(gs);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownGeometryStatus"));
    }
}

TEST_CASE(all_limits_status_values_have_non_empty_to_string) {
    for (std::uint8_t i = 0;
         i <= static_cast<std::uint8_t>(LimitsStatus::HiddenUnlimited);
         ++i) {
        auto ls = static_cast<LimitsStatus>(i);
        auto s = to_string(ls);
        REQUIRE(!s.empty());
        REQUIRE(s != string_view("UnknownLimitsStatus"));
    }
}

// ============================================================================
// T13.7: ABI-independent enum serialization — values are stable
// ============================================================================

TEST_CASE(error_code_values_are_abi_stable) {
    // Prove that enum values match the frozen spec table.
    // If any value changes, this test breaks — which is the intent.
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat), 1U);
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::FatalInternalInvariant), 19U);
}

TEST_CASE(disposition_values_are_abi_stable) {
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::NotCommitted), 0U);
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::CommittedNeedsReconciliation), 3U);
}

TEST_CASE(session_phase_values_are_abi_stable) {
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Closed), 0U);
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::PersistenceFaulted), 9U);
}

TEST_CASE(all_stable_enums_round_trip_explicit_wire_values) {
    constexpr std::array error_cases{
        WireCase{PersistenceErrorCode::InvalidFormat, 1U},
        WireCase{PersistenceErrorCode::UnsupportedVhdx, 2U},
        WireCase{PersistenceErrorCode::ManifestCorrupt, 3U},
        WireCase{PersistenceErrorCode::JournalCorrupt, 4U},
        WireCase{PersistenceErrorCode::BackingDataCorrupt, 5U},
        WireCase{PersistenceErrorCode::ImageInUse, 6U},
        WireCase{PersistenceErrorCode::OperationBusy, 7U},
        WireCase{PersistenceErrorCode::GenerationConflict, 8U},
        WireCase{PersistenceErrorCode::SnapshotBusy, 9U},
        WireCase{PersistenceErrorCode::SnapshotAborted, 10U},
        WireCase{PersistenceErrorCode::SnapshotResourceExhausted, 11U},
        WireCase{PersistenceErrorCode::CommitIndeterminate, 12U},
        WireCase{PersistenceErrorCode::DiskFull, 13U},
        WireCase{PersistenceErrorCode::AccessDenied, 14U},
        WireCase{PersistenceErrorCode::IoError, 15U},
        WireCase{PersistenceErrorCode::Cancelled, 16U},
        WireCase{PersistenceErrorCode::DeadlineExceeded, 17U},
        WireCase{PersistenceErrorCode::VerificationFailed, 18U},
        WireCase{PersistenceErrorCode::FatalInternalInvariant, 19U},
    };
    for (const auto& item : error_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_persistence_error_code(item.wire),
                   std::optional{item.value});
    }

    constexpr std::array disposition_cases{
        WireCase{CommitDisposition::NotCommitted, std::uint8_t{0}},
        WireCase{CommitDisposition::CommitIndeterminate, std::uint8_t{1}},
        WireCase{CommitDisposition::Committed, std::uint8_t{2}},
        WireCase{CommitDisposition::CommittedNeedsReconciliation,
                 std::uint8_t{3}},
    };
    for (const auto& item : disposition_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_commit_disposition(item.wire),
                   std::optional{item.value});
        REQUIRE_EQ(parse_commit_disposition(to_string(item.value)),
                   std::optional{item.value});
    }

    constexpr std::array barrier_cases{
        WireCase{VolumeBarrierKind::WindowsVolumeFlushed, std::uint8_t{0}},
        WireCase{VolumeBarrierKind::RawBlockTestNoFilesystemCache,
                 std::uint8_t{1}},
    };
    for (const auto& item : barrier_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_volume_barrier_kind(item.wire),
                   std::optional{item.value});
    }

    constexpr std::array phase_cases{
        WireCase{SessionPhase::Closed, std::uint8_t{0}},
        WireCase{SessionPhase::Opening, std::uint8_t{1}},
        WireCase{SessionPhase::Ready, std::uint8_t{2}},
        WireCase{SessionPhase::Checkpointing, std::uint8_t{3}},
        WireCase{SessionPhase::Compacting, std::uint8_t{4}},
        WireCase{SessionPhase::Verifying, std::uint8_t{5}},
        WireCase{SessionPhase::ResolvingCommit, std::uint8_t{6}},
        WireCase{SessionPhase::Reconciling, std::uint8_t{7}},
        WireCase{SessionPhase::Closing, std::uint8_t{8}},
        WireCase{SessionPhase::PersistenceFaulted, std::uint8_t{9}},
    };
    for (const auto& item : phase_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_session_phase(item.wire),
                   std::optional{item.value});
    }

    constexpr std::array geometry_cases{
        WireCase{GeometryStatus::Ok, std::uint8_t{0}},
        WireCase{GeometryStatus::VirtualBytesZero, std::uint8_t{1}},
        WireCase{GeometryStatus::LogicalSectorInvalid, std::uint8_t{2}},
        WireCase{GeometryStatus::PhysicalSectorInvalid, std::uint8_t{3}},
        WireCase{GeometryStatus::NotAlignedToLogicalSector, std::uint8_t{4}},
        WireCase{GeometryStatus::TooLargeForBlockCount, std::uint8_t{5}},
    };
    for (const auto& item : geometry_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_geometry_status(item.wire),
                   std::optional{item.value});
    }

    constexpr std::array limit_cases{
        WireCase{LimitsStatus::Ok, std::uint8_t{0}},
        WireCase{LimitsStatus::MaxVirtualBytesZero, std::uint8_t{1}},
        WireCase{LimitsStatus::MaxVirtualBytesUnlimited, std::uint8_t{2}},
        WireCase{LimitsStatus::MaxVirtualBytesNotSectorAligned,
                 std::uint8_t{3}},
        WireCase{LimitsStatus::MaxJournalCommittedBytesZero,
                 std::uint8_t{4}},
        WireCase{LimitsStatus::MaxJournalRecordsZero, std::uint8_t{5}},
        WireCase{LimitsStatus::SingleRecordExceedsJournalBytes,
                 std::uint8_t{6}},
        WireCase{LimitsStatus::JournalRecordsExceedCommittedBytes,
                 std::uint8_t{7}},
        WireCase{LimitsStatus::MaxIndexCacheBytesZero, std::uint8_t{8}},
        WireCase{LimitsStatus::MaxIndexCacheEntriesZero, std::uint8_t{9}},
        WireCase{LimitsStatus::CacheEntriesExceedIndexBytes,
                 std::uint8_t{10}},
        WireCase{LimitsStatus::MaxBatCacheBytesZero, std::uint8_t{11}},
        WireCase{LimitsStatus::MaxBatCacheEntriesZero, std::uint8_t{12}},
        WireCase{LimitsStatus::CacheEntriesExceedBatBytes,
                 std::uint8_t{13}},
        WireCase{LimitsStatus::MaxIoScratchBytesZero, std::uint8_t{14}},
        WireCase{LimitsStatus::IoScratchTooSmall, std::uint8_t{15}},
        WireCase{LimitsStatus::MaxVerifyBatchBytesZero, std::uint8_t{16}},
        WireCase{LimitsStatus::VerifyBatchTooSmall, std::uint8_t{17}},
        WireCase{LimitsStatus::ArithmeticOverflow, std::uint8_t{18}},
        WireCase{LimitsStatus::HiddenUnlimited, std::uint8_t{19}},
    };
    for (const auto& item : limit_cases) {
        REQUIRE_EQ(encode_wire(item.value), std::optional{item.wire});
        REQUIRE_EQ(decode_limits_status(item.wire),
                   std::optional{item.value});
    }
}

TEST_CASE(unknown_stable_enum_wire_values_fail_closed) {
    REQUIRE(!encode_wire(static_cast<PersistenceErrorCode>(0)).has_value());
    REQUIRE(!decode_persistence_error_code(0).has_value());
    REQUIRE(!encode_wire(static_cast<CommitDisposition>(0xFF)).has_value());
    REQUIRE(!decode_commit_disposition(0xFF).has_value());
    REQUIRE(!parse_commit_disposition("committed").has_value());
    REQUIRE(!encode_wire(static_cast<VolumeBarrierKind>(0xFF)).has_value());
    REQUIRE(!decode_volume_barrier_kind(0xFF).has_value());
    REQUIRE(!encode_wire(static_cast<SessionPhase>(0xFF)).has_value());
    REQUIRE(!decode_session_phase(0xFF).has_value());
    REQUIRE(!encode_wire(static_cast<GeometryStatus>(0xFF)).has_value());
    REQUIRE(!decode_geometry_status(0xFF).has_value());
    REQUIRE(!encode_wire(static_cast<LimitsStatus>(0xFF)).has_value());
    REQUIRE(!decode_limits_status(0xFF).has_value());
}

TEST_CASE(fault_phase_registry_has_frozen_schema_and_complete_records) {
    std::ifstream input{MYRD_TEST_PATH(MYRD_M3_FAULT_PHASE_REGISTRY_PATH),
                        std::ios::binary};
    REQUIRE(input.good());
    const std::string json{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    REQUIRE(json.find("\"schema\": \"m3-fault-phase-registry-v1\"") !=
            std::string::npos);

    const auto raw_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(json.data()), json.size()};
    const auto raw_file_hash = myrd::sha256_hex(raw_bytes);
    REQUIRE(raw_file_hash.ok());
    REQUIRE(raw_file_hash.value() ==
                "A47C3E147A7CD8F9A275F0604AB92C0FCB087AABF6334A23E05E6BA6832BA1D2" ||
            raw_file_hash.value() ==
                "4BB1F173B2B04316D7CBA7DAD7311C352202762ACA50AD9323C76ECE6A22C80B");

    std::string normalized_json;
    normalized_json.reserve(json.size());
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '\r') {
            REQUIRE(i + 1 < json.size());
            REQUIRE_EQ(json[i + 1], '\n');
            continue;
        }
        normalized_json.push_back(json[i]);
    }
    const auto normalized_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(normalized_json.data()),
        normalized_json.size()};
    const auto fixture_hash = myrd::sha256_hex(normalized_bytes);
    REQUIRE(fixture_hash.ok());
    REQUIRE_EQ(fixture_hash.value(),
               std::string{"A47C3E147A7CD8F9A275F0604AB92C0FCB087AABF6334A23E05E6BA6832BA1D2"});

    const auto phases = parse_fault_phase_fixtures(json);
    REQUIRE_EQ(phases.size(), static_cast<std::size_t>(59));
    const std::regex id_field_pattern{R"json("id"\s*:)json"};
    const auto id_field_count = static_cast<std::size_t>(std::distance(
        std::sregex_iterator{json.begin(), json.end(),
                             id_field_pattern},
        std::sregex_iterator{}));
    REQUIRE_EQ(id_field_count, phases.size());

    const std::regex stable_id_pattern{"^[a-z0-9]+([._][a-z0-9]+)*$"};
    const std::set<std::string> allowed_gates{
        "m3-v1", "m3-v2", "m3-v4"};
    const std::set<std::string> allowed_recovery{
        "NotApplicable",
        "OldCompleteGenerationOnly",
        "OldOrNewCompleteGeneration",
        "NewCompleteGenerationOnly"};
    std::set<std::string> ids;
    for (const auto& phase : phases) {
        REQUIRE(std::regex_match(phase.id, stable_id_pattern));
        REQUIRE(ids.insert(phase.id).second);
        REQUIRE(phase.boundary.size() >= 12);
        REQUIRE(allowed_gates.contains(phase.gate));
        REQUIRE(parse_commit_disposition(phase.state_disposition).has_value());
        REQUIRE(phase.error_disposition == "NotApplicable" ||
                parse_commit_disposition(phase.error_disposition).has_value());
        REQUIRE(allowed_recovery.contains(phase.kill_recovery));
        REQUIRE_EQ(phase.supports_kill,
                   phase.kill_recovery != "NotApplicable");
    }

    constexpr std::array required_ids{
        "snapshot.cut.before",
        "snapshot.cut.during_drain",
        "snapshot.cut.after_token_active",
        "cow.preimage.alloc.before",
        "cow.preimage.alloc.after",
        "cow.preimage.publish.before",
        "cow.preimage.publish.after",
        "journal.append.data.before",
        "journal.append.data.after",
        "journal.append.zero.before",
        "journal.append.zero.after",
        "journal.append.discard.before",
        "journal.append.discard.after",
        "journal.append.index_node.before",
        "journal.append.index_node.after",
        "journal.commit.append.before",
        "journal.commit.append.after",
        "journal.flush.before",
        "journal.flush.error",
        "journal.flush.after",
        "journal.readback.before",
        "journal.readback.corrupt",
        "journal.readback.after",
        "manifest.slot_write.before",
        "manifest.slot_write.short",
        "manifest.slot_write.torn",
        "manifest.slot_write.after",
        "manifest.flush.before",
        "manifest.flush.error",
        "manifest.flush.after",
        "manifest.readback.before",
        "manifest.readback.corrupt",
        "manifest.readback.after",
        "backing.view_install.before",
        "backing.view_install.after",
        "backing.ack.each.before",
        "backing.ack.each.after",
        "reconciliation.before",
        "reconciliation.after",
        "compaction.vhdx.create.before",
        "compaction.vhdx.create.after",
        "compaction.vhdx.write.before",
        "compaction.vhdx.write.after",
        "compaction.vhdx.flush.before",
        "compaction.vhdx.flush.after",
        "compaction.vhdx.readback.before",
        "compaction.vhdx.readback.after",
        "compaction.vhdx.validate.before",
        "compaction.vhdx.validate.after",
        "compaction.vhdx.windows_validate.before",
        "compaction.vhdx.windows_validate.after",
        "compaction.vhdx.rename.before",
        "compaction.vhdx.rename.after",
        "compaction.journal.create.before",
        "compaction.journal.create.after",
        "compaction.journal.flush.before",
        "compaction.journal.flush.after",
        "cleanup.old_file.delete.before",
        "cleanup.old_file.delete.after",
    };
    REQUIRE_EQ(required_ids.size(), phases.size());
    for (const auto id : required_ids) {
        REQUIRE(ids.contains(id));
    }

    std::string canonical;
    for (const auto& phase : phases) {
        canonical.append(phase.id).push_back('\n');
        canonical.append(phase.boundary).push_back('\n');
        canonical.append(phase.supports_short ? "1\n" : "0\n");
        canonical.append(phase.supports_torn ? "1\n" : "0\n");
        canonical.append(phase.supports_kill ? "1\n" : "0\n");
        canonical.append(phase.gate).push_back('\n');
        canonical.append(phase.state_disposition).push_back('\n');
        canonical.append(phase.error_disposition).push_back('\n');
        canonical.append(phase.kill_recovery).push_back('\n');
    }
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(canonical.data()),
        canonical.size()};
    const auto semantic_hash = myrd::sha256_hex(bytes);
    REQUIRE(semantic_hash.ok());
    REQUIRE_EQ(semantic_hash.value(),
               std::string{"D43DEA1EC3ECAEF5A4A1E4DB089E95CA1E9E0306DE3FD0481443D43D036590C7"});
}

}  // namespace
}  // namespace persistence
}  // namespace myrd
