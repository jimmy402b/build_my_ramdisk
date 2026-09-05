#pragma once

#include "src/common/result.h"
#include "src/driver/aim_protocol.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::host {

inline constexpr std::uint32_t kStorageDeviceTypeDisk = 0x00000007;

enum class VolumeIdentityError : std::uint32_t {
    invalid_expectation = 0x4E01,
    not_found,
    identity_conflict,
    ambiguous,
    enumeration_failed,
    opened_identity_changed,
};

enum class ObservedPartitionStyle : std::uint8_t {
    raw = 0,
    mbr,
    gpt,
    unknown,
};

struct HostOwnershipMarker final {
    product::Uuid profile_uuid;
    product::Uuid image_uuid;
    std::uint64_t profile_revision{};
    std::uint64_t session_generation{};
    aim::DeviceId aim_device_id{};

    bool operator==(const HostOwnershipMarker&) const noexcept = default;
};

struct PhysicalDiskIdentity final {
    std::uint32_t physical_drive_number{};
    std::wstring path;
    std::uint64_t length_bytes{};
    std::uint32_t port_number{};
    std::uint8_t path_id{};
    std::uint8_t target_id{};
    std::uint8_t lun{};

    bool operator==(const PhysicalDiskIdentity&) const noexcept = default;
};

struct DiskExtentIdentity final {
    std::uint32_t disk_number{};
    std::uint64_t starting_offset{};
    std::uint64_t length_bytes{};

    bool operator==(const DiskExtentIdentity&) const noexcept = default;
};

struct VolumeIdentityExpectation final {
    HostOwnershipMarker owner;
    PhysicalDiskIdentity physical_disk;
    product::Uuid gpt_disk_guid;
    product::Uuid partition_guid;
    product::Uuid volume_guid;
    std::optional<std::uint32_t> volume_serial;

    bool operator==(const VolumeIdentityExpectation&) const noexcept = default;
};

struct VolumeIdentityObservation final {
    std::wstring volume_guid_path;
    product::Uuid volume_guid;
    std::uint32_t volume_serial{};
    std::wstring filesystem_name;
    std::uint32_t storage_device_type{};
    std::uint32_t storage_device_number{};
    std::uint32_t storage_partition_number{};
    std::vector<DiskExtentIdentity> extents;
    PhysicalDiskIdentity physical_disk;
    ObservedPartitionStyle partition_style{ObservedPartitionStyle::unknown};
    product::Uuid gpt_disk_guid;
    std::uint32_t partition_number{};
    product::Uuid partition_guid;
    std::uint64_t partition_starting_offset{};
    std::uint64_t partition_length_bytes{};

    bool operator==(const VolumeIdentityObservation&) const noexcept = default;
};

class VerifiedVolumeIdentity final {
public:
    VerifiedVolumeIdentity(const VerifiedVolumeIdentity&) = default;
    VerifiedVolumeIdentity& operator=(const VerifiedVolumeIdentity&) = default;
    VerifiedVolumeIdentity(VerifiedVolumeIdentity&&) noexcept = default;
    VerifiedVolumeIdentity& operator=(VerifiedVolumeIdentity&&) noexcept =
        default;

    const HostOwnershipMarker& owner() const noexcept;
    const VolumeIdentityObservation& observation() const noexcept;
    const VolumeIdentityExpectation& pinned_expectation() const noexcept;

private:
    VerifiedVolumeIdentity(
        VolumeIdentityExpectation pinned,
        VolumeIdentityObservation observation) noexcept;

    VolumeIdentityExpectation pinned_;
    VolumeIdentityObservation observation_;

    friend Result<VerifiedVolumeIdentity> match_volume_identity(
        const VolumeIdentityExpectation&,
        std::span<const VolumeIdentityObservation>);
};

Result<VerifiedVolumeIdentity> match_volume_identity(
    const VolumeIdentityExpectation& expected,
    std::span<const VolumeIdentityObservation> observations);

Result<VerifiedVolumeIdentity> reverify_volume_identity(
    const VerifiedVolumeIdentity& verified,
    const VolumeIdentityObservation& current);

Result<std::vector<VolumeIdentityObservation>>
enumerate_windows_volume_identities(
    const VolumeIdentityExpectation& expected);

Result<VerifiedVolumeIdentity> verify_windows_volume_identity(
    const VolumeIdentityExpectation& expected);

Result<VolumeIdentityObservation> observe_open_windows_volume_identity(
    std::uintptr_t native_volume_handle,
    std::uintptr_t native_disk_handle,
    const VolumeIdentityExpectation& expected);

} // namespace myrd::host
