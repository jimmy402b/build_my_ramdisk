/// @file m3_persistence_types_tests.cpp
/// @brief T13.1 [M] — Frozen enum values, stable strings, geometry constants,
///        phase/session names per SPEC and PLAN §1.4–1.5.

#include "test_framework.h"

#include "src/persistence/persistence_types.h"

#include <string_view>

namespace myrd {
namespace persistence {
namespace {

using std::string_view;

// ============================================================================
// Geometry constants (PLAN §1.4, frozen)
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
// PersistenceErrorCode — SPEC §14 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(persistence_error_code_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
               static_cast<std::uint32_t>(1));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::UnsupportedVhdx),
               static_cast<std::uint32_t>(2));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::ManifestCorrupt),
               static_cast<std::uint32_t>(3));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::JournalCorrupt),
               static_cast<std::uint32_t>(4));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::BackingDataCorrupt),
        static_cast<std::uint32_t>(5));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::ImageInUse),
               static_cast<std::uint32_t>(6));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy),
               static_cast<std::uint32_t>(7));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::GenerationConflict),
        static_cast<std::uint32_t>(8));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotBusy),
               static_cast<std::uint32_t>(9));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotAborted),
               static_cast<std::uint32_t>(10));
    REQUIRE_EQ(static_cast<std::uint32_t>(
                  PersistenceErrorCode::SnapshotResourceExhausted),
               static_cast<std::uint32_t>(11));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::CommitIndeterminate),
        static_cast<std::uint32_t>(12));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::DiskFull),
               static_cast<std::uint32_t>(13));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::AccessDenied),
               static_cast<std::uint32_t>(14));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
               static_cast<std::uint32_t>(15));
    REQUIRE_EQ(static_cast<std::uint32_t>(PersistenceErrorCode::Cancelled),
               static_cast<std::uint32_t>(16));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::DeadlineExceeded),
        static_cast<std::uint32_t>(17));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
        static_cast<std::uint32_t>(18));
    REQUIRE_EQ(
        static_cast<std::uint32_t>(PersistenceErrorCode::FatalInternalInvariant),
        static_cast<std::uint32_t>(19));
}

TEST_CASE(persistence_error_code_stable_strings) {
    REQUIRE_EQ(to_string(PersistenceErrorCode::InvalidFormat),
               std::string_view("InvalidFormat"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::UnsupportedVhdx),
               std::string_view("UnsupportedVhdx"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::ManifestCorrupt),
               std::string_view("ManifestCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::JournalCorrupt),
               std::string_view("JournalCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::BackingDataCorrupt),
               std::string_view("BackingDataCorrupt"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::ImageInUse),
               std::string_view("ImageInUse"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::OperationBusy),
               std::string_view("OperationBusy"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::GenerationConflict),
               std::string_view("GenerationConflict"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotBusy),
               std::string_view("SnapshotBusy"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotAborted),
               std::string_view("SnapshotAborted"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::SnapshotResourceExhausted),
               std::string_view("SnapshotResourceExhausted"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::CommitIndeterminate),
               std::string_view("CommitIndeterminate"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::DiskFull),
               std::string_view("DiskFull"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::AccessDenied),
               std::string_view("AccessDenied"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::IoError),
               std::string_view("IoError"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::Cancelled),
               std::string_view("Cancelled"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::DeadlineExceeded),
               std::string_view("DeadlineExceeded"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::VerificationFailed),
               std::string_view("VerificationFailed"));
    REQUIRE_EQ(to_string(PersistenceErrorCode::FatalInternalInvariant),
               std::string_view("FatalInternalInvariant"));
}

// ============================================================================
// CommitDisposition — PLAN §1.5 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(commit_disposition_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::NotCommitted),
               static_cast<std::uint8_t>(0));
    REQUIRE_EQ(
        static_cast<std::uint8_t>(CommitDisposition::CommitIndeterminate),
        static_cast<std::uint8_t>(1));
    REQUIRE_EQ(static_cast<std::uint8_t>(CommitDisposition::Committed),
               static_cast<std::uint8_t>(2));
    REQUIRE_EQ(
        static_cast<std::uint8_t>(CommitDisposition::CommittedNeedsReconciliation),
        static_cast<std::uint8_t>(3));
}

