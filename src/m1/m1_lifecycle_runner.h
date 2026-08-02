#pragma once

#include "m1_provider.h"
#include "m1_raw_protocol_io.h"
#include "m1_report.h"

#include "src/common/result.h"
#include "src/driver/direct_aim_adapter.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace myrd {

struct M1RunConfig {
    std::string run_id;
    bool debug_failure_point = false;
    int failure_point_stage = 0;
};

// Injectable boundaries for testing
class IM1ProviderFactory {
public:
    virtual ~IM1ProviderFactory() = default;
    virtual Result<std::unique_ptr<M1Provider>> create_and_start(
        const std::wstring& native_path) = 0;
    virtual Result<void> stop_and_release(M1Provider& provider) = 0;
};

class IM1KernelAdapter {
public:
    virtual ~IM1KernelAdapter() = default;
    virtual Result<std::vector<aim::DeviceId>> list_devices() = 0;
    virtual Result<aim::DeviceId> create_proxy_disk(
        const std::wstring& native_path,
        std::uint64_t size_bytes, std::uint32_t sector_size) = 0;
    virtual Result<void> remove_device(aim::DeviceId id) = 0;
    virtual Result<void> wait_until_absent(
        aim::DeviceId id, std::chrono::milliseconds timeout) = 0;
};

Result<M1Report> run_m1_lifecycle(
    IM1ProviderFactory& provider_factory,
    IM1KernelAdapter& kernel,
    IM1RawDiskApi& raw_disk,
    const M1RunConfig& config);

} // namespace myrd
