#include "virtual_memory_arena.h"

#include "block_constants.h"

#include <windows.h>

#include <limits>
#include <new>
#include <utility>

namespace myrd {
namespace {

Error storage_error(OperationErrorCode code, const wchar_t* operation,
    const wchar_t* message, std::uint32_t native_code = 0) {
    return {ErrorDomain::storage, static_cast<std::uint32_t>(code), operation,
        message, native_code};
}

class Win32VirtualMemoryApi final : public IVirtualMemoryApi {
public:
    void* allocate(void* address, std::size_t size_bytes,
        std::uint32_t allocation_type,
        std::uint32_t protection) noexcept override {
        return ::VirtualAlloc(address, size_bytes, allocation_type, protection);
    }

    bool free(void* address, std::size_t size_bytes,
        std::uint32_t free_type) noexcept override {
        return ::VirtualFree(address, size_bytes, free_type) != FALSE;
    }

    bool query(const void* address,
        VirtualMemoryRegionInfo& information) noexcept override {
        MEMORY_BASIC_INFORMATION native{};
        if (::VirtualQuery(address, &native, sizeof(native)) == 0) {
            return false;
        }

        information.allocation_base = native.AllocationBase;
        information.base_address = native.BaseAddress;
        information.region_size_bytes =
            static_cast<std::uint64_t>(native.RegionSize);
        switch (native.State) {
        case MEM_FREE:
            information.state = VirtualMemoryRegionState::Free;
            break;
        case MEM_RESERVE:
            information.state = VirtualMemoryRegionState::Reserved;
            break;
        case MEM_COMMIT:
            information.state = VirtualMemoryRegionState::Committed;
            break;
        default:
            information.state = VirtualMemoryRegionState::Unknown;
            break;
        }
        return true;
    }

    std::uint32_t last_error() const noexcept override {
        return ::GetLastError();
    }
};

std::shared_ptr<IVirtualMemoryApi> production_virtual_memory_api() {
    static auto instance = std::make_shared<Win32VirtualMemoryApi>();
    return instance;
}

} // namespace

std::uint64_t VirtualMemoryCleanupTracker::release_failures() const noexcept {
    return release_failures_.load(std::memory_order_relaxed);
}

std::uint32_t VirtualMemoryCleanupTracker::last_native_error() const noexcept {
    return last_native_error_.load(std::memory_order_relaxed);
}

void VirtualMemoryCleanupTracker::record_release_failure(
    std::uint32_t native_error) noexcept {
    last_native_error_.store(native_error, std::memory_order_relaxed);
    release_failures_.fetch_add(1, std::memory_order_relaxed);
}

Result<std::unique_ptr<VirtualMemoryArena>> VirtualMemoryArena::create(
    std::uint64_t reservation_size_bytes,
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker) {
    try {
        return create_with_api(reservation_size_bytes,
            production_virtual_memory_api(), std::move(cleanup_tracker));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<VirtualMemoryArena>>::failure(
            storage_error(OperationErrorCode::memory_hard_limit,
                L"VirtualMemoryArena::create",
                L"failed to allocate virtual-memory API ownership"));
    }
}

Result<std::unique_ptr<VirtualMemoryArena>>
VirtualMemoryArena::create_with_api(std::uint64_t reservation_size_bytes,
    std::shared_ptr<IVirtualMemoryApi> api,
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker) {
    if (reservation_size_bytes == 0 ||
        reservation_size_bytes % kBlockSizeBytes != 0 ||
        reservation_size_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return Result<std::unique_ptr<VirtualMemoryArena>>::failure(
            storage_error(OperationErrorCode::invalid_range,
                L"VirtualMemoryArena::create_with_api",
                L"reservation must be a non-zero, representable 64 KiB multiple"));
    }
    if (!api) {
        return Result<std::unique_ptr<VirtualMemoryArena>>::failure(
            storage_error(OperationErrorCode::internal_invariant,
                L"VirtualMemoryArena::create_with_api",
                L"virtual-memory API must not be null"));
    }

    try {
        if (!cleanup_tracker) {
            cleanup_tracker = std::make_shared<VirtualMemoryCleanupTracker>();
        }
        auto arena = std::unique_ptr<VirtualMemoryArena>(new VirtualMemoryArena(
            reservation_size_bytes, std::move(api),
            std::move(cleanup_tracker)));
        void* reserved = arena->api_->allocate(nullptr,
            static_cast<std::size_t>(reservation_size_bytes), MEM_RESERVE,
            PAGE_NOACCESS);
        if (reserved == nullptr) {
            const auto native_error = arena->api_->last_error();
            return Result<std::unique_ptr<VirtualMemoryArena>>::failure(
                storage_error(OperationErrorCode::memory_hard_limit,
                    L"VirtualMemoryArena::reserve",
                    L"VirtualAlloc MEM_RESERVE failed", native_error));
        }
        arena->base_address_ = static_cast<std::byte*>(reserved);
        return Result<std::unique_ptr<VirtualMemoryArena>>::success(
            std::move(arena));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<VirtualMemoryArena>>::failure(
            storage_error(OperationErrorCode::memory_hard_limit,
                L"VirtualMemoryArena::create_with_api",
                L"failed to allocate arena ownership"));
    }
}

VirtualMemoryArena::VirtualMemoryArena(
    std::uint64_t reservation_size_bytes,
    std::shared_ptr<IVirtualMemoryApi> api,
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup_tracker) noexcept
    : api_(std::move(api)), cleanup_tracker_(std::move(cleanup_tracker)),
      reservation_size_bytes_(reservation_size_bytes) {}

VirtualMemoryArena::~VirtualMemoryArena() noexcept {
    release_noexcept();
}

VirtualMemoryArena::VirtualMemoryArena(VirtualMemoryArena&& other) noexcept {
    std::scoped_lock lock(other.state_mutex_);
    move_from_locked(other);
}

VirtualMemoryArena& VirtualMemoryArena::operator=(
    VirtualMemoryArena&& other) noexcept {
    if (this == &other) return *this;
    release_noexcept();
    std::scoped_lock lock(state_mutex_, other.state_mutex_);
    move_from_locked(other);
    return *this;
}

void VirtualMemoryArena::move_from_locked(VirtualMemoryArena& other) noexcept {
    api_ = std::move(other.api_);
    cleanup_tracker_ = std::move(other.cleanup_tracker_);
    base_address_ = std::exchange(other.base_address_, nullptr);
    reservation_size_bytes_ =
        std::exchange(other.reservation_size_bytes_, 0);
    committed_blocks_ = std::move(other.committed_blocks_);
    committed_bytes_ = std::exchange(other.committed_bytes_, 0);
    other.committed_blocks_.clear();
}

std::uint64_t VirtualMemoryArena::block_count() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return block_count_locked();
}

