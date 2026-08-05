#pragma once

#include "src/common/error.h"

/// @file persistence_types.h
/// @brief M3 persistence engine public constants, types and version contract.
///
/// This header belongs to the myrd_persistence library and must not depend
/// on driver, proxy, M1, M2 or platform lifecycle types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace myrd {
namespace persistence {

// ============================================================================
// Format version (PLAN §1.4, frozen)
// ============================================================================

inline constexpr std::uint32_t kM3FormatMajor = 1;
inline constexpr std::uint32_t kM3FormatMinor = 0;
// Compatibility aliases for the Task 10 boundary probe. New persistence
// codecs use the M3-qualified names above.
inline constexpr std::uint32_t kFormatMajor = kM3FormatMajor;
inline constexpr std::uint32_t kFormatMinor = kM3FormatMinor;

// ============================================================================
// Fixed layout constants (frozen by SPEC)
// ============================================================================

inline constexpr std::size_t kManifestSlotBytes   = 4096;
inline constexpr std::size_t kManifestFileBytes    = 8192;
inline constexpr std::size_t kJournalAlignment     = 4096;
inline constexpr std::size_t kIndexNodeBytes       = 4096;
inline constexpr std::size_t kMaxRelativeNameBytes = 255;
inline constexpr std::uint32_t kVhdxDataBlockBytes = 2U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxIndexDepth      = 8;

// ============================================================================
// Sector / block size constants (frozen by SPEC §8.6)
// ============================================================================

inline constexpr std::uint32_t kLogicalSectorSize512  = 512;
inline constexpr std::uint32_t kLogicalSectorSize4096 = 4096;
inline constexpr std::uint32_t kPhysicalSectorSize    = 4096;
inline constexpr std::uint64_t kM2BlockSize           = 64ULL * 1024ULL;
inline constexpr std::uint64_t kMinimumDataRecordBytes =
    kM2BlockSize + kJournalAlignment;

// ============================================================================
// Generation / epoch helpers
// ============================================================================
// generation and epoch never wrap; checked-increment returns false on overflow.

inline constexpr std::uint64_t kInvalidGeneration = UINT64_MAX;

struct ImageUuid final {
    std::array<std::byte, 16> bytes{};

