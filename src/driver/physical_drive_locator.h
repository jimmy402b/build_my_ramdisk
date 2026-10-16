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
Result<std::vector<LocatedDisk>> enumerate_present_physical_drives();

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::chrono::milliseconds timeout);

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout);

// Waits until the PhysicalDrive backed by `created` is present, whether or not this process
// created it.
//
// The mirror of `wait_for_created_physical_drive` for the **attach** case: a reclaim adopts the
// proxy disk a killed worker left behind, so that disk is already in the machine and the create
// wait would never see it as newly created -- it skips anything marked pre-existing.  Identity is
// decided from the SCSI address and the expected length instead, which is the same rule
// `wait_for_physical_drive_retired` states for the opposite direction.
Result<LocatedDisk> wait_for_present_physical_drive(
    DeviceId created, std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout);

// Waits until the PhysicalDrive created for `owned` is gone from the machine.
//
// This is the mirror of `wait_for_created_physical_drive`, and it is deliberately
// about *that device's identity* rather than about the machine's disk set.  A
// teardown that waits for the inventory to return to some earlier snapshot of
// the whole machine can never finish while a disk created after that snapshot is
// still there -- which is the ordinary case for a second RAM disk, not an exotic
// one.  Absence is decided from the SCSI address and the expected length alone;
// see the definition for why `existed_before` must not be consulted here.
Result<void> wait_for_physical_drive_retired(
    DeviceId owned, std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout);

} // namespace myrd::aim
