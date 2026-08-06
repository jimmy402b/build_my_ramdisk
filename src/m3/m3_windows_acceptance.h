#pragma once

#include "src/m1/m1_lifecycle_runner.h"
#include "src/m3/m3_provider.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace myrd::m3 {

struct M3AimProviderFactoryConfig final {
    OperationContext context{};
    M3ProviderConfig provider{};
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
    aim::LocatedDisk disk{};
    std::uint64_t virtual_bytes{};
    std::uint32_t sector_size{};
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
    Result<void> stop();

    [[nodiscard]] DynamicBlockStoreStats storage_stats() const;
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

    M3AimProviderFactoryConfig config_;
    std::chrono::milliseconds device_timeout_;
    std::unique_ptr<M3Provider> provider_;
    std::optional<aim::AdapterHandle> adapter_handle_;
    std::unique_ptr<aim::WindowsSrbTransport> transport_;
    std::unique_ptr<aim::DirectAimAdapter> adapter_;
    std::optional<std::vector<std::uint32_t>> physical_baseline_;
    std::optional<aim::DeviceId> owned_device_;
    M3AimRuntimeInfo info_{};
    bool started_{};
    bool device_removed_{};
    bool physical_drive_restored_{};
};

} // namespace myrd::m3