    [[nodiscard]] constexpr bool is_nil() const noexcept {
        for (const auto value : bytes) {
            if (value != std::byte{}) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const ImageUuid&) const noexcept = default;
};

/// @return true if incremented, false on overflow (value unchanged).
inline bool checked_increment(std::uint64_t& value) noexcept {
    if (value == UINT64_MAX) return false;
    ++value;
    return true;
}

// ============================================================================
// Image geometry (SPEC §8.3, §8.6, frozen)
// ============================================================================

struct ImageGeometry {
    std::uint64_t virtual_bytes{0};
    std::uint32_t logical_sector_size{0};
    std::uint32_t physical_sector_size{0};
};

enum class GeometryStatus : std::uint8_t {
    Ok = 0,
    VirtualBytesZero,
    LogicalSectorInvalid,
    PhysicalSectorInvalid,
    NotAlignedToLogicalSector,
    TooLargeForBlockCount,
};

constexpr std::string_view to_string(GeometryStatus status) noexcept {
    switch (status) {
    case GeometryStatus::Ok:                       return "Ok";
    case GeometryStatus::VirtualBytesZero:         return "VirtualBytesZero";
    case GeometryStatus::LogicalSectorInvalid:     return "LogicalSectorInvalid";
    case GeometryStatus::PhysicalSectorInvalid:    return "PhysicalSectorInvalid";
    case GeometryStatus::NotAlignedToLogicalSector:return "NotAlignedToLogicalSector";
    case GeometryStatus::TooLargeForBlockCount:    return "TooLargeForBlockCount";
    }
    return "UnknownGeometryStatus";
}

constexpr std::optional<std::uint8_t> encode_wire(
    GeometryStatus status) noexcept {
    switch (status) {
    case GeometryStatus::Ok:                        return std::uint8_t{0};
    case GeometryStatus::VirtualBytesZero:          return std::uint8_t{1};
    case GeometryStatus::LogicalSectorInvalid:      return std::uint8_t{2};
    case GeometryStatus::PhysicalSectorInvalid:     return std::uint8_t{3};
    case GeometryStatus::NotAlignedToLogicalSector: return std::uint8_t{4};
    case GeometryStatus::TooLargeForBlockCount:     return std::uint8_t{5};
    }
    return std::nullopt;
}

constexpr std::optional<GeometryStatus> decode_geometry_status(
    std::uint8_t wire) noexcept {
    switch (wire) {
    case 0: return GeometryStatus::Ok;
    case 1: return GeometryStatus::VirtualBytesZero;
    case 2: return GeometryStatus::LogicalSectorInvalid;
    case 3: return GeometryStatus::PhysicalSectorInvalid;
    case 4: return GeometryStatus::NotAlignedToLogicalSector;
    case 5: return GeometryStatus::TooLargeForBlockCount;
    default: return std::nullopt;
    }
}

/// Validate image geometry per SPEC frozen profile.
/// logical_sector_size must be 512 or 4096; physical_sector_size must be 4096.
/// virtual_bytes must be non-zero and a multiple of logical_sector_size.
/// A final partial M2 block is valid and is handled by the block store.
inline GeometryStatus validate_geometry(const ImageGeometry& g) noexcept {
    if (g.virtual_bytes == 0) return GeometryStatus::VirtualBytesZero;
    if (g.logical_sector_size != kLogicalSectorSize512 &&
        g.logical_sector_size != kLogicalSectorSize4096)
        return GeometryStatus::LogicalSectorInvalid;
    if (g.physical_sector_size != kPhysicalSectorSize)
        return GeometryStatus::PhysicalSectorInvalid;
    if (g.virtual_bytes % g.logical_sector_size != 0)
        return GeometryStatus::NotAlignedToLogicalSector;
    return GeometryStatus::Ok;
}

// ============================================================================
// PersistenceLimits (PLAN §1.4, frozen)
// ============================================================================

struct PersistenceLimits {
    std::uint64_t max_virtual_bytes{0};
    std::uint64_t max_journal_committed_bytes{0};
    std::uint64_t max_journal_records{0};
    std::uint64_t max_index_cache_bytes{0};
    std::uint64_t max_index_cache_entries{0};
    std::uint64_t max_bat_cache_bytes{0};
    std::uint64_t max_bat_cache_entries{0};
    std::uint64_t max_io_scratch_bytes{0};
    std::uint64_t max_verify_batch_bytes{0};
};

enum class LimitsStatus : std::uint8_t {
    Ok = 0,
    MaxVirtualBytesZero,
    MaxVirtualBytesUnlimited,
    MaxVirtualBytesNotSectorAligned,
    MaxJournalCommittedBytesZero,
    MaxJournalRecordsZero,
    SingleRecordExceedsJournalBytes,
    JournalRecordsExceedCommittedBytes,
    MaxIndexCacheBytesZero,
    MaxIndexCacheEntriesZero,
    CacheEntriesExceedIndexBytes,
    MaxBatCacheBytesZero,
    MaxBatCacheEntriesZero,
    CacheEntriesExceedBatBytes,
    MaxIoScratchBytesZero,
    IoScratchTooSmall,
    MaxVerifyBatchBytesZero,
    VerifyBatchTooSmall,
    ArithmeticOverflow,
};

constexpr std::string_view to_string(LimitsStatus status) noexcept {
    switch (status) {
    case LimitsStatus::Ok:                             return "Ok";
    case LimitsStatus::MaxVirtualBytesZero:            return "MaxVirtualBytesZero";
    case LimitsStatus::MaxVirtualBytesUnlimited:       return "MaxVirtualBytesUnlimited";
    case LimitsStatus::MaxVirtualBytesNotSectorAligned:return "MaxVirtualBytesNotSectorAligned";
    case LimitsStatus::MaxJournalCommittedBytesZero:   return "MaxJournalCommittedBytesZero";
    case LimitsStatus::MaxJournalRecordsZero:          return "MaxJournalRecordsZero";
    case LimitsStatus::SingleRecordExceedsJournalBytes:return "SingleRecordExceedsJournalBytes";
    case LimitsStatus::JournalRecordsExceedCommittedBytes:return "JournalRecordsExceedCommittedBytes";
    case LimitsStatus::MaxIndexCacheBytesZero:         return "MaxIndexCacheBytesZero";
    case LimitsStatus::MaxIndexCacheEntriesZero:       return "MaxIndexCacheEntriesZero";
    case LimitsStatus::CacheEntriesExceedIndexBytes:   return "CacheEntriesExceedIndexBytes";
    case LimitsStatus::MaxBatCacheBytesZero:           return "MaxBatCacheBytesZero";
    case LimitsStatus::MaxBatCacheEntriesZero:         return "MaxBatCacheEntriesZero";
    case LimitsStatus::CacheEntriesExceedBatBytes:     return "CacheEntriesExceedBatBytes";
    case LimitsStatus::MaxIoScratchBytesZero:          return "MaxIoScratchBytesZero";
    case LimitsStatus::IoScratchTooSmall:              return "IoScratchTooSmall";
    case LimitsStatus::MaxVerifyBatchBytesZero:        return "MaxVerifyBatchBytesZero";
    case LimitsStatus::VerifyBatchTooSmall:            return "VerifyBatchTooSmall";
    case LimitsStatus::ArithmeticOverflow:             return "ArithmeticOverflow";
    }
    return "UnknownLimitsStatus";
}

constexpr std::optional<std::uint8_t> encode_wire(
    LimitsStatus status) noexcept {
    switch (status) {
    case LimitsStatus::Ok:                              return std::uint8_t{0};
    case LimitsStatus::MaxVirtualBytesZero:             return std::uint8_t{1};
    case LimitsStatus::MaxVirtualBytesUnlimited:        return std::uint8_t{2};
    case LimitsStatus::MaxVirtualBytesNotSectorAligned: return std::uint8_t{3};
    case LimitsStatus::MaxJournalCommittedBytesZero:    return std::uint8_t{4};
    case LimitsStatus::MaxJournalRecordsZero:           return std::uint8_t{5};
    case LimitsStatus::SingleRecordExceedsJournalBytes: return std::uint8_t{6};
    case LimitsStatus::JournalRecordsExceedCommittedBytes:
        return std::uint8_t{7};
    case LimitsStatus::MaxIndexCacheBytesZero:          return std::uint8_t{8};
    case LimitsStatus::MaxIndexCacheEntriesZero:        return std::uint8_t{9};
    case LimitsStatus::CacheEntriesExceedIndexBytes:    return std::uint8_t{10};
    case LimitsStatus::MaxBatCacheBytesZero:            return std::uint8_t{11};
    case LimitsStatus::MaxBatCacheEntriesZero:          return std::uint8_t{12};
    case LimitsStatus::CacheEntriesExceedBatBytes:      return std::uint8_t{13};
    case LimitsStatus::MaxIoScratchBytesZero:           return std::uint8_t{14};
    case LimitsStatus::IoScratchTooSmall:               return std::uint8_t{15};
    case LimitsStatus::MaxVerifyBatchBytesZero:         return std::uint8_t{16};
    case LimitsStatus::VerifyBatchTooSmall:             return std::uint8_t{17};
    case LimitsStatus::ArithmeticOverflow:              return std::uint8_t{18};
    }
    return std::nullopt;
}

constexpr std::optional<LimitsStatus> decode_limits_status(
    std::uint8_t wire) noexcept {
    switch (wire) {
    case 0:  return LimitsStatus::Ok;
    case 1:  return LimitsStatus::MaxVirtualBytesZero;
    case 2:  return LimitsStatus::MaxVirtualBytesUnlimited;
    case 3:  return LimitsStatus::MaxVirtualBytesNotSectorAligned;
    case 4:  return LimitsStatus::MaxJournalCommittedBytesZero;
    case 5:  return LimitsStatus::MaxJournalRecordsZero;
    case 6:  return LimitsStatus::SingleRecordExceedsJournalBytes;
    case 7:  return LimitsStatus::JournalRecordsExceedCommittedBytes;
    case 8:  return LimitsStatus::MaxIndexCacheBytesZero;
    case 9:  return LimitsStatus::MaxIndexCacheEntriesZero;
    case 10: return LimitsStatus::CacheEntriesExceedIndexBytes;
    case 11: return LimitsStatus::MaxBatCacheBytesZero;
    case 12: return LimitsStatus::MaxBatCacheEntriesZero;
    case 13: return LimitsStatus::CacheEntriesExceedBatBytes;
    case 14: return LimitsStatus::MaxIoScratchBytesZero;
    case 15: return LimitsStatus::IoScratchTooSmall;
    case 16: return LimitsStatus::MaxVerifyBatchBytesZero;
    case 17: return LimitsStatus::VerifyBatchTooSmall;
    case 18: return LimitsStatus::ArithmeticOverflow;
    default: return std::nullopt;
    }
}

/// Validate caller-supplied PersistenceLimits.
/// Every field that is logically required must be > 0.  Cache entries must
/// not exceed the byte budget (each entry costs at least kIndexNodeBytes or
/// an equivalent minimum).  Arithmetic overflow in any derived bound is
/// rejected.
inline LimitsStatus validate_limits(const PersistenceLimits& lim) noexcept {
    if (lim.max_virtual_bytes == 0)          return LimitsStatus::MaxVirtualBytesZero;
    if (lim.max_virtual_bytes == UINT64_MAX)
        return LimitsStatus::MaxVirtualBytesUnlimited;
    if (lim.max_virtual_bytes % kLogicalSectorSize512 != 0)
        return LimitsStatus::MaxVirtualBytesNotSectorAligned;
    if (lim.max_journal_committed_bytes == 0)return LimitsStatus::MaxJournalCommittedBytesZero;
    if (lim.max_journal_records == 0)        return LimitsStatus::MaxJournalRecordsZero;
    if (lim.max_index_cache_bytes == 0)      return LimitsStatus::MaxIndexCacheBytesZero;
    if (lim.max_index_cache_entries == 0)    return LimitsStatus::MaxIndexCacheEntriesZero;
    if (lim.max_bat_cache_bytes == 0)        return LimitsStatus::MaxBatCacheBytesZero;
    if (lim.max_bat_cache_entries == 0)      return LimitsStatus::MaxBatCacheEntriesZero;
    if (lim.max_io_scratch_bytes == 0)       return LimitsStatus::MaxIoScratchBytesZero;
    if (lim.max_verify_batch_bytes == 0)     return LimitsStatus::MaxVerifyBatchBytesZero;

    // Each index cache entry needs at least kIndexNodeBytes.
    if (lim.max_index_cache_entries > lim.max_index_cache_bytes / kIndexNodeBytes)
        return LimitsStatus::CacheEntriesExceedIndexBytes;

    // Each BAT cache entry needs at least sizeof(uint64_t) (a single BAT entry).
    // Use a conservative minimum of 8 bytes per entry.
    if (lim.max_bat_cache_entries > lim.max_bat_cache_bytes / 8)
        return LimitsStatus::CacheEntriesExceedBatBytes;

    // A single journal record must fit in committed bytes (at least header + payload).
    if (lim.max_journal_committed_bytes < kMinimumDataRecordBytes)
        return LimitsStatus::SingleRecordExceedsJournalBytes;
    if (lim.max_journal_records >
        lim.max_journal_committed_bytes / kJournalAlignment)
        return LimitsStatus::JournalRecordsExceedCommittedBytes;

    if (lim.max_io_scratch_bytes < kM2BlockSize)
        return LimitsStatus::IoScratchTooSmall;
    if (lim.max_verify_batch_bytes < kM2BlockSize)
        return LimitsStatus::VerifyBatchTooSmall;

    // Check for arithmetic overflow on combined limits.
    // If max_virtual_bytes + scratch + verify overflows, reject.
    if (lim.max_virtual_bytes > UINT64_MAX - lim.max_io_scratch_bytes)
        return LimitsStatus::ArithmeticOverflow;
    if (lim.max_virtual_bytes + lim.max_io_scratch_bytes >
        UINT64_MAX - lim.max_verify_batch_bytes)
        return LimitsStatus::ArithmeticOverflow;

    return LimitsStatus::Ok;
}

// ============================================================================
// Persistence error codes (SPEC §14, frozen numeric values)
// ============================================================================

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
    case PersistenceErrorCode::InvalidFormat:              return "InvalidFormat";
    case PersistenceErrorCode::UnsupportedVhdx:            return "UnsupportedVhdx";
    case PersistenceErrorCode::ManifestCorrupt:            return "ManifestCorrupt";
    case PersistenceErrorCode::JournalCorrupt:             return "JournalCorrupt";
    case PersistenceErrorCode::BackingDataCorrupt:         return "BackingDataCorrupt";
    case PersistenceErrorCode::ImageInUse:                 return "ImageInUse";
    case PersistenceErrorCode::OperationBusy:              return "OperationBusy";
    case PersistenceErrorCode::GenerationConflict:         return "GenerationConflict";
    case PersistenceErrorCode::SnapshotBusy:               return "SnapshotBusy";
    case PersistenceErrorCode::SnapshotAborted:            return "SnapshotAborted";
    case PersistenceErrorCode::SnapshotResourceExhausted:  return "SnapshotResourceExhausted";
    case PersistenceErrorCode::CommitIndeterminate:        return "CommitIndeterminate";
    case PersistenceErrorCode::DiskFull:                   return "DiskFull";
    case PersistenceErrorCode::AccessDenied:               return "AccessDenied";
    case PersistenceErrorCode::IoError:                    return "IoError";
    case PersistenceErrorCode::Cancelled:                  return "Cancelled";
    case PersistenceErrorCode::DeadlineExceeded:           return "DeadlineExceeded";
    case PersistenceErrorCode::VerificationFailed:         return "VerificationFailed";
    case PersistenceErrorCode::FatalInternalInvariant:     return "FatalInternalInvariant";
    }
    return "UnknownPersistenceErrorCode";
}

