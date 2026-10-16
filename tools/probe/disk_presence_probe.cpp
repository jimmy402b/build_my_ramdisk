// A one-question probe: does DIGCF_PRESENT return the phantom AIM disks?
//
// This exists to falsify or confirm one link in the T1-03 chain.  The guest accumulates "Arsenal
// Virtual SCSI Disk" entries in the SCSI Enum key that survive reboots, report ConfigFlags 0, and show
// as Status Unknown -- phantoms.  `enumerate_disk_candidates` starts from
// `SetupDiGetClassDevsW(..., DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)`, so if that call returns the
// phantoms, enumeration opens each one, issues SCSI and storage IOCTLs against a device with no hardware
// behind it, and slows down as the pile grows.  If it returns only the real disk, that explanation is
// wrong and the stall has to be found elsewhere.
//
// It reports counts only.  It creates nothing, changes nothing, and touches no device state -- every
// call is a read-only query, so it is safe to run anywhere.

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

#include <cstdio>

#pragma comment(lib, "setupapi.lib")

int main() {
    // The same GUID and flags the locator uses.
    static const GUID kDiskGuid = GUID_DEVINTERFACE_DISK;

    // Present only: this is the call whose result we are questioning.
    const HDEVINFO present = SetupDiGetClassDevsW(
        &kDiskGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    int present_count = 0;
    if (present != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
        for (DWORD i = 0;; ++i) {
            if (!SetupDiEnumDeviceInterfaces(present, nullptr, &kDiskGuid, i, &iface)) break;
            ++present_count;
        }
        SetupDiDestroyDeviceInfoList(present);
    } else {
        std::printf("DIGCF_PRESENT call failed, error %lu\n", GetLastError());
    }

    // And without DIGCF_PRESENT, for context: how many exist at all, present or not.
    const HDEVINFO all = SetupDiGetClassDevsW(
        &kDiskGuid, nullptr, nullptr, DIGCF_DEVICEINTERFACE);
    int all_count = 0;
    if (all != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
        for (DWORD i = 0;; ++i) {
            if (!SetupDiEnumDeviceInterfaces(all, nullptr, &kDiskGuid, i, &iface)) break;
            ++all_count;
        }
        SetupDiDestroyDeviceInfoList(all);
    }

    std::printf("disk interfaces with DIGCF_PRESENT : %d\n", present_count);
    std::printf("disk interfaces without it          : %d\n", all_count);
    std::printf("verdict: %s\n",
                present_count > 2
                    ? "the phantoms ARE returned -- the enumeration explanation holds"
                    : "the phantoms are NOT returned -- the explanation is wrong");
    return 0;
}