TEST_CASE(commit_disposition_stable_strings) {
    REQUIRE_EQ(to_string(CommitDisposition::NotCommitted),
               std::string_view("NotCommitted"));
    REQUIRE_EQ(to_string(CommitDisposition::CommitIndeterminate),
               std::string_view("CommitIndeterminate"));
    REQUIRE_EQ(to_string(CommitDisposition::Committed),
               std::string_view("Committed"));
    REQUIRE_EQ(to_string(CommitDisposition::CommittedNeedsReconciliation),
               std::string_view("CommittedNeedsReconciliation"));
}

// ============================================================================
// VolumeBarrierKind — PLAN §1.5 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(volume_barrier_kind_fixed_values) {
    REQUIRE_EQ(
        static_cast<std::uint8_t>(VolumeBarrierKind::WindowsVolumeFlushed),
        static_cast<std::uint8_t>(0));
    REQUIRE_EQ(static_cast<std::uint8_t>(
                  VolumeBarrierKind::RawBlockTestNoFilesystemCache),
               static_cast<std::uint8_t>(1));
}

TEST_CASE(volume_barrier_kind_stable_strings) {
    REQUIRE_EQ(to_string(VolumeBarrierKind::WindowsVolumeFlushed),
               std::string_view("WindowsVolumeFlushed"));
    REQUIRE_EQ(to_string(VolumeBarrierKind::RawBlockTestNoFilesystemCache),
               std::string_view("RawBlockTestNoFilesystemCache"));
}

// ============================================================================
// SessionPhase — SPEC §12 (frozen numeric values + stable strings)
// ============================================================================

TEST_CASE(session_phase_fixed_values) {
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Closed),
               static_cast<std::uint8_t>(0));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Opening),
               static_cast<std::uint8_t>(1));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Ready),
               static_cast<std::uint8_t>(2));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Checkpointing),
               static_cast<std::uint8_t>(3));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Compacting),
               static_cast<std::uint8_t>(4));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Verifying),
               static_cast<std::uint8_t>(5));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::ResolvingCommit),
               static_cast<std::uint8_t>(6));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Reconciling),
               static_cast<std::uint8_t>(7));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::Closing),
               static_cast<std::uint8_t>(8));
    REQUIRE_EQ(static_cast<std::uint8_t>(SessionPhase::PersistenceFaulted),
               static_cast<std::uint8_t>(9));
}

TEST_CASE(session_phase_stable_strings) {
    REQUIRE_EQ(to_string(SessionPhase::Closed), std::string_view("Closed"));
    REQUIRE_EQ(to_string(SessionPhase::Opening), std::string_view("Opening"));
    REQUIRE_EQ(to_string(SessionPhase::Ready), std::string_view("Ready"));
    REQUIRE_EQ(to_string(SessionPhase::Checkpointing),
               std::string_view("Checkpointing"));
    REQUIRE_EQ(to_string(SessionPhase::Compacting),
               std::string_view("Compacting"));
    REQUIRE_EQ(to_string(SessionPhase::Verifying),
               std::string_view("Verifying"));
    REQUIRE_EQ(to_string(SessionPhase::ResolvingCommit),
               std::string_view("ResolvingCommit"));
    REQUIRE_EQ(to_string(SessionPhase::Reconciling),
               std::string_view("Reconciling"));
    REQUIRE_EQ(to_string(SessionPhase::Closing), std::string_view("Closing"));
    REQUIRE_EQ(to_string(SessionPhase::PersistenceFaulted),
               std::string_view("PersistenceFaulted"));
}

}  // namespace
}  // namespace persistence
}  // namespace myrd