constexpr std::optional<std::uint32_t> encode_wire(
    PersistenceErrorCode code) noexcept {
    switch (code) {
    case PersistenceErrorCode::InvalidFormat:             return 1U;
    case PersistenceErrorCode::UnsupportedVhdx:           return 2U;
    case PersistenceErrorCode::ManifestCorrupt:           return 3U;
    case PersistenceErrorCode::JournalCorrupt:            return 4U;
    case PersistenceErrorCode::BackingDataCorrupt:        return 5U;
    case PersistenceErrorCode::ImageInUse:                return 6U;
    case PersistenceErrorCode::OperationBusy:             return 7U;
    case PersistenceErrorCode::GenerationConflict:        return 8U;
    case PersistenceErrorCode::SnapshotBusy:              return 9U;
    case PersistenceErrorCode::SnapshotAborted:           return 10U;
    case PersistenceErrorCode::SnapshotResourceExhausted: return 11U;
    case PersistenceErrorCode::CommitIndeterminate:       return 12U;
    case PersistenceErrorCode::DiskFull:                  return 13U;
    case PersistenceErrorCode::AccessDenied:              return 14U;
    case PersistenceErrorCode::IoError:                   return 15U;
    case PersistenceErrorCode::Cancelled:                 return 16U;
    case PersistenceErrorCode::DeadlineExceeded:          return 17U;
    case PersistenceErrorCode::VerificationFailed:        return 18U;
    case PersistenceErrorCode::FatalInternalInvariant:    return 19U;
    }
    return std::nullopt;
}

