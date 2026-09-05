#include "src/host/volume_identity.h"
#include "src/common/unique_handle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <memory>
#include <utility>

namespace myrd::host {
namespace {

template <typename T>
Result<T> identity_failure(
    VolumeIdentityError code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::evidence,
        static_cast<std::uint32_t>(code),
        operation,
        message});
}

bool is_nil(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{}; });
}

std::wstring widen_ascii(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const char byte : value) {
        result.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(byte)));
    }
    return result;
}

std::wstring physical_drive_path(std::uint32_t number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

std::wstring volume_guid_path(const product::Uuid& volume_guid) {
    return L"\\\\?\\Volume{" + widen_ascii(volume_guid.to_string())
        + L"}\\";
}

bool equals_case_insensitive(
    std::wstring_view left,
    std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::towlower(left[i]) != std::towlower(right[i])) return false;
    }
    return true;
}

bool valid_expectation(const VolumeIdentityExpectation& expected) noexcept {
    if (is_nil(expected.owner.profile_uuid)
        || is_nil(expected.owner.image_uuid)
        || expected.owner.profile_revision == 0
        || expected.owner.session_generation == 0
        || is_nil(expected.gpt_disk_guid)
        || is_nil(expected.partition_guid)
        || is_nil(expected.volume_guid)
        || expected.physical_disk.length_bytes == 0
        || expected.physical_disk.path != physical_drive_path(
            expected.physical_disk.physical_drive_number)) {
        return false;
    }
    const auto address = aim::decode_device_address(
        expected.owner.aim_device_id);
    return address.path_id == expected.physical_disk.path_id
        && address.target_id == expected.physical_disk.target_id
        && address.lun == expected.physical_disk.lun;
}

bool has_authoritative_anchor(
    const VolumeIdentityExpectation& expected,
    const VolumeIdentityObservation& observed) noexcept {
    return observed.physical_disk.physical_drive_number
            == expected.physical_disk.physical_drive_number
        || observed.storage_device_number
            == expected.physical_disk.physical_drive_number
        || observed.physical_disk.port_number
                == expected.physical_disk.port_number
            && observed.physical_disk.path_id == expected.physical_disk.path_id
            && observed.physical_disk.target_id
                == expected.physical_disk.target_id
            && observed.physical_disk.lun == expected.physical_disk.lun
        || observed.gpt_disk_guid == expected.gpt_disk_guid
        || observed.partition_guid == expected.partition_guid
        || observed.volume_guid == expected.volume_guid;
}

bool extent_is_valid(
    const VolumeIdentityObservation& observed) noexcept {
    if (observed.extents.size() != 1) return false;
    const auto& extent = observed.extents.front();
    return extent.disk_number == observed.physical_disk.physical_drive_number
        && extent.starting_offset == observed.partition_starting_offset
        && extent.length_bytes == observed.partition_length_bytes;
}

bool partition_fits_disk(
    const VolumeIdentityObservation& observed) noexcept {
    if (observed.partition_length_bytes == 0
        || observed.partition_starting_offset
            > observed.physical_disk.length_bytes) {
        return false;
    }
    return observed.partition_length_bytes
        <= observed.physical_disk.length_bytes
            - observed.partition_starting_offset;
}

bool is_exact_match(
    const VolumeIdentityExpectation& expected,
    const VolumeIdentityObservation& observed) noexcept {
    if (!(observed.physical_disk == expected.physical_disk)
        || observed.partition_style != ObservedPartitionStyle::gpt
        || observed.gpt_disk_guid != expected.gpt_disk_guid
        || observed.partition_guid != expected.partition_guid
        || observed.volume_guid != expected.volume_guid
        || observed.volume_guid_path != volume_guid_path(expected.volume_guid)
        || !equals_case_insensitive(observed.filesystem_name, L"NTFS")
        || observed.storage_device_type != kStorageDeviceTypeDisk
        || observed.storage_device_number
            != expected.physical_disk.physical_drive_number
        || observed.partition_number == 0
        || observed.storage_partition_number != observed.partition_number
        || !extent_is_valid(observed)
        || !partition_fits_disk(observed)) {
        return false;
    }
    return !expected.volume_serial.has_value()
        || *expected.volume_serial == observed.volume_serial;
}

