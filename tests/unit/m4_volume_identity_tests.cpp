#include "tests/support/test_framework.h"

#include "src/host/volume_identity.h"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::host;

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

HostOwnershipMarker owner() {
    return {
        uuid("00112233-4455-4677-8899-aabbccddeeff"),
        uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
        3,
        7,
        aim::DeviceId{0x00030201}};
}

PhysicalDiskIdentity disk() {
    return {42, L"\\\\.\\PhysicalDrive42", 8ULL << 20, 11, 1, 2, 3};
}

VolumeIdentityExpectation expectation() {
    return {
        owner(),
        disk(),
        uuid("20000000-0000-4000-8000-000000000001"),
        uuid("20000000-0000-4000-8000-000000000002"),
        uuid("20000000-0000-4000-8000-000000000003"),
        std::nullopt};
}

VolumeIdentityObservation exact_observation() {
    return {
        L"\\\\?\\Volume{20000000-0000-4000-8000-000000000003}\\",
        uuid("20000000-0000-4000-8000-000000000003"),
        0xA1B2C3D4,
        L"NTFS",
        kStorageDeviceTypeDisk,
        42,
        1,
        {{42, 1ULL << 20, 7ULL << 20}},
        disk(),
        ObservedPartitionStyle::gpt,
        uuid("20000000-0000-4000-8000-000000000001"),
        1,
        uuid("20000000-0000-4000-8000-000000000002"),
        1ULL << 20,
        7ULL << 20};
}

void require_error(
    const Error& error,
    VolumeIdentityError expected) {
    REQUIRE_EQ(error.code, static_cast<std::uint32_t>(expected));
}

} // namespace

TEST_CASE(volume_identity_accepts_one_complete_exact_lineage_and_pins_serial) {
    const auto expected = expectation();
    const std::array observations{exact_observation()};
    const auto matched = match_volume_identity(expected, observations);
    REQUIRE(matched.ok());
    REQUIRE_EQ(matched.value().owner(), expected.owner);
    REQUIRE_EQ(matched.value().observation(), observations.front());
    REQUIRE(matched.value().pinned_expectation().volume_serial.has_value());
    REQUIRE_EQ(
        *matched.value().pinned_expectation().volume_serial,
        observations.front().volume_serial);
}

TEST_CASE(volume_identity_ignores_fully_foreign_candidates) {
    auto foreign = exact_observation();
    foreign.volume_guid = uuid("90000000-0000-4000-8000-000000000003");
    foreign.volume_guid_path =
        L"\\\\?\\Volume{90000000-0000-4000-8000-000000000003}\\";
    foreign.physical_disk.physical_drive_number = 99;
    foreign.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    foreign.physical_disk.port_number = 99;
    foreign.physical_disk.path_id = 9;
    foreign.physical_disk.target_id = 9;
    foreign.physical_disk.lun = 9;
    foreign.storage_device_number = 99;
    foreign.extents.front().disk_number = 99;
    foreign.gpt_disk_guid =
        uuid("90000000-0000-4000-8000-000000000001");
    foreign.partition_guid =
        uuid("90000000-0000-4000-8000-000000000002");

    const std::array observations{foreign, exact_observation()};
    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(matched.ok());
    REQUIRE_EQ(matched.value().observation().physical_disk, disk());
}

TEST_CASE(volume_identity_does_not_alias_same_scsi_tuple_on_another_port) {
    auto foreign = exact_observation();
    foreign.volume_guid = uuid("90000000-0000-4000-8000-000000000003");
    foreign.volume_guid_path =
        L"\\\\?\\Volume{90000000-0000-4000-8000-000000000003}\\";
    foreign.physical_disk.physical_drive_number = 99;
    foreign.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    foreign.physical_disk.port_number = 12;
    foreign.storage_device_number = 99;
    foreign.extents.front().disk_number = 99;
    foreign.gpt_disk_guid =
        uuid("90000000-0000-4000-8000-000000000001");
    foreign.partition_guid =
        uuid("90000000-0000-4000-8000-000000000002");
    const std::array observations{exact_observation(), foreign};

    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(matched.ok());
}

TEST_CASE(volume_identity_rejects_partial_authoritative_matches) {
    const auto expected = expectation();
    auto wrong_disk = exact_observation();
    wrong_disk.physical_disk.physical_drive_number = 43;
    wrong_disk.physical_disk.path = L"\\\\.\\PhysicalDrive43";
    wrong_disk.storage_device_number = 43;
    wrong_disk.extents.front().disk_number = 43;
    const std::array observations{wrong_disk};

    const auto matched = match_volume_identity(expected, observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::identity_conflict);
}

TEST_CASE(volume_identity_rejects_duplicate_exact_candidates) {
    const std::array observations{exact_observation(), exact_observation()};
    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::ambiguous);
}

TEST_CASE(volume_identity_rejects_physical_drive_number_reuse) {
    auto reused = exact_observation();
    reused.physical_disk.port_number = 12;
    reused.physical_disk.path_id = 4;
    reused.physical_disk.target_id = 5;
    reused.physical_disk.lun = 6;
    reused.physical_disk.length_bytes += 4096;
    reused.gpt_disk_guid =
        uuid("30000000-0000-4000-8000-000000000001");
    const std::array observations{reused};

    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::identity_conflict);
}

