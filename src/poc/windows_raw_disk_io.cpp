#include "windows_raw_disk_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

namespace myrd::poc {

namespace {

Result<void> verify_handle_identity(
    HANDLE handle, const aim::LocatedDisk& disk,
    std::wstring_view operation_suffix) {
    GET_LENGTH_INFORMATION length{};
    STORAGE_DEVICE_NUMBER device_number{};
    SCSI_ADDRESS scsi_address{};
    DWORD bytes = 0;

    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &length, sizeof(length), &bytes, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             std::wstring(L"IOCTL_DISK_GET_LENGTH_INFO") +
                 std::wstring(operation_suffix),
             L"failed"});
    }
    if (!DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &device_number, sizeof(device_number),
            &bytes, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             std::wstring(L"IOCTL_STORAGE_GET_DEVICE_NUMBER") +
                 std::wstring(operation_suffix),
             L"failed"});
    }
    if (!DeviceIoControl(handle, IOCTL_SCSI_GET_ADDRESS,
            nullptr, 0, &scsi_address, sizeof(scsi_address),
            &bytes, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             std::wstring(L"IOCTL_SCSI_GET_ADDRESS") +
                 std::wstring(operation_suffix),
             L"failed"});
    }

    if (static_cast<std::uint64_t>(length.Length.QuadPart) !=
            disk.length_bytes ||
        device_number.DeviceNumber != disk.physical_drive_number ||
        scsi_address.PathId != disk.path_id ||
        scsi_address.TargetId != disk.target_id ||
        scsi_address.Lun != disk.lun) {
        return Result<void>::failure(
            {ErrorDomain::evidence, 0,
             std::wstring(L"verify_identity") +
                 std::wstring(operation_suffix),
             L"disk number, SCSI address, or length mismatch"});
    }
    return Result<void>::success();
}

} // anonymous namespace

std::uint32_t raw_identity_open_access() noexcept {
    return GENERIC_READ;
}

std::optional<DiskAttributeUpdate> writable_disk_attribute_update(
    std::uint64_t disk_attributes) noexcept {
    if ((disk_attributes & DISK_ATTRIBUTE_READ_ONLY) == 0) {
        return std::nullopt;
    }
    return DiskAttributeUpdate{
        0,
        DISK_ATTRIBUTE_READ_ONLY,
        false};
}