template <typename T>
Result<T> windows_failure(
    const wchar_t* operation,
    const wchar_t* message,
    std::uint32_t native_code = GetLastError()) {
    return Result<T>::failure({
        ErrorDomain::win32,
        static_cast<std::uint32_t>(VolumeIdentityError::enumeration_failed),
        operation,
        message,
        native_code});
}

product::Uuid nil_uuid() {
    return product::Uuid::parse(
        "00000000-0000-0000-0000-000000000000").move_value();
}

Result<product::Uuid> uuid_from_guid(
    const GUID& guid,
    const wchar_t* operation) {
    std::array<wchar_t, 40> text{};
    if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size()))
        == 0) {
        return windows_failure<product::Uuid>(
            operation, L"StringFromGUID2 failed", ERROR_INVALID_DATA);
    }
    std::string canonical;
    canonical.reserve(36);
    for (std::size_t i = 1; i <= 36; ++i) {
        const wchar_t value = text[i];
        if (value > 0x7F) {
            return windows_failure<product::Uuid>(
                operation,
                L"Windows returned a non-ASCII GUID",
                ERROR_INVALID_DATA);
        }
        char byte = static_cast<char>(value);
        if (byte >= 'A' && byte <= 'F') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
        canonical.push_back(byte);
    }
    auto parsed = product::Uuid::parse(canonical);
    if (!parsed.ok()) {
        return windows_failure<product::Uuid>(
            operation,
            L"Windows returned a malformed GUID",
            ERROR_INVALID_DATA);
    }
    return parsed;
}

Result<product::Uuid> uuid_from_volume_path(std::wstring_view path) {
    constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
    if (path.size() != prefix.size() + 36 + 2
        || !equals_case_insensitive(path.substr(0, prefix.size()), prefix)
        || path[prefix.size() + 36] != L'}'
        || path.back() != L'\\') {
        return windows_failure<product::Uuid>(
            L"uuid_from_volume_path",
            L"FindFirstVolumeW returned a malformed volume GUID path",
            ERROR_INVALID_DATA);
    }
    std::string canonical;
    canonical.reserve(36);
    for (const wchar_t value : path.substr(prefix.size(), 36)) {
        if (value > 0x7F) {
            return windows_failure<product::Uuid>(
                L"uuid_from_volume_path",
                L"Volume GUID contains non-ASCII data",
                ERROR_INVALID_DATA);
        }
        char byte = static_cast<char>(value);
        if (byte >= 'A' && byte <= 'F') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
        canonical.push_back(byte);
    }
    auto parsed = product::Uuid::parse(canonical);
    if (!parsed.ok()) {
        return windows_failure<product::Uuid>(
            L"uuid_from_volume_path",
            L"Volume GUID text is malformed",
            ERROR_INVALID_DATA);
    }
    return parsed;
}

Result<std::vector<std::byte>> query_dynamic_ioctl(
    HANDLE handle,
    DWORD control_code,
    std::size_t initial_size,
    const wchar_t* operation) {
    constexpr std::size_t kMaximumResponse = 1024 * 1024;
    std::size_t size = initial_size;
    while (size <= kMaximumResponse) {
        std::vector<std::byte> buffer(size);
        DWORD returned = 0;
        if (DeviceIoControl(
                handle,
                control_code,
                nullptr,
                0,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &returned,
                nullptr)) {
            if (returned > buffer.size()) {
                return windows_failure<std::vector<std::byte>>(
                    operation,
                    L"DeviceIoControl returned an invalid byte count",
                    ERROR_INVALID_DATA);
            }
            buffer.resize(returned);
            return Result<std::vector<std::byte>>::success(
                std::move(buffer));
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER
            && error != ERROR_MORE_DATA) {
            return windows_failure<std::vector<std::byte>>(
                operation, L"DeviceIoControl failed", error);
        }
        if (size > kMaximumResponse / 2) break;
        size *= 2;
    }
    return windows_failure<std::vector<std::byte>>(
        operation,
        L"DeviceIoControl response exceeded the safety limit",
        ERROR_INSUFFICIENT_BUFFER);
}

