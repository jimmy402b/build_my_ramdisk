#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/composite_backing_store.h"
#include "src/storage/dynamic_block_store.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace myrd::persistence {

struct LiveExtentEnumeratorOptions final {
    // Finite upper bound for one emitted/read-back DATA extent. Must be a
    // positive multiple of the M2 block size.
    std::uint64_t max_extent_bytes{};
};

struct LiveDataExtent final {
    ImageUuid image_uuid{};
    CheckpointGeneration checkpoint_generation{};
    BackingGeneration backing_generation{};
    std::uint64_t logical_offset{};
    std::uint64_t length{};
    std::uint32_t crc32c{};
    Sha256Digest digest{};

    bool operator==(const LiveDataExtent&) const noexcept = default;
};

struct LiveExtentEnumerationMetrics final {
    std::uint64_t candidate_blocks_examined{};
    std::uint64_t data_blocks{};
    std::uint64_t zero_blocks{};
    std::uint64_t extents_emitted{};
    std::uint64_t bytes_hashed{};
    std::uint64_t peak_scratch_bytes{};
};

using LiveExtentConsumer =
    std::function<Result<void>(const LiveDataExtent&)>;

// Captures one immutable BackingView at construction. Enumeration and verified
// payload reads never consult mutable RAM or follow a later backing generation.
class LiveExtentEnumerator final {
public:
    static Result<std::unique_ptr<LiveExtentEnumerator>> create(
        BackingView captured_view,
        CompositeBackingIdentity identity,
        LiveExtentEnumeratorOptions options);

    ~LiveExtentEnumerator();
    LiveExtentEnumerator(const LiveExtentEnumerator&) = delete;
    LiveExtentEnumerator& operator=(const LiveExtentEnumerator&) = delete;
    LiveExtentEnumerator(LiveExtentEnumerator&&) = delete;
    LiveExtentEnumerator& operator=(LiveExtentEnumerator&&) = delete;

    Result<LiveExtentEnumerationMetrics> enumerate(
        const OperationContext& context,
        const LiveExtentConsumer& consume) const;

    // Reloads from the captured immutable view and verifies descriptor source
    // identity, CRC32C and SHA-256 before the caller may consume the payload.
    Result<void> read_verified_extent(
        const OperationContext& context,
        const LiveDataExtent& extent,
        std::span<std::byte> destination) const;

    [[nodiscard]] const CompositeBackingIdentity& identity() const noexcept;
    [[nodiscard]] BackingGeneration backing_generation() const noexcept;

private:
    struct Impl;
    explicit LiveExtentEnumerator(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace myrd::persistence