std::byte* VirtualMemoryArena::base_address() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return base_address_;
}

std::uint64_t VirtualMemoryArena::reserved_bytes() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return reservation_size_bytes_;
}

std::uint64_t VirtualMemoryArena::block_count_locked() const noexcept {
    return reservation_size_bytes_ / kBlockSizeBytes;
}

std::uint64_t VirtualMemoryArena::committed_bytes() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return committed_bytes_;
}

bool VirtualMemoryArena::is_block_committed(
    std::uint64_t block_index) const noexcept {
    std::scoped_lock lock(state_mutex_);
    return committed_blocks_.contains(block_index);
}

std::shared_ptr<VirtualMemoryCleanupTracker>
VirtualMemoryArena::cleanup_tracker() const {
    std::scoped_lock lock(state_mutex_);
    return cleanup_tracker_;
}

Result<std::byte*> VirtualMemoryArena::block_address(
    std::uint64_t block_index) const {
    std::scoped_lock lock(state_mutex_);
    return block_address_locked(block_index);
}

Result<std::byte*> VirtualMemoryArena::block_address_locked(
    std::uint64_t block_index) const {
    if (base_address_ == nullptr || block_index >= block_count_locked()) {
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::invalid_range,
                L"VirtualMemoryArena::block_address",
                L"block index is outside the reservation"));
    }
    if (block_index >
        (std::numeric_limits<std::uint64_t>::max)() / kBlockSizeBytes) {
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::invalid_range,
                L"VirtualMemoryArena::block_address",
                L"block address multiplication overflow"));
    }
    const auto offset = block_index * kBlockSizeBytes;
    const auto numeric_base = reinterpret_cast<std::uintptr_t>(base_address_);
    if (offset >
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uintptr_t>::max)() - numeric_base)) {
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::invalid_range,
                L"VirtualMemoryArena::block_address",
                L"block address addition overflow"));
    }
    return Result<std::byte*>::success(reinterpret_cast<std::byte*>(
        numeric_base + static_cast<std::uintptr_t>(offset)));
}

