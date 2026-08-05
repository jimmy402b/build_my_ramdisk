/// @file m3_persistence_types_tests.cpp
/// @brief T13.1–T13.7 — Frozen enum values, stable strings, geometry constants,
///        phase/session names, geometry validation, checked increment,
///        PersistenceLimits, CheckpointOutcome, VolumeBarrierKind validation,
///        error context, exhaustive enum coverage per SPEC and PLAN.

#include "test_framework.h"

#include "src/persistence/persistence_types.h"

#include <cstdint>
#include <string_view>

namespace myrd {
namespace persistence {
namespace {

using std::string_view;

// ============================================================================
// T13.1: Geometry constants (PLAN §1.4, frozen)
// ============================================================================

TEST_CASE(geometry_constants_are_frozen) {
    REQUIRE_EQ(kManifestSlotBytes, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kManifestFileBytes, static_cast<std::size_t>(8192));
    REQUIRE_EQ(kJournalAlignment, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kIndexNodeBytes, static_cast<std::size_t>(4096));
    REQUIRE_EQ(kMaxRelativeNameBytes, static_cast<std::size_t>(255));
    REQUIRE_EQ(kFormatMajor, static_cast<std::uint32_t>(1));
    REQUIRE_EQ(kFormatMinor, static_cast<std::uint32_t>(0));
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
    REQUIRE(checked_increment(val));
    REQUIRE_EQ(val, UINT64_MAX);
    REQUIRE(!checked_increment(val)); // second increment fails
    REQUIRE_EQ(val, UINT64_MAX);
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
    lim.max_virtual_bytes = UINT64_MAX;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 1; // will overflow with UINT64_MAX
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::ArithmeticOverflow);
}

TEST_CASE(validate_limits_accepts_valid_limits) {
    PersistenceLimits lim{};
    lim.max_virtual_bytes = 64ULL * 1024 * 1024;
    lim.max_journal_committed_bytes = 4096;
    lim.max_journal_records = 1;
    lim.max_index_cache_bytes = 4096;
    lim.max_index_cache_entries = 1;
    lim.max_bat_cache_bytes = 8;
    lim.max_bat_cache_entries = 1;
    lim.max_io_scratch_bytes = 4096;
    lim.max_verify_batch_bytes = 4096;
    REQUIRE(validate_limits(lim) == LimitsStatus::Ok);
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
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::CommitIndeterminate));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::Committed));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::CommitIndeterminate, CommitDisposition::CommittedNeedsReconciliation));
}

TEST_CASE(committed_is_terminal) {
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::NotCommitted));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::CommitIndeterminate));
    REQUIRE(is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::Committed));
    REQUIRE(!is_valid_disposition_transition(CommitDisposition::Committed, CommitDisposition::CommittedNeedsReconciliation));
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
}

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
    REQUIRE_EQ(ctx.phase, SessionPhase::Closed);
    REQUIRE_EQ(ctx.native_code, 0U);
    REQUIRE_EQ(ctx.operation_id, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(ctx.image_uuid_lo, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(ctx.image_uuid_hi, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(ctx.generation, kInvalidGeneration);
    REQUIRE_EQ(ctx.disposition, CommitDisposition::NotCommitted);
}

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
    ctx.phase = SessionPhase::Opening;
    ctx.native_code = 5;
    ctx.operation_id = 42;
    ctx.image_uuid_lo = 0xDEADBEEFCAFE0001ULL;
    ctx.image_uuid_hi = 0x1234567890ABCDEFULL;
    ctx.generation = 3;
    ctx.disposition = CommitDisposition::CommitIndeterminate;

    REQUIRE_EQ(ctx.operation, string_view("recovery"));
    REQUIRE_EQ(ctx.phase, SessionPhase::Opening);
    REQUIRE_EQ(ctx.native_code, 5U);
    REQUIRE_EQ(ctx.operation_id, static_cast<std::uint64_t>(42));
    REQUIRE_EQ(ctx.image_uuid_lo, 0xDEADBEEFCAFE0001ULL);
    REQUIRE_EQ(ctx.image_uuid_hi, 0x1234567890ABCDEFULL);
    REQUIRE_EQ(ctx.generation, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(ctx.disposition, CommitDisposition::CommitIndeterminate);
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
    for (std::uint8_t i = 0; i <= 13; ++i) {
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

}  // namespace
}  // namespace persistence
}  // namespace myrd
