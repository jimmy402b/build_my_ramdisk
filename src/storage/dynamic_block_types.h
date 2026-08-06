#pragma once

#include "block_math.h"
#include "consistent_snapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace myrd {

class IBackingStore;

// The bounded coordinator allocates one fixed-size bookkeeping slab for each
// pre-image.  The slab includes the record, ordered-index node and allocator
// metadata; a DATA payload is charged separately.  Later coordinator tasks
// must use this exact charge instead of estimating STL node overhead.
inline constexpr std::uint64_t kSnapshotCowRecordBudgetBytes = 256;
inline constexpr std::uint64_t kMinimumSnapshotCowBytes =
    kBlockSizeBytes + kSnapshotCowRecordBudgetBytes;

enum class InitialBlockState : std::uint8_t {
    CleanZero = 0,
    BackingUnresident = 1,
};

constexpr std::string_view to_string(InitialBlockState state) noexcept {
    switch (state) {
    case InitialBlockState::CleanZero:
        return "CleanZero";
    case InitialBlockState::BackingUnresident:
        return "BackingUnresident";
    }
    return "UnknownInitialBlockState";
}

enum class BlockState : std::uint8_t {
    Unresident = 0,
    ResidentClean = 1,
    ResidentDirty = 2,
    ZeroClean = 3,
    ZeroDirty = 4,
    Loading = 5,
    Committing = 6,
    Evicting = 7,
};

constexpr std::string_view to_string(BlockState state) noexcept {
    switch (state) {
    case BlockState::Unresident:
        return "Unresident";
    case BlockState::ResidentClean:
        return "ResidentClean";
    case BlockState::ResidentDirty:
        return "ResidentDirty";
    case BlockState::ZeroClean:
        return "ZeroClean";
    case BlockState::ZeroDirty:
        return "ZeroDirty";
    case BlockState::Loading:
        return "Loading";
    case BlockState::Committing:
        return "Committing";
    case BlockState::Evicting:
        return "Evicting";
    }
    return "UnknownBlockState";
}

constexpr bool is_stable_block_state(BlockState state) noexcept {
    switch (state) {
    case BlockState::Unresident:
    case BlockState::ResidentClean:
    case BlockState::ResidentDirty:
    case BlockState::ZeroClean:
    case BlockState::ZeroDirty:
        return true;
    case BlockState::Loading:
    case BlockState::Committing:
    case BlockState::Evicting:
        return false;
    }
    return false;
}

constexpr bool is_transient_block_state(BlockState state) noexcept {
    switch (state) {
    case BlockState::Loading:
    case BlockState::Committing:
    case BlockState::Evicting:
        return true;
    case BlockState::Unresident:
    case BlockState::ResidentClean:
    case BlockState::ResidentDirty:
    case BlockState::ZeroClean:
    case BlockState::ZeroDirty:
        return false;
    }
    return false;
}

struct DynamicBlockStoreOptions {
    std::uint64_t soft_limit_bytes;
    std::uint64_t hard_limit_bytes;
    std::uint64_t eviction_low_watermark_bytes;
    std::uint64_t max_snapshot_batch_bytes;
    std::size_t max_snapshot_batch_records;
    std::size_t lock_stripe_count;
    // ---- M2.1 consistent snapshot options ----------------------------------
    std::uint64_t snapshot_cow_max_bytes{2 * kMinimumSnapshotCowBytes};
    std::chrono::milliseconds snapshot_writer_stall_limit{250};
};

struct DynamicBlockStoreCreateArgs {
    std::uint64_t logical_size_bytes;
    std::uint32_t sector_size_bytes;
    DynamicBlockStoreOptions options;
    InitialBlockState initial_state;
    std::shared_ptr<const IBackingStore> backing_store;
    BackingGeneration initial_backing_generation;
};

enum class DirtyRecordKind : std::uint8_t {
    Data = 0,
    Zero = 1,
    Discard = 2,
};

constexpr std::string_view to_string(DirtyRecordKind kind) noexcept {
    switch (kind) {
    case DirtyRecordKind::Data:
        return "Data";
    case DirtyRecordKind::Zero:
        return "Zero";
    case DirtyRecordKind::Discard:
        return "Discard";
    }
    return "UnknownDirtyRecordKind";
}

struct DirtyBlockSnapshot {
    std::uint64_t block_index{};
    std::uint64_t version{};
    DirtyRecordKind kind{DirtyRecordKind::Data};
    BackingGeneration base_generation{};
    std::uint64_t dirty_epoch{};
    std::vector<std::byte> data;
};