Result<void> validate_disk_write_preflight(
    std::uint64_t disk_attributes,
    bool disk_stack_reports_writable,
    std::uint32_t disk_stack_error) {
    if ((disk_attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        return Result<void>::failure(
            {ErrorDomain::win32, ERROR_NOT_READY,
             L"IOCTL_DISK_GET_DISK_ATTRIBUTES",
             L"disk has the offline attribute"});
    }
    if ((disk_attributes & DISK_ATTRIBUTE_READ_ONLY) != 0) {
        return Result<void>::failure(
            {ErrorDomain::win32, ERROR_WRITE_PROTECT,
             L"IOCTL_DISK_GET_DISK_ATTRIBUTES",
             L"disk has the read-only attribute"});
    }
    if (!disk_stack_reports_writable) {
        return Result<void>::failure(
            {ErrorDomain::win32,
             disk_stack_error == ERROR_SUCCESS
                 ? static_cast<std::uint32_t>(ERROR_WRITE_PROTECT)
                 : disk_stack_error,
             L"IOCTL_DISK_IS_WRITABLE",
             L"disk stack rejected writable-media preflight"});
    }
    return Result<void>::success();
}

Result<void> WindowsRawDiskIo::verify_identity(
    const aim::LocatedDisk& disk) {
    const auto& path = disk.path;
    const auto expected_path = std::wstring(L"\\\\.\\PhysicalDrive") +
        std::to_wstring(disk.physical_drive_number);
    if (path != expected_path) {
        return Result<void>::failure(
            {ErrorDomain::evidence, 0,
             L"verify_identity(path)",
             L"PhysicalDrive path does not match the located disk number"});
    }

    // Open read-only for identity check
    HANDLE h = CreateFileW(path.c_str(), raw_identity_open_access(),
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateFileW(ro_check)", L"failed to open for identity check"});
    }

    auto read_identity = verify_handle_identity(h, disk, L"(ro_check)");
    if (!read_identity) {
        CloseHandle(h);
        return read_identity;
    }
    CloseHandle(h);

    // Open read-write handle
    rw_handle_ = UniqueHandle(
        CreateFileW(path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!rw_handle_) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateFileW(rw)", L"failed to open for write"});
    }

    // Double-check identity on write handle
    auto write_identity = verify_handle_identity(
        rw_handle_.get(), disk, L"(rw_check)");
    if (!write_identity) return write_identity;

    GET_DISK_ATTRIBUTES attributes{};
    DWORD attributes_bytes = 0;
    if (!DeviceIoControl(rw_handle_.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES,
            nullptr, 0, &attributes, sizeof(attributes),
            &attributes_bytes, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"IOCTL_DISK_GET_DISK_ATTRIBUTES", L"failed"});
    }

    if ((attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        return validate_disk_write_preflight(
            attributes.Attributes, true, ERROR_SUCCESS);
    }

    if (const auto update =
            writable_disk_attribute_update(attributes.Attributes)) {
        SET_DISK_ATTRIBUTES set_attributes{};
        set_attributes.Version = sizeof(set_attributes);
        set_attributes.Persist = update->persist ? TRUE : FALSE;
        set_attributes.Attributes = update->attributes;
        set_attributes.AttributesMask = update->attributes_mask;

        DWORD set_bytes = 0;
        if (!DeviceIoControl(rw_handle_.get(),
                IOCTL_DISK_SET_DISK_ATTRIBUTES,
                &set_attributes, sizeof(set_attributes),
                nullptr, 0, &set_bytes, nullptr)) {
            return Result<void>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(GetLastError()),
                 L"IOCTL_DISK_SET_DISK_ATTRIBUTES",
                 L"failed to clear the non-persistent read-only attribute"});
        }

        attributes = {};
        attributes_bytes = 0;
        if (!DeviceIoControl(rw_handle_.get(),
                IOCTL_DISK_GET_DISK_ATTRIBUTES,
                nullptr, 0, &attributes, sizeof(attributes),
                &attributes_bytes, nullptr)) {
            return Result<void>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(GetLastError()),
                 L"IOCTL_DISK_GET_DISK_ATTRIBUTES(after_set)",
                 L"failed to verify writable disk attributes"});
        }
    }

    SetLastError(ERROR_SUCCESS);
    DWORD writable_bytes = 0;
    const BOOL writable = DeviceIoControl(
        rw_handle_.get(), IOCTL_DISK_IS_WRITABLE,
        nullptr, 0, nullptr, 0, &writable_bytes, nullptr);
    const auto writable_error = static_cast<std::uint32_t>(GetLastError());
    auto preflight = validate_disk_write_preflight(
        attributes.Attributes, writable != FALSE, writable_error);
    if (!preflight) return preflight;

    return Result<void>::success();
}

Result<void> WindowsRawDiskIo::seek(std::uint64_t offset) {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(rw_handle_.get(), li, nullptr, FILE_BEGIN)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"SetFilePointerEx", L"seek failed"});
    }
    return Result<void>::success();
}

Result<void> WindowsRawDiskIo::write_exact(std::span<const std::byte> data) {
    DWORD written = 0;
    if (!WriteFile(rw_handle_.get(), data.data(),
            static_cast<DWORD>(data.size()), &written, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"WriteFile", L"write failed"});
    }
    if (written != data.size()) {
        return Result<void>::failure(
            {ErrorDomain::storage, 0, L"WriteFile", L"short write"});
    }
    return Result<void>::success();
}

Result<void> WindowsRawDiskIo::flush() {
    if (!FlushFileBuffers(rw_handle_.get())) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"FlushFileBuffers", L"flush failed"});
    }
    return Result<void>::success();
}

Result<void> WindowsRawDiskIo::read_exact(std::span<std::byte> data) {
    DWORD read = 0;
    if (!ReadFile(rw_handle_.get(), data.data(),
            static_cast<DWORD>(data.size()), &read, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"ReadFile", L"read failed"});
    }
    if (read != data.size()) {
        return Result<void>::failure(
            {ErrorDomain::storage, 0, L"ReadFile", L"short read"});
    }
    return Result<void>::success();
}

} // namespace myrd::poc
