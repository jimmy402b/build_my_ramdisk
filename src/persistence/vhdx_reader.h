#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/file_io.h"
#include "src/persistence/vhdx_format.h"
#include "src/storage/backing_store.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace myrd::persistence {

class ValidatedPersistenceFile;

inline constexpr std::uint64_t kVhdxBatPageBytes = 4096;

struct VhdxBatCacheOptions final {
    std::uint64_t max_bytes{0};
    std::uint64_t max_entries{0};
};

struct VhdxReaderOptions final {
    VhdxBatCacheOptions bat_cache{};
    std::uint64_t max_extents{0};
    std::uint64_t max_observed_payloads{0};
};

struct VhdxReaderStats final {
    std::uint64_t bat_cache_hits{0};
    std::uint64_t bat_cache_misses{0};
    std::uint64_t bat_cache_evictions{0};
    std::uint64_t bat_read_calls{0};
    std::uint64_t payload_read_calls{0};
    std::uint64_t current_cache_entries{0};
    std::uint64_t current_cache_bytes{0};
    std::uint64_t peak_cache_entries{0};
    std::uint64_t peak_cache_bytes{0};
};

struct VhdxAllocatedExtent final {
    std::uint64_t virtual_offset{0};
    std::uint64_t length{0};

    bool operator==(const VhdxAllocatedExtent&) const noexcept = default;
};

class VhdxReader final {
public:
    // Parse and bind one validated handle exactly once. Recovery uses this
    // entry point so startup performs one fixed structural capture rather
    // than parsing once for validation and again for reader construction.
    static Result<std::unique_ptr<VhdxReader>> open(
        const OperationContext& context,
        std::unique_ptr<ValidatedPersistenceFile>& file,
        VhdxReaderOptions options,
        VhdxParseRequirements requirements = {},
        VhdxParseEvidence* evidence = nullptr);

    static Result<std::unique_ptr<VhdxReader>> create(
        std::unique_ptr<ValidatedPersistenceFile> file,
        VhdxLayout layout,
        VhdxReaderOptions options);
    static Result<std::unique_ptr<VhdxReader>> create(
        const OperationContext& context,
        std::unique_ptr<ValidatedPersistenceFile> file,
        VhdxLayout layout,
        VhdxReaderOptions options);

#if defined(MYRD_ENABLE_TEST_SEAMS)
    static Result<std::unique_ptr<VhdxReader>> create_for_testing(
        std::unique_ptr<IPersistenceFile> file,
        WindowsFileIdentity windows_identity,
        VhdxLayout layout,
        VhdxReaderOptions options);
    static Result<std::unique_ptr<VhdxReader>> create_for_testing(
        const OperationContext& context,
        std::unique_ptr<IPersistenceFile> file,
        WindowsFileIdentity windows_identity,
        VhdxLayout layout,
        VhdxReaderOptions options);
#endif

    ~VhdxReader();
    VhdxReader(VhdxReader&&) noexcept;
    VhdxReader& operator=(VhdxReader&&) noexcept;
    VhdxReader(const VhdxReader&) = delete;
    VhdxReader& operator=(const VhdxReader&) = delete;

    Result<BlockLoadResult> read_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const;

    // Returns the first M2 block >= first_block covered by a FullyPresent BAT
    // payload. Empty VHDX payload ranges are skipped by BAT metadata, not by
    // reading every logical 64 KiB block.
    Result<std::optional<std::uint64_t>> next_allocated_block(
        const OperationContext& context,
        std::uint64_t first_block) const;

    Result<std::vector<VhdxAllocatedExtent>> enumerate_allocated_extents(
        const OperationContext& context) const;

    [[nodiscard]] VhdxReaderStats stats() const;
    [[nodiscard]] const VhdxLayout& layout() const noexcept;
    [[nodiscard]] WindowsFileIdentity windows_identity() const noexcept;
    void clear_cache();

private:
    struct Impl;
    explicit VhdxReader(std::unique_ptr<Impl> impl) noexcept;
    static Result<std::unique_ptr<VhdxReader>> create_owned(
        const OperationContext& context,
        std::unique_ptr<IPersistenceFile> file,
        WindowsFileIdentity windows_identity,
        VhdxLayout layout,
        VhdxReaderOptions options);
    static Result<std::unique_ptr<VhdxReader>> create_parsed(
        const OperationContext& context,
        std::unique_ptr<IPersistenceFile> file,
        WindowsFileIdentity windows_identity,
        VhdxLayout layout,
        VhdxReaderOptions options);
    std::unique_ptr<Impl> impl_;
};

}  // namespace myrd::persistence
