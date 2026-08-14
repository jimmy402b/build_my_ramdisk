#include "test_framework.h"
#include "src/run/windows_volume.h"
#include "src/driver/aim_protocol.h"
#include "src/run/windows_volume.cpp"

#include <type_traits>

TEST_CASE(m4_volume_contract_is_concrete) {
    static_assert(std::is_final_v<myrd::ProductVolume>);
    myrd::ProductVolume volume{};
    REQUIRE(volume.volume_guid_path.empty());
    REQUIRE_EQ(volume.physical_drive_number, 0u);
    REQUIRE_EQ(volume.drive_letter, L'\0');
}

TEST_CASE(m4_locked_volume_contract_is_move_only_and_busy_errors_are_narrow) {
    static_assert(std::is_final_v<myrd::LockedVolume>);
    static_assert(!std::is_copy_constructible_v<myrd::LockedVolume>);
    static_assert(std::is_nothrow_move_constructible_v<myrd::LockedVolume>);
    REQUIRE(myrd::volume_lock_busy_error(ERROR_SHARING_VIOLATION));
    REQUIRE(myrd::volume_lock_busy_error(ERROR_LOCK_VIOLATION));
    REQUIRE(!myrd::volume_lock_busy_error(ERROR_ACCESS_DENIED));
}

TEST_CASE(m4_volume_rejects_runtime_sentinels_before_native_open) {
    myrd::m3::M3AimRuntimeInfo info{};
    info.device_id.value = myrd::aim::kAllDevices;
    info.disk.physical_drive_number = 7;
    info.disk.path = L"\\\\.\\PhysicalDrive7";
    info.disk.length_bytes = 64ull * 1024 * 1024;
    info.virtual_bytes = info.disk.length_bytes;
    info.sector_size = 512;
    auto result = myrd::provision_new_volume(info, L'R', std::chrono::seconds(1));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().operation, std::wstring(L"validate_exact_disk"));
}

TEST_CASE(m4_volume_rejects_zero_runtime_geometry_before_native_open) {
    myrd::m3::M3AimRuntimeInfo info{};
    info.device_id.value = 1;
    info.disk.physical_drive_number = 7;
    info.disk.path = L"\\\\.\\PhysicalDrive7";
    auto result = myrd::find_recovered_volume(info, L'R', std::chrono::seconds(1));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().operation, std::wstring(L"validate_exact_disk"));
}

TEST_CASE(m4_runtime_identity_predicate_rejects_all_forged_facts) {
    myrd::m3::M3AimRuntimeInfo info{};
    info.device_id.value = 0x00010203;
    info.disk.path_id = 3; info.disk.target_id = 2; info.disk.lun = 1;
    info.disk.physical_drive_number = 7; info.disk.path = L"\\\\.\\PhysicalDrive7";
    info.virtual_bytes = info.disk.length_bytes = 64ull * 1024 * 1024; info.sector_size = 512;
    REQUIRE(myrd::runtime_identity_ok(info));
    info.device_id.value = myrd::aim::kAllDevices; REQUIRE(!myrd::runtime_identity_ok(info));
    info.device_id.value = 0x00010203; info.disk.path = L"\\\\.\\PhysicalDrive8"; REQUIRE(!myrd::runtime_identity_ok(info));
    info.disk.path = L"\\\\.\\PhysicalDrive7"; info.disk.target_id = 9; REQUIRE(!myrd::runtime_identity_ok(info));
    info.device_id.value=0x00010000; info.disk.path_id=0; info.disk.target_id=0; info.disk.lun=1; REQUIRE(myrd::runtime_identity_ok(info));
}

TEST_CASE(m4_drive_mask_predicate_fails_closed_only_on_real_error) {
    REQUIRE(myrd::drive_mask_ok(1,ERROR_SUCCESS)); REQUIRE(myrd::drive_mask_ok(0,ERROR_SUCCESS));
    REQUIRE(!myrd::drive_mask_ok(0,ERROR_ACCESS_DENIED));
}

TEST_CASE(m4_volume_device_and_attribute_predicates_reject_malformed_inputs) {
    std::wstring device;
    REQUIRE(myrd::volume_device_path(L"\\\\?\\Volume{abc}\\",device)); REQUIRE_EQ(device,std::wstring(L"\\\\?\\Volume{abc}"));
    REQUIRE(!myrd::volume_device_path(L"\\\\?\\Volume{abc}",device)); REQUIRE(!myrd::volume_device_path(L"R:\\",device));
    REQUIRE(myrd::attribute_list_size_ok(FALSE,ERROR_INSUFFICIENT_BUFFER,1)); REQUIRE(!myrd::attribute_list_size_ok(TRUE,ERROR_SUCCESS,1)); REQUIRE(!myrd::attribute_list_size_ok(FALSE,ERROR_SUCCESS,1));
}

