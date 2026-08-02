#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/driver/physical_drive_locator.h"
#include "src/proxy/request_processor.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace myrd {

class IM1RawDiskApi {
public:
    virtual ~IM1RawDiskApi() = default;
    virtual Result<void> open(const aim::LocatedDisk& disk) = 0;
    virtual Result<void> read(std::uint64_t offset,
                              std::span<std::byte> buffer) = 0;
    virtual Result<void> write(std::uint64_t offset,
                               std::span<const std::byte> buffer) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> trim_range(
        std::uint64_t offset, std::uint64_t length) = 0;
};

class WindowsM1RawDiskApi final : public IM1RawDiskApi {
public:
    Result<void> open(const aim::LocatedDisk& disk) override;
    Result<void> read(std::uint64_t offset,
                      std::span<std::byte> buffer) override;
    Result<void> write(std::uint64_t offset,
                       std::span<const std::byte> buffer) override;
    Result<void> flush() override;
    Result<void> trim_range(
        std::uint64_t offset, std::uint64_t length) override;

private:
    Result<void> seek(std::uint64_t offset);
    UniqueHandle handle_;
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
    const aim::LocatedDisk& disk,
    const proxy::RequestCounters& before,
    const std::function<proxy::RequestCounters()>& counters);

} // namespace myrd
