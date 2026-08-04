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
    std::uint8_t  path_id = 0;
    std::uint8_t  target_id = 0;
    std::uint8_t  lun = 0;
};

std::uint32_t disk_candidate_open_access() noexcept;

bool same_located_disk_identity(
    const LocatedDisk& left, const LocatedDisk& right) noexcept;

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates);

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates,
    std::uint64_t expected_size_bytes);

Result<std::vector<std::uint32_t>> snapshot_physical_drive_numbers();

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::chrono::milliseconds timeout);

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout);

} // namespace myrd::aim
