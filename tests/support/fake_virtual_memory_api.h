#pragma once

#include "src/storage/block_constants.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace myrd::test {

enum class FakeVirtualMemoryOperation {
    Reserve,
    Commit,
    Decommit,
    Release,
    Query
};

struct FakeVirtualMemoryAllocateCall {
    void* address = nullptr;
    std::size_t size_bytes = 0;
    std::uint32_t allocation_type = 0;
    std::uint32_t protection = 0;
};

struct FakeVirtualMemoryFreeCall {
    void* address = nullptr;
    std::size_t size_bytes = 0;
    std::uint32_t free_type = 0;
};

class FakeVirtualMemoryApi final : public IVirtualMemoryApi {
public:
    void fail(FakeVirtualMemoryOperation operation,
        std::uint32_t native_error) {
        std::scoped_lock lock(mutex_);
        failures_[operation] = native_error;
    }

    void clear_failure(FakeVirtualMemoryOperation operation) {
        std::scoped_lock lock(mutex_);
        failures_.erase(operation);
    }

    void* allocate(void* address, std::size_t size_bytes,
        std::uint32_t allocation_type,
        std::uint32_t protection) noexcept override {
        std::scoped_lock lock(mutex_);
        allocate_calls_.push_back(
            {address, size_bytes, allocation_type, protection});

        const auto operation = address == nullptr
                                   ? FakeVirtualMemoryOperation::Reserve
                                   : FakeVirtualMemoryOperation::Commit;
        if (set_failure_if_configured(operation)) return nullptr;

        if (operation == FakeVirtualMemoryOperation::Reserve) {
            auto* base = reinterpret_cast<void*>(next_base_);
            reservations_[base] = size_bytes;
            next_base_ += size_bytes + kBlockSizeBytes;
            return base;
        }

        committed_addresses_.insert(address);
        return address;
    }

    bool free(void* address, std::size_t size_bytes,
        std::uint32_t free_type) noexcept override {
        std::scoped_lock lock(mutex_);
        free_calls_.push_back({address, size_bytes, free_type});
        const auto operation = free_type == MEM_RELEASE
                                   ? FakeVirtualMemoryOperation::Release
                                   : FakeVirtualMemoryOperation::Decommit;
        if (set_failure_if_configured(operation)) return false;

        if (operation == FakeVirtualMemoryOperation::Release) {
            reservations_.erase(address);
        } else {
            committed_addresses_.erase(address);
        }
        return true;
    }

    bool query(const void* address,
        VirtualMemoryRegionInfo& information) noexcept override {
        std::scoped_lock lock(mutex_);
        query_addresses_.push_back(address);
        if (set_failure_if_configured(FakeVirtualMemoryOperation::Query)) {
            return false;
        }

        const auto numeric_address = reinterpret_cast<std::uintptr_t>(address);
        for (const auto& [base, size] : reservations_) {
            const auto numeric_base = reinterpret_cast<std::uintptr_t>(base);
            if (numeric_address >= numeric_base &&
                numeric_address - numeric_base < size) {
                information.allocation_base = base;
                information.base_address = address;
                information.region_size_bytes = kBlockSizeBytes;
                information.state = committed_addresses_.contains(
                                        const_cast<void*>(address))
                                        ? VirtualMemoryRegionState::Committed
                                        : VirtualMemoryRegionState::Reserved;
                last_error_ = ERROR_SUCCESS;
                return true;
            }
        }

        last_error_ = ERROR_INVALID_ADDRESS;
        return false;
    }

    std::uint32_t last_error() const noexcept override {
        std::scoped_lock lock(mutex_);
        return last_error_;
    }

    std::vector<FakeVirtualMemoryAllocateCall> allocate_calls() const {
        std::scoped_lock lock(mutex_);
        return allocate_calls_;
    }

    std::vector<FakeVirtualMemoryFreeCall> free_calls() const {
        std::scoped_lock lock(mutex_);
        return free_calls_;
    }

    std::size_t query_count() const {
        std::scoped_lock lock(mutex_);
        return query_addresses_.size();
    }

private:
    struct EnumHash {
        std::size_t operator()(FakeVirtualMemoryOperation value) const {
            return static_cast<std::size_t>(value);
        }
    };

    bool set_failure_if_configured(FakeVirtualMemoryOperation operation) {
        const auto found = failures_.find(operation);
        if (found == failures_.end()) {
            last_error_ = ERROR_SUCCESS;
            return false;
        }
        last_error_ = found->second;
        return true;
    }

    mutable std::mutex mutex_;
    std::uintptr_t next_base_ = 0x1'0000'0000ull;
    std::unordered_map<void*, std::size_t> reservations_;
    std::unordered_set<void*> committed_addresses_;
    std::unordered_map<FakeVirtualMemoryOperation, std::uint32_t, EnumHash>
        failures_;
    std::vector<FakeVirtualMemoryAllocateCall> allocate_calls_;
    std::vector<FakeVirtualMemoryFreeCall> free_calls_;
    std::vector<const void*> query_addresses_;
    std::uint32_t last_error_ = ERROR_SUCCESS;
};

} // namespace myrd::test
