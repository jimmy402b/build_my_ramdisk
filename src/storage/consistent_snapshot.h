#pragma once

/// @file consistent_snapshot.h
/// @brief M2.1 consistent snapshot public contracts (types, token, status).
///
/// This header belongs to myrd_storage and defines the public API surface for
/// point-in-time consistent snapshots. The implementation (Task 02–09) lives in
/// snapshot_coordinator and store internals.

#include <cstdint>
#include <string_view>

namespace myrd {

class DynamicBlockStore;

using BackingGeneration = std::uint64_t;

// ---- Token -----------------------------------------------------------------
struct ConsistentSnapshotToken {
    std::uint64_t snapshot_id{};
    BackingGeneration source_backing_generation{};
    std::uint64_t cutoff_dirty_epoch{};
};

// ---- State -----------------------------------------------------------------
enum class ConsistentSnapshotState : std::uint8_t {
    Active,
    Aborted,
    FullyScanned,
};

constexpr std::string_view to_string(ConsistentSnapshotState state) noexcept {
    switch (state) {
    case ConsistentSnapshotState::Active:
        return "Active";
    case ConsistentSnapshotState::Aborted:
        return "Aborted";
    case ConsistentSnapshotState::FullyScanned:
        return "FullyScanned";
    }
    return "UnknownConsistentSnapshotState";
}

// ---- Status ----------------------------------------------------------------
struct ConsistentSnapshotStatus {
    ConsistentSnapshotState state{ConsistentSnapshotState::Active};
    std::uint64_t cow_bytes{};
    std::uint64_t cow_records{};
    std::uint64_t blocked_writers{};
};

}  // namespace myrd
