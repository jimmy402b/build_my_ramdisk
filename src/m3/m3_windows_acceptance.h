#pragma once

#include "src/m1/m1_lifecycle_runner.h"
#include "src/m3/m3_provider.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"
#include "src/persistence/production_policy.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace myrd::m3 {

// One bounded production resource policy shared by the standalone acceptance
// server and the product worker. Callers may replace only profile-owned memory
// limits after obtaining these finite defaults.
DynamicBlockStoreOptions production_store_options() noexcept;

struct M3AimProviderFactoryConfig final {
    OperationContext context{};
    M3ProviderConfig provider{};

    // **Set when this runtime attaches to a volume that already exists rather than creating one.**
    //
    // True for `recover` and `reclaim`, false for `create`.  It exists because those two do not need to
    // know which physical drive the AIM device became, and obtaining that knowledge is what blocked:
    // `wait_for_present_physical_drive` opened every candidate disk, and one of those opens took 11017 ms
    // while a device was retiring.
    //
    // A create still needs the number -- it provisions the volume on that disk -- so the flag only
    // governs the attach-only case.
    bool attach_only{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    M3ProviderFactories test_factories{};
#endif
};

// Composition root adapter: the M3 provider owns the M1-compatible SHM
// endpoint and exposes its driver object path to the AIM lifecycle runner.
// No AIM or Windows disk mutation occurs in this factory itself.
class M3AimProviderFactory final : public IM1ProviderFactory {
public:
    explicit M3AimProviderFactory(M3AimProviderFactoryConfig config);

    Result<std::unique_ptr<IM1ProviderLifecycle>> create() override;

private:
    M3AimProviderFactoryConfig config_;
};

struct M3AimRuntimeInfo final {
    aim::DeviceId device_id{};
    // **Optional because an attach-only runtime never locates the disk.**
    //
    // `recover` and `reclaim` skip the physical-drive wait: it only produced this value, and after the
    // drive-number checks were removed from find and verify, nothing on those paths consumes it.  A
    // default-constructed `LocatedDisk` would mean *physical drive 0*, which is a real disk and a false
    // claim, so absence is expressed rather than defaulted.
    //
    // Callers that need the number -- only the create path, which provisions the volume on that disk --
    // must handle its absence instead of reading a zero.
    std::optional<aim::LocatedDisk> disk{};
    std::uint64_t virtual_bytes{};
    std::uint32_t sector_size{};
    std::wstring native_object_path;
};

// Owns the complete real-host composition while an AIM disk is visible:
// M3 provider -> AIM proxy disk -> PhysicalDrive. Checkpoint is intentionally
// separate from stop so the caller can flush the filesystem/volume first.
class M3AimRuntime final {
public:
    static Result<std::unique_ptr<M3AimRuntime>> create(
        M3AimProviderFactoryConfig config,
        std::chrono::milliseconds device_timeout =
            std::chrono::seconds(30));

    ~M3AimRuntime() noexcept;
    M3AimRuntime(const M3AimRuntime&) = delete;
    M3AimRuntime& operator=(const M3AimRuntime&) = delete;

    Result<void> start();
    M3CheckpointOutcome checkpoint(
        const OperationContext& context,
        persistence::VolumeBarrierKind barrier);

    // M5 two-phase checkpoint.  These expose the provider's explicit
    // prepare/publish boundary to the per-profile worker; the foreground run
    // loop keeps the one-shot checkpoint() above and never calls these.
    Result<M3CheckpointPrepared> prepare_checkpoint(
        const M3CheckpointPrepareRequest& request);
    M3CheckpointOutcome commit_checkpoint(
        const M3CheckpointCommitRequest& request);
    Result<void> abort_checkpoint(std::uint64_t native_operation_id);

    // The online-compaction half of the same boundary.  The runtime must be
    // started: compaction folds the live store and its journal, not a bundle
    // opened read-only.
    Result<M3CompactionPrepared> prepare_compaction(
        const OperationContext& context, M3CompactionRequest request);
    M3CompactionOutcome commit_compaction(
        const OperationContext& context, std::uint64_t native_operation_id,
        std::uint64_t cut_dirty_epoch);
    Result<void> abort_compaction(std::uint64_t native_operation_id);
    M3CompactionOutcome reconcile_compaction(
        const OperationContext& context,
        const M3CompactionBoundaryEvidence& boundary);

    Result<void> stop();

    [[nodiscard]] DynamicBlockStoreStats storage_stats() const;
    [[nodiscard]] std::uint64_t memory_budget_bytes() const;
    [[nodiscard]] proxy::SessionSnapshot transport_snapshot() const;

    [[nodiscard]] const M3AimRuntimeInfo& info() const noexcept {
        return info_;
    }
    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    M3AimRuntime(
        M3AimProviderFactoryConfig config,
        std::chrono::milliseconds device_timeout);

    Result<void> remove_owned_device();
    Result<void> stop_provider();
    // The device this provider's bundle is already backed by, if the driver still holds one.
    //
    // Exists for the reclaim path: a killed worker leaves its proxy disk behind, and adopting
    // that device lets the reclaimer avoid creating a second one and re-enumerating the
    // adapter -- a PnP call with no timeout, which is what used to block forever.  Matched on
    // size, sector size and backing file, so a device belonging to another profile is never
    // taken over.
    Result<std::optional<aim::DeviceId>> find_existing_device(
        const aim::ProxyDiskSpec& spec);
    // One trace line each for "what we are looking for" and "what the driver holds", because
    // a missing device and a mismatched identity are opposite problems and the first version
    // reported both as the same "not found".
    static std::string describe_device_target(const aim::ProxyDiskSpec& spec);
    static std::string describe_device_candidate(const aim::DiskInfo& candidate);

    M3AimProviderFactoryConfig config_;
    std::chrono::milliseconds device_timeout_;
    std::unique_ptr<M3Provider> provider_;
    std::optional<aim::AdapterHandle> adapter_handle_;
    std::unique_ptr<aim::WindowsSrbTransport> transport_;
    std::unique_ptr<aim::DirectAimAdapter> adapter_;
    // `physical_baseline_` is gone with the whole-machine enumeration that filled it.  Keeping a
    // member nothing writes would be worse than removing it: the next reader would reasonably assume
    // something still maintains it.
    std::optional<aim::DeviceId> owned_device_;
    M3AimRuntimeInfo info_{};
    bool started_{};
    bool device_removed_{};
    bool physical_drive_restored_{};
};

} // namespace myrd::m3
