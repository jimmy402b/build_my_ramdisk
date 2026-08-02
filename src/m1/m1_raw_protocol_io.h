#pragma once

#include "src/common/result.h"
#include "src/proxy/request_processor.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace myrd {

class IM1RawDiskApi {
public:
    virtual ~IM1RawDiskApi() = default;
    virtual Result<void> open(const std::wstring& path) = 0;
    virtual Result<void> read(std::uint64_t offset,
                              std::span<std::byte> buffer) = 0;
    virtual Result<void> write(std::uint64_t offset,
                               std::span<const std::byte> buffer) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> trim(std::uint64_t offset, std::uint64_t length) = 0;
};

struct M1RawProtocolEvidence {
    std::uint64_t offset;
    std::uint64_t length;
    std::string write_sha256;
    std::string read_sha256;
    bool zero_write_verified;
    bool unmap_verified;
    bool flush_succeeded;
};

Result<M1RawProtocolEvidence> verify_m1_raw_protocol(
    IM1RawDiskApi& api,
    std::uint64_t device_size,
    std::uint32_t sector_size,
    const proxy::RequestCounters& before,
    const std::function<proxy::RequestCounters()>& counters);

} // namespace myrd
