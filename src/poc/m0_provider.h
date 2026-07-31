#pragma once

#include "lifecycle_runner.h"
#include "poc_report.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/proxy/object_names.h"
#include "src/proxy/shared_memory_channel.h"
#include "src/proxy/shm_proxy_server.h"
#include "src/storage/memory_block_device.h"

namespace myrd::poc {

class M0Provider final : public IProviderLifecycle {
public:
    M0Provider();
    Result<void> start() override;
    Result<void> stop_and_release() override;
    std::wstring native_object_path() const override;
    proxy::RequestCounters counters() const override;
    Result<bool> named_objects_released() const override;

    proxy::ObjectNames names() const { return names_; }

private:
    MemoryBlockDevice device_{
        aim::kM0DiskSize, aim::kM0SectorSize};
    proxy::RequestProcessor processor_{device_};
    proxy::ObjectNames names_;
    std::optional<proxy::SharedMemoryChannel> channel_;
    std::optional<proxy::ShmProxyServer> server_;
};

} // namespace myrd::poc