template <typename T>
Result<T> query_fixed_ioctl(
    HANDLE handle,
    DWORD control_code,
    const wchar_t* operation) {
    T value{};
    DWORD returned = 0;
    if (!DeviceIoControl(
            handle,
            control_code,
            nullptr,
            0,
            &value,
            sizeof(value),
            &returned,
            nullptr)) {
        return windows_failure<T>(
            operation, L"DeviceIoControl failed");
    }
    if (returned < sizeof(value)) {
        return windows_failure<T>(
            operation,
            L"DeviceIoControl returned a truncated identity",
            ERROR_INVALID_DATA);
    }
    return Result<T>::success(value);
}

struct GptPartitionSnapshot final {
    std::uint32_t number{};
    product::Uuid guid;
    std::uint64_t starting_offset{};
    std::uint64_t length_bytes{};
};

struct DiskSnapshot final {
    PhysicalDiskIdentity identity;
    ObservedPartitionStyle partition_style{ObservedPartitionStyle::unknown};
    product::Uuid disk_guid;
    std::vector<GptPartitionSnapshot> partitions;
};

Result<DiskSnapshot> inspect_physical_disk_handle(
    HANDLE disk_handle,
    std::uint32_t expected_number) {
    if (disk_handle == nullptr || disk_handle == INVALID_HANDLE_VALUE) {
        return windows_failure<DiskSnapshot>(
            L"inspect_physical_disk_handle",
            L"The PhysicalDrive handle is invalid",
            ERROR_INVALID_HANDLE);
    }
    auto storage = query_fixed_ioctl<STORAGE_DEVICE_NUMBER>(
        disk_handle,
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        L"IOCTL_STORAGE_GET_DEVICE_NUMBER(PhysicalDrive)");
    if (!storage.ok()) {
        return Result<DiskSnapshot>::failure(storage.error());
    }
    if (storage.value().DeviceType != kStorageDeviceTypeDisk
        || storage.value().DeviceNumber != expected_number) {
        return identity_failure<DiskSnapshot>(
            VolumeIdentityError::opened_identity_changed,
            L"inspect_physical_disk_handle",
            L"The opened PhysicalDrive number or device type changed");
    }

    auto scsi = query_fixed_ioctl<SCSI_ADDRESS>(
        disk_handle,
        IOCTL_SCSI_GET_ADDRESS,
        L"IOCTL_SCSI_GET_ADDRESS(PhysicalDrive)");
    if (!scsi.ok()) {
        return Result<DiskSnapshot>::failure(scsi.error());
    }
    if (scsi.value().Length != sizeof(SCSI_ADDRESS)) {
        return windows_failure<DiskSnapshot>(
            L"IOCTL_SCSI_GET_ADDRESS(PhysicalDrive)",
            L"Windows returned an invalid SCSI_ADDRESS length",
            ERROR_INVALID_DATA);
    }
    auto length = query_fixed_ioctl<GET_LENGTH_INFORMATION>(
        disk_handle,
        IOCTL_DISK_GET_LENGTH_INFO,
        L"IOCTL_DISK_GET_LENGTH_INFO(PhysicalDrive)");
    if (!length.ok()) {
        return Result<DiskSnapshot>::failure(length.error());
    }
    if (length.value().Length.QuadPart <= 0) {
        return identity_failure<DiskSnapshot>(
            VolumeIdentityError::opened_identity_changed,
            L"inspect_physical_disk_handle",
            L"The opened PhysicalDrive has an invalid length");
    }
    const PhysicalDiskIdentity observed{
        storage.value().DeviceNumber,
        physical_drive_path(storage.value().DeviceNumber),
        static_cast<std::uint64_t>(length.value().Length.QuadPart),
        scsi.value().PortNumber,
        scsi.value().PathId,
        scsi.value().TargetId,
        scsi.value().Lun};

    auto layout_bytes = query_dynamic_ioctl(
        disk_handle,
        IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        64 * 1024,
        L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX");
    if (!layout_bytes.ok()) {
        return Result<DiskSnapshot>::failure(layout_bytes.error());
    }
    constexpr std::size_t kEntriesOffset =
        offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    if (layout_bytes.value().size() < kEntriesOffset) {
        return windows_failure<DiskSnapshot>(
            L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX",
            L"Drive layout response is truncated",
            ERROR_INVALID_DATA);
    }
    const auto* layout =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            layout_bytes.value().data());
    if (layout->PartitionCount >
        (layout_bytes.value().size() - kEntriesOffset)
            / sizeof(PARTITION_INFORMATION_EX)) {
        return windows_failure<DiskSnapshot>(
            L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX",
            L"Drive layout partition count exceeds the response",
            ERROR_INVALID_DATA);
    }
    ObservedPartitionStyle partition_style = ObservedPartitionStyle::unknown;
    if (layout->PartitionStyle == PARTITION_STYLE_RAW) {
        partition_style = ObservedPartitionStyle::raw;
    } else if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
        partition_style = ObservedPartitionStyle::mbr;
    } else if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
        partition_style = ObservedPartitionStyle::gpt;
    }
    if (partition_style != ObservedPartitionStyle::gpt) {
        return Result<DiskSnapshot>::success({
            observed, partition_style, nil_uuid(), {}});
    }

    auto disk_guid = uuid_from_guid(
        layout->Gpt.DiskId, L"inspect_physical_disk_handle.disk_guid");
    if (!disk_guid.ok()) {
        return Result<DiskSnapshot>::failure(disk_guid.error());
    }
    std::vector<GptPartitionSnapshot> partitions;
    partitions.reserve(layout->PartitionCount);
    for (DWORD index = 0; index < layout->PartitionCount; ++index) {
        const auto& partition = layout->PartitionEntry[index];
        if (partition.PartitionStyle != PARTITION_STYLE_GPT
            || partition.PartitionNumber == 0) {
            continue;
        }
        if (partition.StartingOffset.QuadPart < 0
            || partition.PartitionLength.QuadPart <= 0) {
            return windows_failure<DiskSnapshot>(
                L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX",
                L"GPT partition has an invalid extent",
                ERROR_INVALID_DATA);
        }
        auto partition_guid = uuid_from_guid(
            partition.Gpt.PartitionId,
            L"inspect_physical_disk_handle.partition_guid");
        if (!partition_guid.ok()) {
            return Result<DiskSnapshot>::failure(partition_guid.error());
        }
        const auto duplicate = std::find_if(
            partitions.begin(), partitions.end(),
            [&](const GptPartitionSnapshot& existing) {
                return existing.number == partition.PartitionNumber
                    || existing.guid == partition_guid.value();
            });
        if (duplicate != partitions.end()) {
            return identity_failure<DiskSnapshot>(
                VolumeIdentityError::ambiguous,
                L"inspect_physical_disk_handle",
                L"The GPT layout repeats a partition number or GUID");
        }
        partitions.push_back({
            partition.PartitionNumber,
            partition_guid.move_value(),
            static_cast<std::uint64_t>(partition.StartingOffset.QuadPart),
            static_cast<std::uint64_t>(partition.PartitionLength.QuadPart)});
    }
    return Result<DiskSnapshot>::success({
        observed,
        ObservedPartitionStyle::gpt,
        disk_guid.move_value(),
        std::move(partitions)});
}