enum class SnapshotMemberAck : std::uint8_t {
    Acknowledged = 0,
    AlreadyPersisted,
    NewerDirtyPreserved,
};

struct DirtyScanCursor {
    std::uint64_t next_block_index{};
};

enum class DynamicBlockStoreErrorCode : std::uint32_t {
    None = 0,
    BackingGenerationConflict = 0x2001,
    FatalInternalInvariant = 0x2002,
    // ---- M2.1 consistent snapshot errors -----------------------------------
    SnapshotBusy                = 0x2101,
    SnapshotTokenInvalid        = 0x2102,
    SnapshotAborted             = 0x2103,
    SnapshotNotFullyScanned     = 0x2104,
    SnapshotResourceExhausted   = 0x2105,
};

constexpr std::string_view to_string(
    DynamicBlockStoreErrorCode error) noexcept {
    switch (error) {
    case DynamicBlockStoreErrorCode::None:
        return "None";
    case DynamicBlockStoreErrorCode::BackingGenerationConflict:
        return "BackingGenerationConflict";
    case DynamicBlockStoreErrorCode::FatalInternalInvariant:
        return "FatalInternalInvariant";
    case DynamicBlockStoreErrorCode::SnapshotBusy:
        return "SnapshotBusy";
    case DynamicBlockStoreErrorCode::SnapshotTokenInvalid:
        return "SnapshotTokenInvalid";
    case DynamicBlockStoreErrorCode::SnapshotAborted:
        return "SnapshotAborted";
    case DynamicBlockStoreErrorCode::SnapshotNotFullyScanned:
        return "SnapshotNotFullyScanned";
    case DynamicBlockStoreErrorCode::SnapshotResourceExhausted:
        return "SnapshotResourceExhausted";
    }
    return "UnknownDynamicBlockStoreErrorCode";
}

struct CheckedSequenceIncrement {
    std::uint64_t value{};
    DynamicBlockStoreErrorCode error{DynamicBlockStoreErrorCode::None};

    constexpr bool ok() const noexcept {
        return error == DynamicBlockStoreErrorCode::None;
    }
};

constexpr CheckedSequenceIncrement checked_increment_version(
    std::uint64_t current) noexcept {
    if (current == std::numeric_limits<std::uint64_t>::max()) {
        return CheckedSequenceIncrement{
            current, DynamicBlockStoreErrorCode::FatalInternalInvariant};
    }
    return CheckedSequenceIncrement{
        current + 1, DynamicBlockStoreErrorCode::None};
}

constexpr CheckedSequenceIncrement checked_increment_backing_generation(
    BackingGeneration current) noexcept {
    return checked_increment_version(current);
}

enum class DynamicBlockStoreOptionsError : std::uint8_t {
    None = 0,
    InvalidLimitOrder = 1,
    LimitNotBlockAligned = 2,
    SnapshotBytesTooSmall = 3,
    SnapshotBytesNotBlockAligned = 4,
    SnapshotRecordsZero = 5,
    StripeCountTooSmall = 6,
    StripeCountNotPowerOfTwo = 7,
    // ---- M2.1 consistent snapshot option errors ----------------------------
    SnapshotCowBytesTooSmall = 8,
    SnapshotCowStallNotPositive = 9,
    SnapshotCowStallTooLarge = 10,
};

constexpr std::string_view to_string(
    DynamicBlockStoreOptionsError error) noexcept {
    switch (error) {
    case DynamicBlockStoreOptionsError::None:
        return "None";
    case DynamicBlockStoreOptionsError::InvalidLimitOrder:
        return "InvalidLimitOrder";
    case DynamicBlockStoreOptionsError::LimitNotBlockAligned:
        return "LimitNotBlockAligned";
    case DynamicBlockStoreOptionsError::SnapshotBytesTooSmall:
        return "SnapshotBytesTooSmall";
    case DynamicBlockStoreOptionsError::SnapshotBytesNotBlockAligned:
        return "SnapshotBytesNotBlockAligned";
    case DynamicBlockStoreOptionsError::SnapshotRecordsZero:
        return "SnapshotRecordsZero";
    case DynamicBlockStoreOptionsError::StripeCountTooSmall:
        return "StripeCountTooSmall";
    case DynamicBlockStoreOptionsError::StripeCountNotPowerOfTwo:
        return "StripeCountNotPowerOfTwo";
    case DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall:
        return "SnapshotCowBytesTooSmall";
    case DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive:
        return "SnapshotCowStallNotPositive";
    case DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge:
        return "SnapshotCowStallTooLarge";
    }
    return "UnknownDynamicBlockStoreOptionsError";
}