constexpr std::optional<PersistenceErrorCode>
decode_persistence_error_code(std::uint32_t wire) noexcept {
    switch (wire) {
    case 1:  return PersistenceErrorCode::InvalidFormat;
    case 2:  return PersistenceErrorCode::UnsupportedVhdx;
    case 3:  return PersistenceErrorCode::ManifestCorrupt;
    case 4:  return PersistenceErrorCode::JournalCorrupt;
    case 5:  return PersistenceErrorCode::BackingDataCorrupt;
    case 6:  return PersistenceErrorCode::ImageInUse;
    case 7:  return PersistenceErrorCode::OperationBusy;
    case 8:  return PersistenceErrorCode::GenerationConflict;
    case 9:  return PersistenceErrorCode::SnapshotBusy;
    case 10: return PersistenceErrorCode::SnapshotAborted;
    case 11: return PersistenceErrorCode::SnapshotResourceExhausted;
    case 12: return PersistenceErrorCode::CommitIndeterminate;
    case 13: return PersistenceErrorCode::DiskFull;
    case 14: return PersistenceErrorCode::AccessDenied;
    case 15: return PersistenceErrorCode::IoError;
    case 16: return PersistenceErrorCode::Cancelled;
    case 17: return PersistenceErrorCode::DeadlineExceeded;
    case 18: return PersistenceErrorCode::VerificationFailed;
    case 19: return PersistenceErrorCode::FatalInternalInvariant;
    default: return std::nullopt;
    }
}