Result<std::byte*> VirtualMemoryArena::commit_block(
    std::uint64_t block_index) {
    std::scoped_lock lock(state_mutex_);
    const auto address = block_address_locked(block_index);
    if (!address) return Result<std::byte*>::failure(address.error());
    if (committed_blocks_.contains(block_index)) {
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::internal_invariant,
                L"VirtualMemoryArena::commit_block",
                L"block is already committed"));
    }

    try {
        committed_blocks_.insert(block_index);
    } catch (const std::bad_alloc&) {
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::commit_failed,
                L"VirtualMemoryArena::commit_block",
                L"failed to reserve committed-block metadata",
                ERROR_NOT_ENOUGH_MEMORY));
    }

    void* committed = api_->allocate(address.value(),
        static_cast<std::size_t>(kBlockSizeBytes), MEM_COMMIT,
        PAGE_READWRITE);
    if (committed == nullptr) {
        const auto native_error = api_->last_error();
        committed_blocks_.erase(block_index);
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::commit_failed,
                L"VirtualMemoryArena::commit_block",
                L"VirtualAlloc MEM_COMMIT failed", native_error));
    }
    if (committed != address.value()) {
        api_->free(committed, static_cast<std::size_t>(kBlockSizeBytes),
            MEM_DECOMMIT);
        committed_blocks_.erase(block_index);
        return Result<std::byte*>::failure(
            storage_error(OperationErrorCode::internal_invariant,
                L"VirtualMemoryArena::commit_block",
                L"VirtualAlloc returned an unexpected commit address"));
    }

    committed_bytes_ += kBlockSizeBytes;
    return Result<std::byte*>::success(address.value());
}

Result<void> VirtualMemoryArena::decommit_block(
    std::uint64_t block_index) {
    std::scoped_lock lock(state_mutex_);
    const auto address = block_address_locked(block_index);
    if (!address) return Result<void>::failure(address.error());
    if (!committed_blocks_.contains(block_index)) {
        return Result<void>::failure(
            storage_error(OperationErrorCode::internal_invariant,
                L"VirtualMemoryArena::decommit_block",
                L"block is not committed"));
    }

    if (!api_->free(address.value(),
            static_cast<std::size_t>(kBlockSizeBytes), MEM_DECOMMIT)) {
        return Result<void>::failure(
            storage_error(OperationErrorCode::decommit_failed,
                L"VirtualMemoryArena::decommit_block",
                L"VirtualFree MEM_DECOMMIT failed", api_->last_error()));
    }

    committed_blocks_.erase(block_index);
    committed_bytes_ -= kBlockSizeBytes;
    return Result<void>::success();
}

Result<VirtualMemoryRegionInfo> VirtualMemoryArena::query_block(
    std::uint64_t block_index) const {
    std::scoped_lock lock(state_mutex_);
    const auto address = block_address_locked(block_index);
    if (!address) {
        return Result<VirtualMemoryRegionInfo>::failure(address.error());
    }

    VirtualMemoryRegionInfo information{};
    if (!api_->query(address.value(), information)) {
        return Result<VirtualMemoryRegionInfo>::failure(
            storage_error(OperationErrorCode::internal_invariant,
                L"VirtualMemoryArena::query_block",
                L"VirtualQuery failed", api_->last_error()));
    }
    return Result<VirtualMemoryRegionInfo>::success(information);
}

void VirtualMemoryArena::release_noexcept() noexcept {
    std::shared_ptr<IVirtualMemoryApi> api;
    std::shared_ptr<VirtualMemoryCleanupTracker> cleanup;
    std::byte* base = nullptr;
    {
        std::scoped_lock lock(state_mutex_);
        base = std::exchange(base_address_, nullptr);
        if (base == nullptr) return;
        api = api_;
        cleanup = cleanup_tracker_;
        reservation_size_bytes_ = 0;
        committed_blocks_.clear();
        committed_bytes_ = 0;
    }

    if (!api->free(base, 0, MEM_RELEASE) && cleanup) {
        cleanup->record_release_failure(api->last_error());
    }
}

} // namespace myrd
