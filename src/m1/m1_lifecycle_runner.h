#pragma once

#include "m1_cli.h"
#include "m1_raw_protocol_io.h"
#include "m1_report.h"

#include "src/common/result.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace myrd {

struct M1RunConfig {
    std::string run_id;
    std::string started_utc;
    M1FailurePoint failure_point = M1FailurePoint::none;
    std::chrono::milliseconds cleanup_timeout{15000};
    std::chrono::milliseconds poll_interval{100};
};

class IM1ExecutionGuard {
public:
    virtual ~IM1ExecutionGuard() = default;
    virtual Result<void> require_elevated_and_acquire() = 0;
};

class IM1ProviderLifecycle {
public:
    virtual ~IM1ProviderLifecycle() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop_and_release() = 0;
    virtual std::wstring native_object_path() const = 0;
    virtual proxy::SessionSnapshot snapshot() const = 0;
    virtual Result<bool> named_objects_released() const = 0;
};

class IM1ProviderFactory {
public:
    virtual ~IM1ProviderFactory() = default;
    virtual Result<std::unique_ptr<IM1ProviderLifecycle>> create() = 0;
};

class IM1KernelAdapter {
public:
    virtual ~IM1KernelAdapter() = default;
    virtual Result<M1DriverEvidence> query_driver() = 0;
    virtual Result<std::vector<aim::DeviceId>> list_devices() = 0;
    virtual Result<aim::DeviceId> create_proxy_disk(
        const aim::ProxyDiskSpec& spec) = 0;
    virtual Result<void> rescan_adapter() = 0;
    virtual Result<aim::DiskInfo> query_device(aim::DeviceId id) = 0;
    virtual Result<void> remove_device(aim::DeviceId id) = 0;
    virtual Result<void> wait_until_absent(
        aim::DeviceId id, std::chrono::milliseconds timeout) = 0;
};

class IM1DiskLocator {
public:
    virtual ~IM1DiskLocator() = default;
    virtual Result<std::vector<std::uint32_t>> snapshot_numbers() = 0;
    virtual Result<aim::LocatedDisk> wait_for(
        aim::DeviceId id, std::span<const std::uint32_t> baseline) = 0;
};

class IM1ProtocolVerifier {
public:
    virtual ~IM1ProtocolVerifier() = default;
    virtual Result<M1RawProtocolEvidence> verify(
        const aim::LocatedDisk& disk,
        const proxy::RequestCounters& before,
        const std::function<proxy::RequestCounters()>& counters) = 0;
};

M1Report run_m1_lifecycle(
    IM1ExecutionGuard& execution_guard,
    IM1ProviderFactory& provider_factory,
    IM1KernelAdapter& kernel,
    IM1DiskLocator& locator,
    IM1ProtocolVerifier& protocol,
    const M1RunConfig& config);

} // namespace myrd