// ============================================================================
// Commit disposition (PLAN §1.5, frozen)
// ============================================================================

enum class CommitDisposition : std::uint8_t {
    NotCommitted                  = 0,
    CommitIndeterminate           = 1,
    Committed                     = 2,
    CommittedNeedsReconciliation  = 3,
};

constexpr std::string_view to_string(CommitDisposition disposition) noexcept {
    switch (disposition) {
    case CommitDisposition::NotCommitted:                  return "NotCommitted";
    case CommitDisposition::CommitIndeterminate:           return "CommitIndeterminate";
    case CommitDisposition::Committed:                     return "Committed";
    case CommitDisposition::CommittedNeedsReconciliation:  return "CommittedNeedsReconciliation";
    }
    return "UnknownCommitDisposition";
}

constexpr std::optional<std::uint8_t> encode_wire(
    CommitDisposition disposition) noexcept {
    switch (disposition) {
    case CommitDisposition::NotCommitted:                 return std::uint8_t{0};
    case CommitDisposition::CommitIndeterminate:          return std::uint8_t{1};
    case CommitDisposition::Committed:                    return std::uint8_t{2};
    case CommitDisposition::CommittedNeedsReconciliation: return std::uint8_t{3};
    }
    return std::nullopt;
}

constexpr std::optional<CommitDisposition> decode_commit_disposition(
    std::uint8_t wire) noexcept {
    switch (wire) {
    case 0: return CommitDisposition::NotCommitted;
    case 1: return CommitDisposition::CommitIndeterminate;
    case 2: return CommitDisposition::Committed;
    case 3: return CommitDisposition::CommittedNeedsReconciliation;
    default: return std::nullopt;
    }
}

