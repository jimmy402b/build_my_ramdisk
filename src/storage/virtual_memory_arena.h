#pragma once

#include "src/common/result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace myrd {

enum class VirtualMemoryRegionState {
    Free,
    Reserved,
    Committed,
    Unknown
};

struct VirtualMemoryRegionInfo {
    const void* allocation_base = nullptr;
    const void* base_address = nullptr;
    std::uint64_t region_size_bytes = 0;
    VirtualMemoryRegionState state = VirtualMemoryRegionState::Unknown;
};

class IVirtualMemoryApi {
public:
    virtual ~IVirtualMemoryApi() = default;

    virtual void* allocate(void* address, std::size_t size_bytes,
        std::uint32_t allocation_type,
        std::uint32_t protection) noexcept = 0;
    virtual bool free(void* address, std::size_t size_bytes,
        std::uint32_t free_type) noexcept = 0;
    virtual bool query(const void* address,
        VirtualMemoryRegionInfo& information) noexcept = 0;
    virtual std::uint32_t last_error() const noexcept = 0;
};

class VirtualMemoryCleanupTracker final {
public:
    std::uint64_t release_failures() const noexcept;
    std::uint32_t last_native_error() const noexcept;

private:
    friend class VirtualMemoryArena;
    void record_release_failure(std::uint32_t native_error) noexcept;

    std::atomic<std::uint64_t> release_failures_{0};
    std::atomic<std::uint32_t> last_native_error_{0};
};

class VirtualMemoryArena final {
public:
    static Result<std::unique_ptr<VirtualMemoryArena>> create(
        std::uint64_t reservation_size_bytes,
        std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker = {});

    static Result<std::unique_ptr<VirtualMemoryArena>> create_with_api(
        std::uint64_t reservation_size_bytes,
        std::shared_ptr<IVirtualMemoryApi> api,
        std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker = {});

    ~VirtualMemoryArena() noexcept;

    VirtualMemoryArena(const VirtualMemoryArena&) = delete;
    VirtualMemoryArena& operator=(const VirtualMemoryArena&) = delete;
    VirtualMemoryArena(VirtualMemoryArena&& other) noexcept;
    VirtualMemoryArena& operator=(VirtualMemoryArena&& other) noexcept;

    std::byte* base_address() const noexcept;
    std::uint64_t reserved_bytes() const noexcept;
    std::uint64_t block_count() const noexcept;
    std::uint64_t committed_bytes() const noexcept;
    bool is_block_committed(std::uint64_t block_index) const noexcept;
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker() const;

    Result<std::byte*> block_address(std::uint64_t block_index) const;
    Result<std::byte*> commit_block(std::uint64_t block_index);
    Result<void> decommit_block(std::uint64_t block_index);
    Result<VirtualMemoryRegionInfo> query_block(
        std::uint64_t block_index) const;

private:
    VirtualMemoryArena(std::uint64_t reservation_size_bytes,
        std::shared_ptr<IVirtualMemoryApi> api,
        std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker) noexcept;

    void release_noexcept() noexcept;
    void move_from_locked(VirtualMemoryArena& other) noexcept;
    std::uint64_t block_count_locked() const noexcept;
    Result<std::byte*> block_address_locked(
        std::uint64_t block_index) const;

    mutable std::mutex state_mutex_;
    std::shared_ptr<IVirtualMemoryApi> api_;
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker_;
    std::byte* base_address_ = nullptr;
    std::uint64_t reservation_size_bytes_ = 0;
    std::unordered_set<std::uint64_t> committed_blocks_;
    std::uint64_t committed_bytes_ = 0;
};

} // namespace myrd