Result<DiskSnapshot> inspect_physical_disk(std::uint32_t number) {
    const auto path = physical_drive_path(number);
    UniqueHandle disk_handle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!disk_handle) {
        return windows_failure<DiskSnapshot>(
            L"CreateFileW(PhysicalDrive)",
            L"Failed to open a volume's exact PhysicalDrive for identity");
    }
    return inspect_physical_disk_handle(disk_handle.get(), number);
}

Result<DiskSnapshot> inspect_expected_physical_disk(
    const PhysicalDiskIdentity& expected) {
    auto observed = inspect_physical_disk(expected.physical_drive_number);
    if (!observed.ok()) return observed;
    if (!(observed.value().identity == expected)) {
        return identity_failure<DiskSnapshot>(
            VolumeIdentityError::opened_identity_changed,
            L"inspect_expected_physical_disk",
            L"The opened PhysicalDrive port, SCSI address, path, or length changed");
    }
    return observed;
}

Result<std::vector<DiskExtentIdentity>> query_volume_extents(HANDLE volume) {
    auto bytes = query_dynamic_ioctl(
        volume,
        IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        4096,
        L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS");
    if (!bytes.ok()) {
        return Result<std::vector<DiskExtentIdentity>>::failure(bytes.error());
    }
    constexpr std::size_t kExtentsOffset =
        offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (bytes.value().size() < kExtentsOffset) {
        return windows_failure<std::vector<DiskExtentIdentity>>(
            L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS",
            L"Volume extent response is truncated",
            ERROR_INVALID_DATA);
    }
    const auto* response = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(
        bytes.value().data());
    if (response->NumberOfDiskExtents >
        (bytes.value().size() - kExtentsOffset) / sizeof(DISK_EXTENT)) {
        return windows_failure<std::vector<DiskExtentIdentity>>(
            L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS",
            L"Volume extent count exceeds the response",
            ERROR_INVALID_DATA);
    }
    std::vector<DiskExtentIdentity> extents;
    extents.reserve(response->NumberOfDiskExtents);
    for (DWORD index = 0; index < response->NumberOfDiskExtents; ++index) {
        const auto& extent = response->Extents[index];
        if (extent.StartingOffset.QuadPart < 0
            || extent.ExtentLength.QuadPart <= 0) {
            return windows_failure<std::vector<DiskExtentIdentity>>(
                L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS",
                L"Volume extent is invalid",
                ERROR_INVALID_DATA);
        }
        extents.push_back({
            extent.DiskNumber,
            static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
            static_cast<std::uint64_t>(extent.ExtentLength.QuadPart)});
    }
    return Result<std::vector<DiskExtentIdentity>>::success(
        std::move(extents));
}

Result<VolumeIdentityObservation> inspect_volume_handle(
    HANDLE volume,
    const product::Uuid& volume_guid,
    const DiskSnapshot& disk) {
    if (volume == nullptr || volume == INVALID_HANDLE_VALUE) {
        return windows_failure<VolumeIdentityObservation>(
            L"inspect_volume_handle",
            L"The volume handle is invalid",
            ERROR_INVALID_HANDLE);
    }
    auto storage = query_fixed_ioctl<STORAGE_DEVICE_NUMBER>(
        volume,
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        L"IOCTL_STORAGE_GET_DEVICE_NUMBER(volume)");
    if (!storage.ok()) {
        return Result<VolumeIdentityObservation>::failure(storage.error());
    }
    if (storage.value().DeviceType != kStorageDeviceTypeDisk
        || storage.value().DeviceNumber
            != disk.identity.physical_drive_number) {
        return identity_failure<VolumeIdentityObservation>(
            VolumeIdentityError::opened_identity_changed,
            L"inspect_volume_handle",
            L"The opened volume no longer maps to the retained PhysicalDrive");
    }
    auto extents = query_volume_extents(volume);
    if (!extents.ok()) {
        return Result<VolumeIdentityObservation>::failure(extents.error());
    }

    std::array<wchar_t, MAX_PATH + 1> filesystem{};
    DWORD serial = 0;
    if (!GetVolumeInformationByHandleW(
            volume,
            nullptr,
            0,
            &serial,
            nullptr,
            nullptr,
            filesystem.data(),
            static_cast<DWORD>(filesystem.size()))) {
        return windows_failure<VolumeIdentityObservation>(
            L"GetVolumeInformationByHandleW",
            L"Failed to read filesystem identity from the exact volume");
    }

    const GptPartitionSnapshot* matched_partition = nullptr;
    for (const auto& partition : disk.partitions) {
        if (partition.number != storage.value().PartitionNumber) continue;
        if (matched_partition != nullptr) {
            return identity_failure<VolumeIdentityObservation>(
                VolumeIdentityError::ambiguous,
                L"inspect_volume_handle",
                L"The disk layout repeats the volume partition number");
        }
        matched_partition = &partition;
    }
    const auto partition_guid = matched_partition != nullptr
        ? matched_partition->guid : nil_uuid();
    const auto partition_number = matched_partition != nullptr
        ? matched_partition->number : 0;
    const auto partition_offset = matched_partition != nullptr
        ? matched_partition->starting_offset : 0;
    const auto partition_length = matched_partition != nullptr
        ? matched_partition->length_bytes : 0;
    return Result<VolumeIdentityObservation>::success({
        volume_guid_path(volume_guid),
        volume_guid,
        serial,
        filesystem.data(),
        storage.value().DeviceType,
        storage.value().DeviceNumber,
        storage.value().PartitionNumber,
        extents.move_value(),
        disk.identity,
        disk.partition_style,
        disk.disk_guid,
        partition_number,
        partition_guid,
        partition_offset,
        partition_length});
}

Result<std::optional<VolumeIdentityObservation>> inspect_volume_candidate(
    std::wstring_view enumerated_path,
    const product::Uuid& volume_guid,
    const VolumeIdentityExpectation& expected) {
    std::wstring open_path(enumerated_path);
    if (open_path.empty() || open_path.back() != L'\\') {
        return windows_failure<std::optional<VolumeIdentityObservation>>(
            L"inspect_volume_candidate",
            L"Volume GUID path is malformed",
            ERROR_INVALID_DATA);
    }
    open_path.pop_back();
    UniqueHandle volume(CreateFileW(
        open_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!volume) {
        if (volume_guid != expected.volume_guid) {
            return Result<std::optional<VolumeIdentityObservation>>::success(
                std::nullopt);
        }
        return windows_failure<std::optional<VolumeIdentityObservation>>(
            L"CreateFileW(volume GUID)",
            L"Failed to open the exact expected volume GUID for identity");
    }
    auto storage = query_fixed_ioctl<STORAGE_DEVICE_NUMBER>(
        volume.get(),
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        L"IOCTL_STORAGE_GET_DEVICE_NUMBER(enumerated volume)");
    if (!storage.ok()) {
        if (volume_guid != expected.volume_guid) {
            return Result<std::optional<VolumeIdentityObservation>>::success(
                std::nullopt);
        }
        return Result<std::optional<VolumeIdentityObservation>>::failure(
            storage.error());
    }
    if (storage.value().DeviceType != kStorageDeviceTypeDisk) {
        if (volume_guid != expected.volume_guid) {
            return Result<std::optional<VolumeIdentityObservation>>::success(
                std::nullopt);
        }
        return identity_failure<std::optional<VolumeIdentityObservation>>(
            VolumeIdentityError::identity_conflict,
            L"inspect_volume_candidate",
            L"The expected volume GUID is not backed by a disk device");
    }
    const bool known_volume_or_number_anchor =
        volume_guid == expected.volume_guid
        || storage.value().DeviceNumber
            == expected.physical_disk.physical_drive_number;
    auto disk = inspect_physical_disk(storage.value().DeviceNumber);
    if (!disk.ok()) {
        if (!known_volume_or_number_anchor) {
            return Result<std::optional<VolumeIdentityObservation>>::success(
                std::nullopt);
        }
        return Result<std::optional<VolumeIdentityObservation>>::failure(
            disk.error());
    }
    const auto& disk_value = disk.value();
    const bool scsi_anchor =
        disk_value.identity.port_number
                == expected.physical_disk.port_number
        && disk_value.identity.path_id == expected.physical_disk.path_id
        && disk_value.identity.target_id == expected.physical_disk.target_id
        && disk_value.identity.lun == expected.physical_disk.lun;
    const bool partition_anchor = std::any_of(
        disk_value.partitions.begin(), disk_value.partitions.end(),
        [&](const GptPartitionSnapshot& partition) {
            return partition.guid == expected.partition_guid;
        });
    const bool known_anchor = known_volume_or_number_anchor || scsi_anchor
        || disk_value.disk_guid == expected.gpt_disk_guid
        || partition_anchor;
    if (!known_anchor) {
        return Result<std::optional<VolumeIdentityObservation>>::success(
            std::nullopt);
    }
    auto observed = inspect_volume_handle(
        volume.get(), volume_guid, disk_value);
    if (!observed.ok()) {
        return Result<std::optional<VolumeIdentityObservation>>::failure(
            observed.error());
    }
    return Result<std::optional<VolumeIdentityObservation>>::success(
        observed.move_value());
}

struct FindVolumeCloser final {
    void operator()(void* value) const noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            FindVolumeClose(static_cast<HANDLE>(value));
        }
    }
};

