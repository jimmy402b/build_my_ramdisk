// Second probe: which part of the enumeration is slow -- the enumeration, the open, or the IOCTLs?
//
// The first probe settled one question and raised this one.  `SetupDiGetClassDevsW` with
// `DIGCF_PRESENT` returns only the real disk on this guest, not the phantom AIM entries, so the pile of
// stale devices is not what enumeration walks.  `enumerate_disk_candidates` does more than enumerate,
// though: for each candidate it opens the device and issues two IOCTLs.  This times each step
// separately, which decides where the stall lives.
//
// The open's flags are copied from `disk_candidate_open_access()`'s call site in
// physical_drive_locator.cpp -- sharing read and write, no overlapped flag.  Nothing here creates or
// changes anything; every call is a query.

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <chrono>
#include <cstdio>

#pragma comment(lib, "setupapi.lib")

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main() {
    static const GUID kDiskGuid = GUID_DEVINTERFACE_DISK;

    auto t = Clock::now();
    const HDEVINFO info = SetupDiGetClassDevsW(
        &kDiskGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    std::printf("SetupDiGetClassDevsW        : %8.1f ms\n", ms_since(t));
    if (info == INVALID_HANDLE_VALUE) {
        std::printf("enumeration failed, error %lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
    t = Clock::now();
    const BOOL got = SetupDiEnumDeviceInterfaces(info, nullptr, &kDiskGuid, 0, &iface);
    std::printf("SetupDiEnumDeviceInterfaces : %8.1f ms (got=%d)\n", ms_since(t), got);
    if (!got) {
        SetupDiDestroyDeviceInfoList(info);
        return 1;
    }

    DWORD req = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &req, nullptr);
    std::vector<BYTE> buffer(req ? req : 1024);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    t = Clock::now();
    const BOOL have_detail = SetupDiGetDeviceInterfaceDetailW(
        info, &iface, detail, req, nullptr, nullptr);
    std::printf("SetupDiGetDeviceInterfaceDetailW: %6.1f ms\n", ms_since(t));
    if (!have_detail) {
        SetupDiDestroyDeviceInfoList(info);
        return 1;
    }
    std::wprintf(L"path: %ls\n", detail->DevicePath);

    // The open, with the shared access and lack of overlapped that the locator uses.
    t = Clock::now();
    HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const double open_ms = ms_since(t);
    std::printf("CreateFileW (as locator does): %8.1f ms (handle=%s)\n", open_ms,
                h == INVALID_HANDLE_VALUE ? "INVALID" : "ok");

    if (h != INVALID_HANDLE_VALUE) {
        SCSI_ADDRESS address{};
        DWORD bytes = 0;
        t = Clock::now();
        const BOOL addr_ok = DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, nullptr, 0,
                                             &address, sizeof(address), &bytes, nullptr);
        std::printf("IOCTL_SCSI_GET_ADDRESS      : %8.1f ms (ok=%d)\n", ms_since(t), addr_ok);

        STORAGE_DEVICE_NUMBER number{};
        t = Clock::now();
        const BOOL num_ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                            &number, sizeof(number), &bytes, nullptr);
        std::printf("IOCTL_STORAGE_GET_DEVICE_NUMBER: %6.1f ms (ok=%d, disk=%lu)\n",
                    ms_since(t), num_ok, num_ok ? number.DeviceNumber : 0UL);

        // A second open while this one is held, to see whether that is what blocks.
        t = Clock::now();
        HANDLE h2 = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::printf("second CreateFileW while held: %7.1f ms (handle=%s)\n", ms_since(t),
                    h2 == INVALID_HANDLE_VALUE ? "INVALID" : "ok");
        if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(info);
    return 0;
}