constexpr std::optional<CommitDisposition> parse_commit_disposition(
    std::string_view text) noexcept {
    if (text == "NotCommitted") return CommitDisposition::NotCommitted;
    if (text == "CommitIndeterminate")
        return CommitDisposition::CommitIndeterminate;
    if (text == "Committed") return CommitDisposition::Committed;
    if (text == "CommittedNeedsReconciliation")
        return CommitDisposition::CommittedNeedsReconciliation;
    return std::nullopt;
}

/// Exhaustive valid-transition table (SPEC §12, PLAN §1.5).
/// Returns true if transitioning from `from` to `to` is allowed.
constexpr bool is_valid_disposition_transition(
    CommitDisposition from, CommitDisposition to) noexcept
{
    switch (from) {
    case CommitDisposition::NotCommitted:
        // Can transition to any disposition.
        return true;
    case CommitDisposition::CommitIndeterminate:
        // Resolution may prove old gen intact → NotCommitted,
        // or prove new gen committed → CommittedNeedsReconciliation.
        return to == CommitDisposition::NotCommitted ||
               to == CommitDisposition::CommittedNeedsReconciliation;
    case CommitDisposition::Committed:
        // Terminal: cannot transition back.
        return to == CommitDisposition::Committed;
    case CommitDisposition::CommittedNeedsReconciliation:
        // Reconciliation can succeed → Committed, or remain in reconciliation.
        return to == CommitDisposition::Committed ||
               to == CommitDisposition::CommittedNeedsReconciliation;
    }
    return false;
}