using FindVolumeHandle = std::unique_ptr<void, FindVolumeCloser>;

} // namespace

VerifiedVolumeIdentity::VerifiedVolumeIdentity(
    VolumeIdentityExpectation pinned,
    VolumeIdentityObservation observation) noexcept
    : pinned_(std::move(pinned)), observation_(std::move(observation)) {}

const HostOwnershipMarker& VerifiedVolumeIdentity::owner() const noexcept {
    return pinned_.owner;
}

const VolumeIdentityObservation& VerifiedVolumeIdentity::observation()
    const noexcept {
    return observation_;
}

const VolumeIdentityExpectation&
VerifiedVolumeIdentity::pinned_expectation() const noexcept {
    return pinned_;
}

Result<VerifiedVolumeIdentity> match_volume_identity(
    const VolumeIdentityExpectation& expected,
    std::span<const VolumeIdentityObservation> observations) {
    if (!valid_expectation(expected)) {
        return identity_failure<VerifiedVolumeIdentity>(
            VolumeIdentityError::invalid_expectation,
            L"match_volume_identity",
            L"The expected host, AIM, disk, partition, or volume identity is incomplete");
    }

    const VolumeIdentityObservation* exact = nullptr;
    bool conflict = false;
    for (const auto& observed : observations) {
        if (is_exact_match(expected, observed)) {
            if (exact != nullptr) {
                return identity_failure<VerifiedVolumeIdentity>(
                    VolumeIdentityError::ambiguous,
                    L"match_volume_identity",
                    L"Multiple complete observations match the expected volume");
            }
            exact = &observed;
        } else if (has_authoritative_anchor(expected, observed)) {
            conflict = true;
        }
    }

    if (conflict) {
        return identity_failure<VerifiedVolumeIdentity>(
            exact == nullptr ? VolumeIdentityError::identity_conflict
                             : VolumeIdentityError::ambiguous,
            L"match_volume_identity",
            L"An authoritative disk, partition, or volume identifier conflicts with the expected lineage");
    }
    if (exact == nullptr) {
        return identity_failure<VerifiedVolumeIdentity>(
            VolumeIdentityError::not_found,
            L"match_volume_identity",
            L"No complete exact volume lineage was found");
    }

    auto pinned = expected;
    pinned.volume_serial = exact->volume_serial;
    return Result<VerifiedVolumeIdentity>::success(
        VerifiedVolumeIdentity{std::move(pinned), *exact});
}

