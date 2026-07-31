#pragma once

#include "aim_protocol.h"
#include "src/common/result.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace myrd::aim {

struct DiskCandidate {
    std::uint8_t  path_id;
    std::uint8_t  target_id;
    std::uint8_t  lun;
    std::uint32_t physical_drive_number;
    std::uint64_t length_bytes;
    bool          existed_before;
};

struct LocatedDisk {
    std::uint32_t physical_drive_number;
    std::wstring  path;
    std::uint64_t length_bytes;
};

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates);

Result<std::vector<std::uint32_t>> snapshot_physical_drive_numbers();

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::chrono::milliseconds timeout);

} // namespace myrd::aim