// ============================================================================
// CheckpointOutcome (PLAN §1.5, frozen)
// ============================================================================

// ============================================================================
// Volume barrier kind (PLAN §1.5, frozen)
// ============================================================================

enum class VolumeBarrierKind : std::uint8_t {
    WindowsVolumeFlushed            = 0,
    RawBlockTestNoFilesystemCache   = 1,
};

constexpr std::string_view to_string(VolumeBarrierKind kind) noexcept {
    switch (kind) {
    case VolumeBarrierKind::WindowsVolumeFlushed:          return "WindowsVolumeFlushed";
    case VolumeBarrierKind::RawBlockTestNoFilesystemCache: return "RawBlockTestNoFilesystemCache";
    }
    return "UnknownVolumeBarrierKind";
}

constexpr std::optional<std::uint8_t> encode_wire(
    VolumeBarrierKind kind) noexcept {
    switch (kind) {
    case VolumeBarrierKind::WindowsVolumeFlushed:          return std::uint8_t{0};
    case VolumeBarrierKind::RawBlockTestNoFilesystemCache: return std::uint8_t{1};
    }
    return std::nullopt;
}

constexpr std::optional<VolumeBarrierKind> decode_volume_barrier_kind(
    std::uint8_t wire) noexcept {
    switch (wire) {
    case 0: return VolumeBarrierKind::WindowsVolumeFlushed;
    case 1: return VolumeBarrierKind::RawBlockTestNoFilesystemCache;
    default: return std::nullopt;
    }
}

/// Returns true if `kind` is one of the two known values (not an unlisted byte).
constexpr bool is_valid_volume_barrier_kind(VolumeBarrierKind kind) noexcept {
    switch (kind) {
    case VolumeBarrierKind::WindowsVolumeFlushed:
    case VolumeBarrierKind::RawBlockTestNoFilesystemCache:
        return true;
    }
    return false;
}

/// Only WindowsVolumeFlushed permits a filesystem-durability claim.
/// RawBlockTestNoFilesystemCache explicitly does not.
constexpr bool permits_filesystem_durability_claim(
    VolumeBarrierKind kind) noexcept
{
    return kind == VolumeBarrierKind::WindowsVolumeFlushed;
}

/// Validate a barrier kind for a checkpoint request.
/// An unknown/missing barrier (cast from an out-of-range integer) fails.
constexpr bool is_valid_barrier_for_checkpoint(
    VolumeBarrierKind kind) noexcept
{
    return is_valid_volume_barrier_kind(kind);
}

/// Optional form used by request validation so an omitted barrier cannot be
/// confused with the zero-valued production barrier.
constexpr bool is_valid_barrier_for_checkpoint(
    const std::optional<VolumeBarrierKind>& kind) noexcept
{
    return kind.has_value() && is_valid_volume_barrier_kind(*kind);
}