Result<VerifiedVolumeIdentity> reverify_volume_identity(
    const VerifiedVolumeIdentity& verified,
    const VolumeIdentityObservation& current) {
    const std::array observations{current};
    auto result = match_volume_identity(
        verified.pinned_expectation(), observations);
    if (!result.ok()
        && result.error().code == static_cast<std::uint32_t>(
            VolumeIdentityError::identity_conflict)) {
        auto error = result.error();
        error.code = static_cast<std::uint32_t>(
            VolumeIdentityError::opened_identity_changed);
        error.operation = L"reverify_volume_identity";
        error.message = L"The opened volume lineage changed after verification";
        return Result<VerifiedVolumeIdentity>::failure(std::move(error));
    }
    return result;
}

Result<std::vector<VolumeIdentityObservation>>
enumerate_windows_volume_identities(
    const VolumeIdentityExpectation& expected) {
    if (!valid_expectation(expected)) {
        return identity_failure<std::vector<VolumeIdentityObservation>>(
            VolumeIdentityError::invalid_expectation,
            L"enumerate_windows_volume_identities",
            L"The expected exact volume identity is incomplete");
    }
    auto disk = inspect_expected_physical_disk(expected.physical_disk);
    if (!disk.ok()) {
        return Result<std::vector<VolumeIdentityObservation>>::failure(
            disk.error());
    }

    std::array<wchar_t, 1024> path{};
    const HANDLE raw_find = FindFirstVolumeW(
        path.data(), static_cast<DWORD>(path.size()));
    if (raw_find == INVALID_HANDLE_VALUE) {
        return windows_failure<std::vector<VolumeIdentityObservation>>(
            L"FindFirstVolumeW",
            L"Failed to begin volume GUID enumeration");
    }
    FindVolumeHandle find(raw_find);
    std::vector<VolumeIdentityObservation> observations;
    for (;;) {
        auto volume_guid = uuid_from_volume_path(path.data());
        if (!volume_guid.ok()) {
            return Result<std::vector<VolumeIdentityObservation>>::failure(
                volume_guid.error());
        }
        auto observed = inspect_volume_candidate(
            path.data(), volume_guid.value(), expected);
        if (!observed.ok()) {
            return Result<std::vector<VolumeIdentityObservation>>::failure(
                observed.error());
        }
        if (observed.value().has_value()) {
            observations.push_back(std::move(*observed.value()));
        }

        path.fill(L'\0');
        if (FindNextVolumeW(
                find.get(), path.data(), static_cast<DWORD>(path.size()))) {
            continue;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) break;
        return windows_failure<std::vector<VolumeIdentityObservation>>(
            L"FindNextVolumeW",
            L"Volume GUID enumeration failed",
            error);
    }
    return Result<std::vector<VolumeIdentityObservation>>::success(
        std::move(observations));
}