struct DynamicBlockStoreOptionsValidation {
    DynamicBlockStoreOptionsError error{DynamicBlockStoreOptionsError::None};

    constexpr bool ok() const noexcept {
        return error == DynamicBlockStoreOptionsError::None;
    }
};

constexpr DynamicBlockStoreOptionsValidation
validate_dynamic_block_store_options(
    const DynamicBlockStoreOptions& options) noexcept {
    if (options.eviction_low_watermark_bytes == 0 ||
        options.eviction_low_watermark_bytes > options.soft_limit_bytes ||
        options.soft_limit_bytes >= options.hard_limit_bytes) {
        return {DynamicBlockStoreOptionsError::InvalidLimitOrder};
    }
    if ((options.eviction_low_watermark_bytes % kBlockSizeBytes) != 0 ||
        (options.soft_limit_bytes % kBlockSizeBytes) != 0 ||
        (options.hard_limit_bytes % kBlockSizeBytes) != 0) {
        return {DynamicBlockStoreOptionsError::LimitNotBlockAligned};
    }
    if (options.max_snapshot_batch_bytes < kBlockSizeBytes) {
        return {DynamicBlockStoreOptionsError::SnapshotBytesTooSmall};
    }
    if ((options.max_snapshot_batch_bytes % kBlockSizeBytes) != 0) {
        return {
            DynamicBlockStoreOptionsError::SnapshotBytesNotBlockAligned};
    }
    if (options.max_snapshot_batch_records == 0) {
        return {DynamicBlockStoreOptionsError::SnapshotRecordsZero};
    }
    if (options.lock_stripe_count < 64) {
        return {DynamicBlockStoreOptionsError::StripeCountTooSmall};
    }
    if ((options.lock_stripe_count & (options.lock_stripe_count - 1)) != 0) {
        return {DynamicBlockStoreOptionsError::StripeCountNotPowerOfTwo};
    }
    // ---- M2.1 consistent snapshot option validation ------------------------
    if (options.snapshot_cow_max_bytes < kMinimumSnapshotCowBytes) {
        return {DynamicBlockStoreOptionsError::SnapshotCowBytesTooSmall};
    }
    if (options.snapshot_writer_stall_limit.count() <= 0) {
        return {DynamicBlockStoreOptionsError::SnapshotCowStallNotPositive};
    }
    if (options.snapshot_writer_stall_limit.count() > 5000) {
        return {DynamicBlockStoreOptionsError::SnapshotCowStallTooLarge};
    }
    return {};
}

enum class BackingNormalization : std::uint8_t {
    None = 0,
    UseZeroBackingStore = 1,
};

constexpr std::string_view to_string(
    BackingNormalization normalization) noexcept {
    switch (normalization) {
    case BackingNormalization::None:
        return "None";
    case BackingNormalization::UseZeroBackingStore:
        return "UseZeroBackingStore";
    }
    return "UnknownBackingNormalization";
}

enum class DynamicBlockStoreCreateError : std::uint8_t {
    None = 0,
    InvalidLogicalSize = 1,
    InvalidSectorSize = 2,
    LogicalSizeNotSectorAligned = 3,
    InvalidGeometry = 4,
    InvalidOptions = 5,
    InvalidInitialState = 6,
    BackingStoreRequired = 7,
};

constexpr std::string_view to_string(
    DynamicBlockStoreCreateError error) noexcept {
    switch (error) {
    case DynamicBlockStoreCreateError::None:
        return "None";
    case DynamicBlockStoreCreateError::InvalidLogicalSize:
        return "InvalidLogicalSize";
    case DynamicBlockStoreCreateError::InvalidSectorSize:
        return "InvalidSectorSize";
    case DynamicBlockStoreCreateError::LogicalSizeNotSectorAligned:
        return "LogicalSizeNotSectorAligned";
    case DynamicBlockStoreCreateError::InvalidGeometry:
        return "InvalidGeometry";
    case DynamicBlockStoreCreateError::InvalidOptions:
        return "InvalidOptions";
    case DynamicBlockStoreCreateError::InvalidInitialState:
        return "InvalidInitialState";
    case DynamicBlockStoreCreateError::BackingStoreRequired:
        return "BackingStoreRequired";
    }
    return "UnknownDynamicBlockStoreCreateError";
}

struct DynamicBlockStoreCreateValidation {
    DynamicBlockStoreCreateError error{DynamicBlockStoreCreateError::None};
    DynamicBlockStoreOptionsError options_error{
        DynamicBlockStoreOptionsError::None};
    BackingNormalization backing_normalization{BackingNormalization::None};