TEST_CASE(m4_native_predicates_freeze_geometry_layout_and_timeout) {
    myrd::m3::M3AimRuntimeInfo info{}; info.virtual_bytes = 4096; info.sector_size = 512;
    REQUIRE(myrd::native_geometry_ok(4096,512,4096,info)); REQUIRE(myrd::native_geometry_ok(4096,512,512,info));
    DRIVE_LAYOUT_INFORMATION_EX layout{}; layout.PartitionStyle=PARTITION_STYLE_RAW; layout.Mbr.Signature=1; layout.Mbr.CheckSum=1; REQUIRE(myrd::layout_ok(layout,true)); layout.PartitionCount=1; REQUIRE(!myrd::layout_ok(layout,true));
    layout={}; layout.PartitionStyle=PARTITION_STYLE_MBR; layout.PartitionCount=0; layout.Mbr.Signature=0; layout.Mbr.CheckSum=0; REQUIRE(myrd::layout_ok(layout,true));
    layout.Mbr.Signature=1; REQUIRE(!myrd::layout_ok(layout,true)); layout.Mbr.Signature=0; layout.Mbr.CheckSum=1; REQUIRE(!myrd::layout_ok(layout,true)); layout.Mbr.CheckSum=0; layout.PartitionCount=1; REQUIRE(!myrd::layout_ok(layout,true));
    layout={}; layout.PartitionStyle=PARTITION_STYLE_GPT; REQUIRE(!myrd::layout_ok(layout,true)); layout.PartitionStyle=static_cast<PARTITION_STYLE>(99); REQUIRE(!myrd::layout_ok(layout,true));
    layout={}; layout.PartitionStyle=PARTITION_STYLE_GPT; layout.PartitionCount=1; layout.PartitionEntry[0].Gpt.PartitionType=myrd::kBasicDataPartition; layout.PartitionEntry[0].PartitionLength.QuadPart=1; REQUIRE(myrd::layout_ok(layout,false)); layout.PartitionEntry[0].PartitionLength.QuadPart=0; REQUIRE(!myrd::layout_ok(layout,false));
    REQUIRE(myrd::valid_timeout(std::chrono::milliseconds(1))); REQUIRE(!myrd::valid_timeout(std::chrono::milliseconds(0))); REQUIRE(!myrd::valid_timeout(std::chrono::seconds(121)));
}

TEST_CASE(m4_native_geometry_accepts_windows_512_or_4096_physical_sectors_only) {
    myrd::m3::M3AimRuntimeInfo info{}; info.virtual_bytes = 4096; info.sector_size = 512;
    REQUIRE(myrd::native_geometry_ok(4096, 512, 512, info));
    REQUIRE(myrd::native_geometry_ok(4096, 512, 4096, info));
    REQUIRE(!myrd::native_geometry_ok(4096, 512, 0, info));
    REQUIRE(!myrd::native_geometry_ok(4096, 512, 1024, info));
    REQUIRE(!myrd::native_geometry_ok(4096, 512, 8192, info));
    REQUIRE(!myrd::native_geometry_ok(4095, 512, 512, info));
    REQUIRE(!myrd::native_geometry_ok(4096, 4096, 512, info));
}

TEST_CASE(m4_extent_and_mount_predicates_reject_ambiguous_tables) {
    alignas(VOLUME_DISK_EXTENTS) std::array<std::byte, sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT)> bytes{};
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(bytes.data());
    REQUIRE_EQ(myrd::classify_extents(bytes.data(),0,7),myrd::ExtentClass::Invalid);
    extents->NumberOfDiskExtents=1; extents->Extents[0].DiskNumber=7;
    REQUIRE_EQ(myrd::classify_extents(bytes.data(),offsetof(VOLUME_DISK_EXTENTS,Extents)+sizeof(DISK_EXTENT),7),myrd::ExtentClass::Exact);
    extents->Extents[0].DiskNumber=8; REQUIRE_EQ(myrd::classify_extents(bytes.data(),offsetof(VOLUME_DISK_EXTENTS,Extents)+sizeof(DISK_EXTENT),7),myrd::ExtentClass::Other);
    extents->NumberOfDiskExtents=2; REQUIRE_EQ(myrd::classify_extents(bytes.data(),static_cast<DWORD>(bytes.size()),7),myrd::ExtentClass::Mixed);
    const wchar_t absent[]={L'X',L':',L'\\',L'\0',L'\0'}; const wchar_t exact[]={L'R',L':',L'\\',L'\0',L'\0'}; const wchar_t malformed[]={L'R',L':',L'\\'};
    REQUIRE_EQ(myrd::classify_mount(L'R',absent,5,L"x"),myrd::MountClass::Absent);
    REQUIRE_EQ(myrd::classify_mount(L'R',exact,5,L"x"),myrd::MountClass::Exact);
    REQUIRE_EQ(myrd::classify_mount(L'R',malformed,3,L"x"),myrd::MountClass::Malformed);
}