Result<VerifiedVolumeIdentity> verify_windows_volume_identity(
    const VolumeIdentityExpectation& expected) {
    auto observations = enumerate_windows_volume_identities(expected);
    if (!observations.ok()) {
        return Result<VerifiedVolumeIdentity>::failure(observations.error());
    }
    return match_volume_identity(expected, observations.value());
}

Result<VolumeIdentityObservation> observe_open_windows_volume_identity(
    std::uintptr_t native_volume_handle,
    std::uintptr_t native_disk_handle,
    const VolumeIdentityExpectation& expected) {
    if (!valid_expectation(expected) || native_volume_handle == 0
        || native_disk_handle == 0) {
        return identity_failure<VolumeIdentityObservation>(
            VolumeIdentityError::invalid_expectation,
            L"observe_open_windows_volume_identity",
            L"Valid expected identity plus retained volume and disk handles are required");
    }
    auto disk = inspect_physical_disk_handle(
        reinterpret_cast<HANDLE>(native_disk_handle),
        expected.physical_disk.physical_drive_number);
    if (!disk.ok()) {
        return Result<VolumeIdentityObservation>::failure(disk.error());
    }
    if (!(disk.value().identity == expected.physical_disk)) {
        return identity_failure<VolumeIdentityObservation>(
            VolumeIdentityError::opened_identity_changed,
            L"observe_open_windows_volume_identity",
            L"The retained PhysicalDrive port, SCSI address, path, or length changed");
    }
    return inspect_volume_handle(
        reinterpret_cast<HANDLE>(native_volume_handle),
        expected.volume_guid,
        disk.value());
}

} // namespace myrd::host
