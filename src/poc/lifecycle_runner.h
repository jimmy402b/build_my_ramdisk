#pragma once

#include "cli.h"
#include "poc_report.h"
#include "src/common/result.h"
#include "src/driver/aim_protocol.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/proxy/request_processor.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::poc {

struct RunConfig {
    std::string run_id;
    bool debug_build = false;
    FailurePoint injected_failure = FailurePoint::none;
    std::string report_path;
};

// Inject interfaces for testing

class IProviderLifecycle {
public:
    virtual ~IProviderLifecycle() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop_and_release() = 0;
    virtual std::wstring native_object_path() const = 0;
    virtual proxy::RequestCounters counters() const = 0;
    virtual Result<bool> named_objects_released() const = 0;
};

class IKernelDiskAdapter {
public:
    virtual ~IKernelDiskAdapter() = default;
    virtual Result<std::vector<aim::DeviceId>> list_devices() = 0;
    virtual Result<aim::DeviceId> create_proxy_disk(
        const aim::ProxyDiskSpec&) = 0;
    virtual Result<aim::DiskInfo> query_device(aim::DeviceId) = 0;
    virtual Result<void> remove_device(aim::DeviceId) = 0;
    virtual Result<void> wait_until_absent(
        aim::DeviceId, std::chrono::milliseconds) = 0;
};

class IDiskLocator {
public:
    virtual ~IDiskLocator() = default;
    virtual Result<std::vector<std::uint32_t>> snapshot_numbers() = 0;
    virtual Result<aim::LocatedDisk> wait_for(
        aim::DeviceId, std::span<const std::uint32_t>) = 0;
};

class IRoundTrip {
public:
    virtual ~IRoundTrip() = default;
    virtual Result<RawRoundTripEvidence> run(const aim::LocatedDisk&) = 0;
};

enum class LifecycleStage : std::uint8_t {
    started, preflight_passed, baseline_captured, provider_ready,
    create_returned, device_confirmed, physical_drive_confirmed,
    round_trip_verified, device_removed, provider_stopped,
    cleanup_verified, report_written
};

PocReport run_lifecycle(
    IProviderLifecycle& provider,
    IKernelDiskAdapter& adapter,
    IDiskLocator& locator,
    IRoundTrip& round_trip,
    const RunConfig& config);

} // namespace myrd::poc