    constexpr bool ok() const noexcept {
        return error == DynamicBlockStoreCreateError::None;
    }

    constexpr bool requires_zero_backing_store() const noexcept {
        return backing_normalization ==
               BackingNormalization::UseZeroBackingStore;
    }
};

inline DynamicBlockStoreCreateValidation
validate_dynamic_block_store_create_args(
    const DynamicBlockStoreCreateArgs& args) noexcept {
    if (args.logical_size_bytes == 0) {
        return {DynamicBlockStoreCreateError::InvalidLogicalSize};
    }
    if (!is_supported_sector_size(args.sector_size_bytes)) {
        return {DynamicBlockStoreCreateError::InvalidSectorSize};
    }
    if ((args.logical_size_bytes % args.sector_size_bytes) != 0) {
        return {
            DynamicBlockStoreCreateError::LogicalSizeNotSectorAligned};
    }
    if (!compute_block_geometry(args.logical_size_bytes).ok()) {
        return {DynamicBlockStoreCreateError::InvalidGeometry};
    }

    const auto options = validate_dynamic_block_store_options(args.options);
    if (!options.ok()) {
        return {DynamicBlockStoreCreateError::InvalidOptions, options.error};
    }

    switch (args.initial_state) {
    case InitialBlockState::CleanZero:
        if (!args.backing_store) {
            return {
                DynamicBlockStoreCreateError::None,
                DynamicBlockStoreOptionsError::None,
                BackingNormalization::UseZeroBackingStore,
            };
        }
        return {};
    case InitialBlockState::BackingUnresident:
        if (!args.backing_store) {
            return {DynamicBlockStoreCreateError::BackingStoreRequired};
        }
        return {};
    }
    return {DynamicBlockStoreCreateError::InvalidInitialState};
}

struct DynamicBlockStoreStats {
    // Coherent gauges: capture these from one logical store snapshot.
    std::uint64_t logical_size_bytes{};
    std::uint64_t reserved_bytes{};
    std::uint64_t committed_bytes{};
    std::uint64_t resident_clean_blocks{};
    std::uint64_t resident_dirty_blocks{};
    std::uint64_t zero_clean_blocks{};
    std::uint64_t zero_dirty_blocks{};
    std::uint64_t loading_blocks{};

    // Lifetime monotonic counters: these never decrease for a store instance.
    std::uint64_t evicted_blocks_total{};
    std::uint64_t backing_reads_total{};
    std::uint64_t backing_read_bytes_total{};
    std::uint64_t uncached_reads_total{};
    std::uint64_t unmap_bytes_total{};
    std::uint64_t zero_bytes_total{};
    std::uint64_t hard_limit_rejections{};
    std::uint64_t commit_failures{};
    std::uint64_t decommit_failures{};
    std::uint64_t synchronous_pressure_passes_total{};
    std::uint64_t synchronous_pressure_waits_total{};
    std::uint64_t synchronous_pressure_no_progress_total{};
    std::uint64_t synchronous_pressure_cancellations_total{};
    std::uint64_t synchronous_pressure_deadlines_total{};
    std::uint64_t commit_reservation_rollbacks_total{};
    std::uint64_t snapshot_records_total{};
    std::uint64_t snapshot_version_conflicts{};

    // Coherent gauge: generation belonging to the same snapshot as gauges above.
    BackingGeneration backing_generation{};

    // Lifetime monotonic counters, continued after the generation gauge.
    std::uint64_t backing_view_installs_total{};
    std::uint64_t backing_generation_conflicts{};
    std::uint64_t stale_backing_load_retries{};

    // ---- M2.1 consistent snapshot gauges -----------------------------------
    std::uint64_t snapshot_active_id{};
    std::optional<ConsistentSnapshotState> snapshot_active_state{};
    // ---- M2.1 consistent snapshot gauges -----------------------------------
    std::uint64_t snapshot_cow_current_bytes{};
    // ---- M2.1 consistent snapshot peak/lifetime counters -------------------
    std::uint64_t snapshot_cow_peak_bytes{};
    // ---- M2.1 consistent snapshot gauges, continued ------------------------
    std::uint64_t snapshot_cow_records{};
    std::uint64_t snapshot_blocked_writers{};
    // ---- M2.1 consistent snapshot lifetime counters ------------------------
    std::uint64_t snapshot_writer_stalls_total{};
    std::uint64_t snapshot_auto_aborts_total{};
    std::uint64_t snapshot_consistent_batches_total{};
    std::uint64_t snapshot_consistent_records_total{};

    bool operator==(const DynamicBlockStoreStats&) const = default;
};

} // namespace myrd
