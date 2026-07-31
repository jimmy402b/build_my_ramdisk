#pragma once

#include "aim_protocol.h"
#include "srb_transport.h"
#include "src/common/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace myrd::aim {

struct DriverVersion { std::uint32_t raw; };

struct DiskInfo {
    DeviceId id;
    std::uint64_t size_bytes;
    std::uint32_t sector_size;
    std::uint32_t flags;
    std::wstring native_object_path;
};

class DirectAimAdapter final {
public:
    explicit DirectAimAdapter(ISrbTransport& transport) noexcept;

    Result<DriverVersion> query_version();
    Result<std::vector<DeviceId>> list_devices();
    Result<DeviceId> create_proxy_disk(const ProxyDiskSpec& spec);
    Result<DiskInfo> query_device(DeviceId id);
    Result<void> remove_device(DeviceId id);

private:
    ISrbTransport& transport_;
};

} // namespace myrd::aim
