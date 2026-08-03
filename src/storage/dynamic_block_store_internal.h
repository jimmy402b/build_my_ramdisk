#pragma once

#include "dynamic_block_types.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>

namespace myrd {

class IVirtualMemoryApi;
class VirtualMemoryCleanupTracker;
class DynamicBlockStore;

// BlockEntry deliberately contains only scalar metadata. Synchronization is
// supplied by LockStripeSet, never by one mutex/condition variable per block.
struct BlockEntry {
    BlockState state{BlockState::Unresident};
    BlockState prior_state{BlockState::Unresident};
    std::uint64_t transition_owner{};
    std::uint64_t transition_epoch{};
    std::uint64_t load_epoch{};
    std::uint64_t version{};
    std::uint64_t pin_count{};
    std::uint64_t last_access_epoch{};
    std::uint64_t dirty_epoch{};
    BackingGeneration dirty_base_generation{};
    BackingGeneration clean_generation{};
    bool reference_bit{};
    DirtyRecordKind dirty_kind{DirtyRecordKind::Data};
};

static_assert(std::is_trivially_copyable_v<BlockEntry>);

enum class BlockEntryInvariantError : std::uint8_t {
    None = 0,
    InvalidState = 1,
    InvalidPriorState = 2,
    InvalidDirtyKind = 3,
    TransientPriorNotStable = 4,
    BackingGenerationWrapped = 5,
    ContentVersionWrapped = 6,
    CleanGenerationAhead = 7,
    DirtyBaseGenerationAhead = 8,
};

struct BlockEntryInvariantResult {
    BlockEntryInvariantError error{BlockEntryInvariantError::None};

    constexpr bool ok() const noexcept {
        return error == BlockEntryInvariantError::None;
    }
};

// The wrap flags are sticky state owned by the future store core. They let the
// checker distinguish a legitimate UINT64_MAX value from an already-detected
// attempt to wrap it back to zero.
BlockEntryInvariantResult validate_block_entry(
    const BlockEntry& entry,
    BackingGeneration current_backing_generation,
    bool backing_generation_wrapped,
    bool content_version_wrapped) noexcept;

template <class T, class Error>
class StorageInternalResult {
public:
    static StorageInternalResult success(T value) {
        return StorageInternalResult(std::move(value), Error::None);
    }

    static StorageInternalResult failure(Error error) {
        return StorageInternalResult(T{}, error);
    }

    bool ok() const noexcept { return error_ == Error::None; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() noexcept { return value_; }
    const T& value() const noexcept { return value_; }
    T&& move_value() noexcept { return std::move(value_); }
    Error error() const noexcept { return error_; }

private:
    StorageInternalResult(T value, Error error)
        : value_(std::move(value)), error_(error) {}

    T value_{};
    Error error_;
};

struct LockStripe {
    std::mutex mutex;
    std::condition_variable condition;

    LockStripe() = default;
    LockStripe(const LockStripe&) = delete;
    LockStripe& operator=(const LockStripe&) = delete;
};

enum class LockStripeSetError : std::uint8_t {
    None = 0,
    InvalidCount = 1,
    AllocationFailed = 2,
};

class LockStripeSet final {
public:
    using CreateResult = StorageInternalResult<
        std::unique_ptr<LockStripeSet>, LockStripeSetError>;

    static CreateResult create(std::size_t stripe_count) noexcept;

    LockStripeSet(const LockStripeSet&) = delete;
    LockStripeSet& operator=(const LockStripeSet&) = delete;
    LockStripeSet(LockStripeSet&&) = delete;
    LockStripeSet& operator=(LockStripeSet&&) = delete;

    std::size_t count() const noexcept { return stripe_count_; }

    std::size_t stripe_index(std::uint64_t block_index) const noexcept {
        return static_cast<std::size_t>(
            block_index & static_cast<std::uint64_t>(stripe_count_ - 1));
    }

    LockStripe& stripe_for(std::uint64_t block_index) noexcept {
        return stripes_[stripe_index(block_index)];
    }

private:
    LockStripeSet(std::unique_ptr<LockStripe[]> stripes,
                  std::size_t stripe_count) noexcept
        : stripes_(std::move(stripes)), stripe_count_(stripe_count) {}

    std::unique_ptr<LockStripe[]> stripes_;
    std::size_t stripe_count_{};
};

enum class DynamicBlockStoreFactoryStage : std::uint8_t {
    None = 0,
    CreateStripes = 1,
    AllocateDirectory = 2,
    StartWorker = 3,
    AfterWorkerStarted = 4,
};

enum class DynamicBlockStoreLifecycleEvent : std::uint8_t {
    WorkerStarted = 0,
    RejectNewRequests = 1,
    WaitersNotified = 2,
    WorkerStopped = 3,
    Quiesced = 4,
    DirectoryReleased = 5,
    StripesReleased = 6,
    ArenaReleased = 7,
};

class IDynamicBlockStoreLifecycleObserver {
public:
    virtual ~IDynamicBlockStoreLifecycleObserver() = default;
    virtual void on_event(DynamicBlockStoreLifecycleEvent event) noexcept = 0;
};

class IDynamicBlockStoreReadObserver {
public:
    virtual ~IDynamicBlockStoreReadObserver() = default;
    virtual void after_resident_pin(std::uint64_t block_index) noexcept = 0;
    virtual void after_segment(std::uint64_t block_index) noexcept = 0;
};

// Mutating access is intentionally confined to tests that need to establish
// states owned by later M2 tasks. Product code cannot reach these helpers
// through DynamicBlockStore's public API.
class DynamicBlockStoreTestAccess final {
public:
    static bool seed_zero(DynamicBlockStore& store,
                          std::uint64_t block_index,
                          BlockState state,
                          BackingGeneration generation);
    static bool seed_resident(DynamicBlockStore& store,
                              std::uint64_t block_index,
                              BlockState state,
                              std::span<const std::byte> data,
                              BackingGeneration generation);
    static bool seed_uncommitted_resident(DynamicBlockStore& store,
                                          std::uint64_t block_index,
                                          BlockState state,
                                          BackingGeneration generation);
    static bool snapshot_entry(DynamicBlockStore& store,
                               std::uint64_t block_index,
                               BlockEntry& output);
    static bool try_decommit_resident_clean(DynamicBlockStore& store,
                                            std::uint64_t block_index);
};

struct DynamicBlockStoreTestDependencies {
    std::shared_ptr<IVirtualMemoryApi> virtual_memory_api;
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker;
    std::shared_ptr<IDynamicBlockStoreLifecycleObserver> lifecycle_observer;
    std::shared_ptr<IDynamicBlockStoreReadObserver> read_observer;
    DynamicBlockStoreFactoryStage fail_stage{
        DynamicBlockStoreFactoryStage::None};
};

} // namespace myrd
