#pragma once

#include "lifecycle_runner.h"
#include "poc_report.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/proxy/object_names.h"
#include "src/proxy/shared_memory_channel.h"
#include "src/proxy/shm_proxy_server.h"
#include "src/storage/memory_block_device.h"

namespace myrd::poc {

// M0 predates range-operation capability advertisement. Keep its wire-visible
// INFO response frozen while the shared MemoryBlockDevice gains M1 UNMAP/ZERO
// support.
class M0CompatibilityBlockDevice final : public IBlockDevice {
public:
    M0CompatibilityBlockDevice(
        std::uint64_t size_bytes, std::uint32_t sector_size)
        : inner_(size_bytes, sector_size) {}

    std::uint64_t size_bytes() const noexcept override {
        return inner_.size_bytes();
    }
    std::uint32_t sector_size() const noexcept override {
        return inner_.sector_size();
    }
    DeviceFeatures features() const noexcept override {
        return {false, false, false};
    }
    Result<void> read(const OperationContext& context,
        std::uint64_t offset, std::span<std::byte> output) override {
        return inner_.read(context, offset, output);
    }
    Result<void> write(const OperationContext& context,
        std::uint64_t offset, std::span<const std::byte> input) override {
        return inner_.write(context, offset, input);
    }
    Result<void> unmap(const OperationContext& context,
        std::span<const DataRange> ranges) override {
        return inner_.unmap(context, ranges);
    }
    Result<void> zero(const OperationContext& context,
        std::span<const DataRange> ranges) override {
        return inner_.zero(context, ranges);
    }

private:
    MemoryBlockDevice inner_;
};

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
    M0CompatibilityBlockDevice device_{
        aim::kM0DiskSize, aim::kM0SectorSize};
    proxy::RequestProcessor processor_{device_};
    proxy::ObjectNames names_;
    std::optional<proxy::ShmProxyServer> server_;
};

} // namespace myrd::poc
