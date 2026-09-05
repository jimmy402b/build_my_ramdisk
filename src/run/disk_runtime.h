#pragma once

#include "src/common/result.h"
#include "src/run/windows_volume.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace myrd {

enum class DiskBundleKind : std::uint8_t {
    empty,
    manifest,
    unknown_nonempty,
};

enum class DiskRuntimeMode : std::uint8_t { create, recover };

enum class DiskCheckpointDisposition : std::uint8_t {
    committed,
    not_committed,
    commit_indeterminate,
};

struct DiskCheckpointResult final {
    DiskCheckpointDisposition disposition{
        DiskCheckpointDisposition::not_committed};
    std::uint64_t generation{};
};

struct DiskMaintenanceStats final {
    std::uint64_t dirty_bytes{};
    std::uint64_t journal_bytes{};
    std::uint64_t generation{};
};

struct DiskRuntimeOptions final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::milliseconds volume_timeout{30000};
};

// T06 deliberately exposes callbacks instead of production objects.  This
// keeps lifecycle tests deterministic and lets T07 adapt the already accepted
// M3 and Windows-volume implementations without changing their contracts.
struct DiskRuntimeOperations final {
    std::function<Result<void>()> preflight;
    std::function<Result<DiskBundleKind>(const std::filesystem::path&)>
        classify_bundle;
    std::function<Result<void>(const std::filesystem::path&)> acquire_bundle_lock;
    std::function<Result<void>()> release_bundle_lock;

    std::function<Result<void>(std::uint64_t)> create_generation_zero_image;
    std::function<Result<void>(std::uint64_t)> recover_complete_image;
    std::function<Result<void>()> start_aim;
    std::function<Result<void>()> stop_aim;

    std::function<Result<ProductVolume>(std::uint64_t, wchar_t)>
        provision_new_volume;
    std::function<Result<ProductVolume>(wchar_t)> find_recovered_volume;
    std::function<Result<void>(const ProductVolume&, std::uint64_t)>
        verify_volume;
    std::function<Result<void>(const ProductVolume&)> flush_volume;
    std::function<Result<void>(const ProductVolume&, wchar_t)>
        assign_drive_letter;
    std::function<Result<void>(const ProductVolume&)> dismount_volume;
    std::function<Result<void>(const ProductVolume&)> remove_drive_letter;

    std::function<Result<DiskCheckpointResult>()> checkpoint;
    std::function<DiskMaintenanceStats()> maintenance_stats;
};

class DiskRuntime final {
public:
    static Result<std::unique_ptr<DiskRuntime>> create(
        DiskRuntimeOptions options, DiskRuntimeOperations operations);
    static Result<std::unique_ptr<DiskRuntime>> recover(
        DiskRuntimeOptions options, DiskRuntimeOperations operations);

    ~DiskRuntime() noexcept;
    DiskRuntime(const DiskRuntime&) = delete;
    DiskRuntime& operator=(const DiskRuntime&) = delete;

    // Performs the mode-specific lifecycle. Create must establish generation
    // zero, provision and verify the new volume, flush it, then commit gen 1.
    // Recover must never provision: it recovers M3 using options_.virtual_bytes,
    // finds an existing volume, verifies its geometry against that same size,
    // then assigns the requested drive letter.
    //
    // A failed stage owns no later resources. Cleanup is reverse-order and
    // idempotent: remove a drive letter only when this runtime assigned it,
    // dismount only when this runtime obtained a volume, stop AIM only after
    // AIM started, and release the bundle lock only after lock acquisition.
    Result<void> start();
    [[nodiscard]] DiskMaintenanceStats maintenance_stats() const;
    // Flushes the mounted Windows volume before a persistence checkpoint.
    // This is intentionally separate from checkpoint() so the M4 run loop
    // can enforce the filesystem durability ordering.
    Result<void> flush_volume();
    // May only be called after start() succeeds. The provider's disposition is
    // returned verbatim, including not_committed and commit_indeterminate;
    // callers must not infer commitment from a successful API call alone.
    Result<DiskCheckpointResult> checkpoint();
    // May only be called after a successful start (or after a failed start has
    // completed its owned-resource cleanup). Repeated calls are harmless.
    Result<void> shutdown_clean();

private:
    DiskRuntime(DiskRuntimeMode mode, DiskRuntimeOptions options,
                DiskRuntimeOperations operations);

    DiskRuntimeMode mode_;
    DiskRuntimeOptions options_;
    DiskRuntimeOperations operations_;
    std::optional<ProductVolume> volume_;
    bool bundle_lock_held_{};
    bool aim_started_{};
    bool drive_letter_assigned_{};
    bool volume_dismounted_{};
    bool started_{};
    bool cleanup_attempted_{};
};

} // namespace myrd
