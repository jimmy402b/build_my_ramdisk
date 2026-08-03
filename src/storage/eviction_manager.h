#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace myrd {

class DynamicBlockStore;
struct OperationContext;

struct EvictionPassResult {
    std::uint64_t scanned_blocks{};
    std::uint64_t cleared_reference_bits{};
    std::uint64_t evicted_blocks{};
    std::uint64_t decommit_failures{};
    std::uint64_t skipped_dirty_blocks{};
    std::uint64_t skipped_zero_blocks{};
    std::uint64_t skipped_transient_blocks{};
    std::uint64_t skipped_pinned_blocks{};
    std::uint64_t skipped_generation_blocks{};
    std::uint64_t invariant_failures{};
    bool target_reached{};
    bool closing_observed{};
    bool waited_for_scan{};
    bool request_interrupted{};
};

// Owns the persistent CLOCK cursor and serializes all eviction passes. It
// never performs backing-store I/O: a victim is eligible only when the current
// immutable backing view can reconstruct it.
class EvictionManager final {
public:
    static std::unique_ptr<EvictionManager> create(
        DynamicBlockStore& store) noexcept;

    EvictionManager(const EvictionManager&) = delete;
    EvictionManager& operator=(const EvictionManager&) = delete;

    EvictionPassResult sweep_once(std::uint64_t target_bytes);
    EvictionPassResult reclaim_to(
        std::uint64_t target_bytes,
        const OperationContext* context = nullptr);
    std::uint64_t cursor() const;
    std::uint64_t synchronous_waiters() const noexcept;

private:
    explicit EvictionManager(DynamicBlockStore& store) noexcept
        : store_(store) {}

    EvictionPassResult sweep_once_locked(
        std::uint64_t target_bytes,
        const OperationContext* context);
    bool committed_at_or_below(std::uint64_t target_bytes) const;

    DynamicBlockStore& store_;
    mutable std::timed_mutex scan_mutex_;
    std::atomic<std::uint64_t> synchronous_waiters_{0};
    std::uint64_t next_block_index_{};
};

} // namespace myrd