// ============================================================================
// Session phase (SPEC §12, frozen)
// ============================================================================

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
    case SessionPhase::Closed:             return "Closed";
    case SessionPhase::Opening:            return "Opening";
    case SessionPhase::Ready:              return "Ready";
    case SessionPhase::Checkpointing:      return "Checkpointing";
    case SessionPhase::Compacting:         return "Compacting";
    case SessionPhase::Verifying:          return "Verifying";
    case SessionPhase::ResolvingCommit:    return "ResolvingCommit";
    case SessionPhase::Reconciling:        return "Reconciling";
    case SessionPhase::Closing:            return "Closing";
    case SessionPhase::PersistenceFaulted: return "PersistenceFaulted";
    }
    return "UnknownSessionPhase";
}

constexpr std::optional<std::uint8_t> encode_wire(
    SessionPhase phase) noexcept {
    switch (phase) {
    case SessionPhase::Closed:             return std::uint8_t{0};
    case SessionPhase::Opening:            return std::uint8_t{1};
    case SessionPhase::Ready:              return std::uint8_t{2};
    case SessionPhase::Checkpointing:      return std::uint8_t{3};
    case SessionPhase::Compacting:         return std::uint8_t{4};
    case SessionPhase::Verifying:          return std::uint8_t{5};
    case SessionPhase::ResolvingCommit:    return std::uint8_t{6};
    case SessionPhase::Reconciling:        return std::uint8_t{7};
    case SessionPhase::Closing:            return std::uint8_t{8};
    case SessionPhase::PersistenceFaulted: return std::uint8_t{9};
    }
    return std::nullopt;
}

constexpr std::optional<SessionPhase> decode_session_phase(
    std::uint8_t wire) noexcept {
    switch (wire) {
    case 0: return SessionPhase::Closed;
    case 1: return SessionPhase::Opening;
    case 2: return SessionPhase::Ready;
    case 3: return SessionPhase::Checkpointing;
    case 4: return SessionPhase::Compacting;
    case 5: return SessionPhase::Verifying;
    case 6: return SessionPhase::ResolvingCommit;
    case 7: return SessionPhase::Reconciling;
    case 8: return SessionPhase::Closing;
    case 9: return SessionPhase::PersistenceFaulted;
    default: return std::nullopt;
    }
}

// ============================================================================
// Persistence error context (SPEC §14, §16, frozen)
// ============================================================================

struct PersistenceErrorContext {
    /// Human-readable operation label (not used for programmatic branching).
    std::string operation{};
    PersistenceErrorCode code{PersistenceErrorCode::FatalInternalInvariant};
    /// Session phase when the error occurred.
    SessionPhase phase{SessionPhase::Closed};
    /// Platform error code (Win32 ERROR_* or NTSTATUS, 0 if not applicable).
    std::uint32_t native_code{0};
    /// Unique operation id for correlation with evidence.
    std::uint64_t operation_id{0};
    /// Image UUID bytes — all zero when not available.
    ImageUuid image_uuid{};
    /// Generation at the time of the error, or kInvalidGeneration.
    std::uint64_t generation{kInvalidGeneration};
    /// Commit disposition at the time of the error.
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    std::string safe_context{};
};

/// Returns true if the context is in its default/zero state.
constexpr bool is_default_context(const PersistenceErrorContext& ctx) noexcept {
    return ctx.operation.empty() &&
           ctx.code == PersistenceErrorCode::FatalInternalInvariant &&
           ctx.phase == SessionPhase::Closed &&
           ctx.native_code == 0 &&
           ctx.operation_id == 0 &&
           ctx.image_uuid.is_nil() &&
           ctx.generation == kInvalidGeneration &&
           ctx.disposition == CommitDisposition::NotCommitted &&
           ctx.safe_context.empty();
}

struct CheckpointOutcome {
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    std::uint64_t generation{0};
    bool manifest_candidate_verified{false};
    bool backing_view_installed{false};
    std::uint64_t acknowledged_blocks{0};
    std::uint64_t still_dirty_blocks{0};
    std::optional<Error> error;
    std::optional<PersistenceErrorContext> error_context;
};

}  // namespace persistence
}  // namespace myrd