TEST_CASE(m4_enumerated_extent_query_skips_only_unsupported_unrelated_volumes) {
    REQUIRE_EQ(myrd::classify_enumerated_extent_query(TRUE, ERROR_SUCCESS),
               myrd::EnumeratedExtentQueryClass::Inspect);
    REQUIRE_EQ(myrd::classify_enumerated_extent_query(FALSE, ERROR_INVALID_FUNCTION),
               myrd::EnumeratedExtentQueryClass::SkipUnsupported);
    REQUIRE_EQ(myrd::classify_enumerated_extent_query(FALSE, ERROR_MORE_DATA),
               myrd::EnumeratedExtentQueryClass::TooLarge);
    REQUIRE_EQ(myrd::classify_enumerated_extent_query(FALSE, ERROR_ACCESS_DENIED),
               myrd::EnumeratedExtentQueryClass::Failed);
    REQUIRE_EQ(myrd::classify_enumerated_extent_query(FALSE, ERROR_GEN_FAILURE),
               myrd::EnumeratedExtentQueryClass::Failed);
}

TEST_CASE(m4_letter_target_selection_bypasses_enumeration_or_fails_closed) {
    std::wstring device;
    REQUIRE_EQ(myrd::classify_letter_target(TRUE, ERROR_SUCCESS, L"\\\\?\\Volume{target}\\", device), myrd::LetterTargetClass::Target);
    REQUIRE_EQ(device, std::wstring(L"\\\\?\\Volume{target}"));
    REQUIRE_EQ(myrd::classify_letter_target(FALSE, ERROR_FILE_NOT_FOUND, L"", device), myrd::LetterTargetClass::Absent);
    REQUIRE_EQ(myrd::classify_letter_target(FALSE, ERROR_PATH_NOT_FOUND, L"", device), myrd::LetterTargetClass::Absent);
    REQUIRE_EQ(myrd::classify_letter_target(FALSE, ERROR_ACCESS_DENIED, L"", device), myrd::LetterTargetClass::Failed);
    REQUIRE_EQ(myrd::classify_letter_target(TRUE, ERROR_SUCCESS, L"R:\\", device), myrd::LetterTargetClass::Failed);
    REQUIRE(!myrd::retry_volume_discovery(L"find_recovered_volume_target"));
    REQUIRE(myrd::retry_volume_discovery(L"find_recovered_volume"));
    myrd::Error original{myrd::ErrorDomain::win32, 5, L"verify_extent", L"volume extent query failed", 123};
    const auto targeted = myrd::target_failure(original);
    REQUIRE_EQ(targeted.operation, std::wstring(L"find_recovered_volume_target"));
    REQUIRE_EQ(targeted.message, original.message);
    REQUIRE_EQ(targeted.code, original.code);
    REQUIRE_EQ(targeted.native_code, original.native_code);
}

TEST_CASE(m4_mount_ownership_predicate_is_case_insensitive_and_exact) {
    REQUIRE(myrd::owned_mount(L"\\\\?\\Volume{abc}\\", L"\\\\?\\VOLUME{ABC}\\"));
    REQUIRE(!myrd::owned_mount(L"\\\\?\\Volume{other}\\", L"\\\\?\\Volume{abc}\\"));
    REQUIRE(!myrd::owned_mount(nullptr, L"x"));
}

TEST_CASE(m4_fixed_ioctl_size_predicate_rejects_truncation_and_layout_overflow) {
    constexpr DWORD header=offsetof(DRIVE_LAYOUT_INFORMATION_EX,PartitionEntry);
    REQUIRE(myrd::fixed_ioctl_sizes_ok(sizeof(STORAGE_DEVICE_NUMBER),sizeof(DISK_GEOMETRY_EX),sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),header+sizeof(PARTITION_INFORMATION_EX),1));
    REQUIRE(!myrd::fixed_ioctl_sizes_ok(0,sizeof(DISK_GEOMETRY_EX),sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),header,0));
    REQUIRE(!myrd::fixed_ioctl_sizes_ok(sizeof(STORAGE_DEVICE_NUMBER),sizeof(DISK_GEOMETRY_EX)-1,sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),header,0));
    REQUIRE(!myrd::fixed_ioctl_sizes_ok(sizeof(STORAGE_DEVICE_NUMBER),sizeof(DISK_GEOMETRY_EX),sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),header,1));
    REQUIRE(!myrd::fixed_ioctl_sizes_ok(sizeof(STORAGE_DEVICE_NUMBER),sizeof(DISK_GEOMETRY_EX),sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),header+sizeof(PARTITION_INFORMATION_EX),2));
}