TEST_CASE(volume_identity_rejects_same_scsi_address_on_another_port) {
    auto reused = exact_observation();
    reused.physical_disk.port_number = 12;
    const std::array observations{reused};

    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::identity_conflict);
}

TEST_CASE(volume_identity_rejects_foreign_volume_sharing_gpt_lineage) {
    auto cloned = exact_observation();
    cloned.volume_guid = uuid("90000000-0000-4000-8000-000000000003");
    cloned.volume_guid_path =
        L"\\\\?\\Volume{90000000-0000-4000-8000-000000000003}\\";
    cloned.physical_disk.physical_drive_number = 99;
    cloned.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    cloned.physical_disk.port_number = 99;
    cloned.physical_disk.path_id = 9;
    cloned.physical_disk.target_id = 9;
    cloned.physical_disk.lun = 9;
    cloned.storage_device_number = 99;
    cloned.extents.front().disk_number = 99;
    const std::array observations{exact_observation(), cloned};

    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::ambiguous);
}

TEST_CASE(volume_identity_rejects_multi_extent_and_partition_mismatch) {
    auto spanned = exact_observation();
    spanned.extents.push_back({43, 0, 1ULL << 20});
    const std::array spanned_set{spanned};
    auto result = match_volume_identity(expectation(), spanned_set);
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeIdentityError::identity_conflict);

    auto wrong_partition = exact_observation();
    wrong_partition.storage_partition_number = 2;
    const std::array wrong_partition_set{wrong_partition};
    result = match_volume_identity(expectation(), wrong_partition_set);
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeIdentityError::identity_conflict);
}

TEST_CASE(volume_identity_rejects_wrong_layout_filesystem_and_volume_path) {
    auto wrong_style = exact_observation();
    wrong_style.partition_style = ObservedPartitionStyle::mbr;
    const std::array wrong_style_set{wrong_style};
    auto result = match_volume_identity(expectation(), wrong_style_set);
    REQUIRE(!result.ok());

    auto wrong_filesystem = exact_observation();
    wrong_filesystem.filesystem_name = L"ReFS";
    const std::array wrong_filesystem_set{wrong_filesystem};
    result = match_volume_identity(expectation(), wrong_filesystem_set);
    REQUIRE(!result.ok());

    auto wrong_path = exact_observation();
    wrong_path.volume_guid_path =
        L"\\\\?\\Volume{90000000-0000-4000-8000-000000000003}\\";
    const std::array wrong_path_set{wrong_path};
    result = match_volume_identity(expectation(), wrong_path_set);
    REQUIRE(!result.ok());
}

TEST_CASE(volume_identity_revalidation_rejects_serial_and_lineage_change) {
    const std::array observations{exact_observation()};
    const auto initial = match_volume_identity(expectation(), observations);
    REQUIRE(initial.ok());

    auto changed_serial = exact_observation();
    ++changed_serial.volume_serial;
    auto reverified = reverify_volume_identity(initial.value(), changed_serial);
    REQUIRE(!reverified.ok());
    require_error(
        reverified.error(), VolumeIdentityError::opened_identity_changed);

    auto changed_extent = exact_observation();
    changed_extent.extents.front().starting_offset += 4096;
    reverified = reverify_volume_identity(initial.value(), changed_extent);
    REQUIRE(!reverified.ok());
    require_error(
        reverified.error(), VolumeIdentityError::opened_identity_changed);
}

TEST_CASE(volume_identity_rejects_invalid_or_incomplete_expectations) {
    auto invalid = expectation();
    invalid.owner.profile_uuid =
        uuid("00000000-0000-0000-0000-000000000000");
    const std::array observations{exact_observation()};
    auto matched = match_volume_identity(invalid, observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::invalid_expectation);

    invalid = expectation();
    invalid.physical_disk.path = L"R:";
    matched = match_volume_identity(invalid, observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::invalid_expectation);
}

TEST_CASE(volume_identity_reports_not_found_without_authoritative_anchor) {
    auto foreign = exact_observation();
    foreign.volume_guid = uuid("90000000-0000-4000-8000-000000000003");
    foreign.volume_guid_path =
        L"\\\\?\\Volume{90000000-0000-4000-8000-000000000003}\\";
    foreign.physical_disk.physical_drive_number = 99;
    foreign.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    foreign.physical_disk.port_number = 99;
    foreign.physical_disk.path_id = 9;
    foreign.physical_disk.target_id = 9;
    foreign.physical_disk.lun = 9;
    foreign.storage_device_number = 99;
    foreign.extents.front().disk_number = 99;
    foreign.gpt_disk_guid =
        uuid("90000000-0000-4000-8000-000000000001");
    foreign.partition_guid =
        uuid("90000000-0000-4000-8000-000000000002");
    const std::array observations{foreign};

    const auto matched = match_volume_identity(expectation(), observations);
    REQUIRE(!matched.ok());
    require_error(matched.error(), VolumeIdentityError::not_found);
}
