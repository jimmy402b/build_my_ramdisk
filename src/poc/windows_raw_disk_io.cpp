#include "windows_raw_disk_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace myrd::poc {

Result<void> WindowsRawDiskIo::verify_identity(
    const aim::LocatedDisk& disk) {
    const auto& path = disk.path;

    // Open read-only for identity check
    HANDLE h = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateFileW(ro_check)", L"failed to open for identity check"});
    }

    GET_LENGTH_INFORMATION len{};
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &len, sizeof(len), &br, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        return Result<void>::failure(
            {ErrorDomain::win32, err,
             L"IOCTL_DISK_GET_LENGTH_INFO", L"failed"});
    }
    CloseHandle(h);

    if (static_cast<std::uint64_t>(len.Length.QuadPart) != disk.length_bytes) {
        return Result<void>::failure(
            {ErrorDomain::evidence, 0,
             L"verify_identity", L"disk length mismatch"});
    }

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
    DWORD br2 = 0;
    if (!DeviceIoControl(rw_handle_.get(), IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &len, sizeof(len), &br2, nullptr)) {
        return Result<void>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"IOCTL_DISK_GET_LENGTH_INFO(rw_check)", L"failed"});
    }
    if (static_cast<std::uint64_t>(len.Length.QuadPart) != disk.length_bytes) {
        return Result<void>::failure(
            {ErrorDomain::evidence, 0,
             L"verify_identity(rw_check)", L"disk length mismatch on rw handle"});
    }

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
