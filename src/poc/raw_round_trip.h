#pragma once

#include "lifecycle_runner.h"
#include "poc_report.h"
#include "src/common/result.h"
#include "src/driver/physical_drive_locator.h"

#include <cstdint>
#include <span>

namespace myrd::poc {

class IRawDiskIo {
public:
    virtual ~IRawDiskIo() = default;
    virtual Result<void> verify_identity(const aim::LocatedDisk&) = 0;
    virtual Result<void> seek(std::uint64_t offset) = 0;
    virtual Result<void> write_exact(std::span<const std::byte>) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> read_exact(std::span<std::byte>) = 0;
};

class RawRoundTrip final : public IRoundTrip {
public:
    explicit RawRoundTrip(IRawDiskIo& io) noexcept;
    Result<RawRoundTripEvidence> run(const aim::LocatedDisk&) override;

private:
    IRawDiskIo& io_;
};

} // namespace myrd::poc
