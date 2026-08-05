#pragma once

/// @file persistence_types.h
/// @brief M3 persistence engine public constants, types and version contract.
///
/// This header belongs to the myrd_persistence library and must not depend
/// on driver, proxy, M1, M2 or platform lifecycle types.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace myrd {
namespace persistence {

// ---- Format version --------------------------------------------------------
inline constexpr std::uint32_t kFormatMajor = 1;
inline constexpr std::uint32_t kFormatMinor = 0;

// ---- Fixed layout constants (frozen by SPEC) -------------------------------
inline constexpr std::size_t kManifestSlotBytes   = 4096;
inline constexpr std::size_t kManifestFileBytes    = 8192;
inline constexpr std::size_t kJournalAlignment     = 4096;
inline constexpr std::size_t kIndexNodeBytes       = 4096;
inline constexpr std::size_t kMaxRelativeNameBytes = 255;
inline constexpr std::uint32_t kVhdxDataBlockBytes = 2U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxIndexDepth      = 8;

// ---- Generation / epoch helpers --------------------------------------------
// generation and epoch never wrap; checked-increment callers (Task 13) return
// a fatal error on overflow.

inline constexpr std::uint64_t kInvalidGeneration = UINT64_MAX;

// ---- Persistence error codes (SPEC §14, frozen numeric values) -------------
enum class PersistenceErrorCode : std::uint32_t {
    InvalidFormat              = 1,
    UnsupportedVhdx            = 2,
    ManifestCorrupt            = 3,
    JournalCorrupt             = 4,
    BackingDataCorrupt         = 5,
    ImageInUse                 = 6,
    OperationBusy              = 7,
    GenerationConflict         = 8,
    SnapshotBusy               = 9,
    SnapshotAborted            = 10,
    SnapshotResourceExhausted  = 11,
    CommitIndeterminate        = 12,
    DiskFull                   = 13,
    AccessDenied               = 14,
    IoError                    = 15,
    Cancelled                  = 16,
    DeadlineExceeded           = 17,
    VerificationFailed         = 18,
    FatalInternalInvariant     = 19,
};

constexpr std::string_view to_string(PersistenceErrorCode code) noexcept {
    switch (code) {
    case PersistenceErrorCode::InvalidFormat:
        return "InvalidFormat";
    case PersistenceErrorCode::UnsupportedVhdx:
        return "UnsupportedVhdx";
    case PersistenceErrorCode::ManifestCorrupt:
        return "ManifestCorrupt";
    case PersistenceErrorCode::JournalCorrupt:
        return "JournalCorrupt";
    case PersistenceErrorCode::BackingDataCorrupt:
        return "BackingDataCorrupt";
    case PersistenceErrorCode::ImageInUse:
        return "ImageInUse";
    case PersistenceErrorCode::OperationBusy:
        return "OperationBusy";
    case PersistenceErrorCode::GenerationConflict:
        return "GenerationConflict";
    case PersistenceErrorCode::SnapshotBusy:
        return "SnapshotBusy";
    case PersistenceErrorCode::SnapshotAborted:
        return "SnapshotAborted";
    case PersistenceErrorCode::SnapshotResourceExhausted:
        return "SnapshotResourceExhausted";
    case PersistenceErrorCode::CommitIndeterminate:
        return "CommitIndeterminate";
    case PersistenceErrorCode::DiskFull:
        return "DiskFull";
    case PersistenceErrorCode::AccessDenied:
        return "AccessDenied";
    case PersistenceErrorCode::IoError:
        return "IoError";
    case PersistenceErrorCode::Cancelled:
        return "Cancelled";
    case PersistenceErrorCode::DeadlineExceeded:
        return "DeadlineExceeded";
    case PersistenceErrorCode::VerificationFailed:
        return "VerificationFailed";
    case PersistenceErrorCode::FatalInternalInvariant:
        return "FatalInternalInvariant";
    }
    return "UnknownPersistenceErrorCode";
}

// ---- Commit disposition (PLAN §1.5, frozen) --------------------------------
enum class CommitDisposition : std::uint8_t {
    NotCommitted                  = 0,
    CommitIndeterminate           = 1,
    Committed                     = 2,
    CommittedNeedsReconciliation  = 3,
};

constexpr std::string_view to_string(CommitDisposition disposition) noexcept {
    switch (disposition) {
    case CommitDisposition::NotCommitted:
        return "NotCommitted";
    case CommitDisposition::CommitIndeterminate:
        return "CommitIndeterminate";
    case CommitDisposition::Committed:
        return "Committed";
    case CommitDisposition::CommittedNeedsReconciliation:
        return "CommittedNeedsReconciliation";
    }
    return "UnknownCommitDisposition";
}

// ---- Volume barrier kind (PLAN §1.5, frozen) -------------------------------
enum class VolumeBarrierKind : std::uint8_t {
    WindowsVolumeFlushed            = 0,
    RawBlockTestNoFilesystemCache   = 1,
};

constexpr std::string_view to_string(VolumeBarrierKind kind) noexcept {
    switch (kind) {
    case VolumeBarrierKind::WindowsVolumeFlushed:
        return "WindowsVolumeFlushed";
    case VolumeBarrierKind::RawBlockTestNoFilesystemCache:
        return "RawBlockTestNoFilesystemCache";
    }
    return "UnknownVolumeBarrierKind";
}

// ---- Session phase (SPEC §12, frozen) --------------------------------------
enum class SessionPhase : std::uint8_t {
    Closed              = 0,
    Opening             = 1,
    Ready               = 2,
    Checkpointing       = 3,
    Compacting          = 4,
    Verifying           = 5,
    ResolvingCommit     = 6,
    Reconciling         = 7,
    Closing             = 8,
    PersistenceFaulted  = 9,
};

constexpr std::string_view to_string(SessionPhase phase) noexcept {
    switch (phase) {
    case SessionPhase::Closed:
        return "Closed";
    case SessionPhase::Opening:
        return "Opening";
    case SessionPhase::Ready:
        return "Ready";
    case SessionPhase::Checkpointing:
        return "Checkpointing";
    case SessionPhase::Compacting:
        return "Compacting";
    case SessionPhase::Verifying:
        return "Verifying";
    case SessionPhase::ResolvingCommit:
        return "ResolvingCommit";
    case SessionPhase::Reconciling:
        return "Reconciling";
    case SessionPhase::Closing:
        return "Closing";
    case SessionPhase::PersistenceFaulted:
        return "PersistenceFaulted";
    }
    return "UnknownSessionPhase";
}

}  // namespace persistence
}  // namespace myrd
