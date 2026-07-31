#include "physical_drive_locator.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <chrono>
#include <memory>
#include <thread>

#pragma comment(lib, "setupapi.lib")

namespace myrd::aim {

// ── Pure matching ─────────────────────────────────────────────

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates) {

    auto addr = decode_device_address(created);
    constexpr std::uint64_t kExpectedSize = 64ull * 1024ull * 1024ull;

    const DiskCandidate* match = nullptr;
    for (const auto& c : candidates) {
        if (c.path_id != addr.path_id) continue;
        if (c.target_id != addr.target_id) continue;
        if (c.lun != addr.lun) continue;
        if (c.existed_before) continue;
        if (c.length_bytes != kExpectedSize) continue;
        if (match != nullptr) {
            return Result<LocatedDisk>::failure(
                {ErrorDomain::evidence, 0,
                 L"match_created_disk",
                 L"multiple candidates match"});
        }
        match = &c;
    }

    if (!match) {
        return Result<LocatedDisk>::failure(
            {ErrorDomain::evidence, 0,
             L"match_created_disk",
             L"no candidate matches"});
    }

    LocatedDisk result{};
    result.physical_drive_number = match->physical_drive_number;
    result.length_bytes = match->length_bytes;
    result.path = L"\\\\.\\PhysicalDrive" +
                  std::to_wstring(match->physical_drive_number);
    return Result<LocatedDisk>::success(result);
}

// ── Enumeration helpers ───────────────────────────────────────

namespace {

struct DevInfoDeleter {
    void operator()(HDEVINFO h) const {
        if (h && h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
    }
};
using DevInfoGuard = std::unique_ptr<
    std::remove_pointer_t<HDEVINFO>, DevInfoDeleter>;

Result<std::vector<DiskCandidate>> enumerate_disk_candidates() {
    static const GUID kDiskGuid = GUID_DEVINTERFACE_DISK;
    std::vector<DiskCandidate> candidates;

    DevInfoGuard di(SetupDiGetClassDevsW(&kDiskGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!di) return Result<std::vector<DiskCandidate>>::success(candidates);

    SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(di.get(), nullptr, &kDiskGuid,
                                      idx, &iface);
         ++idx) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(di.get(), &iface, nullptr, 0,
                                          &req, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) continue;

        std::vector<std::byte> dbuf(req);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            dbuf.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(di.get(), &iface, detail,
                req, nullptr, nullptr)) continue;

        HANDLE h = CreateFileW(detail->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        DiskCandidate c{};

        // SCSI address
        SCSI_ADDRESS scsi_addr{};
        DWORD br = 0;
        if (DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, nullptr, 0,
                &scsi_addr, sizeof(scsi_addr), &br, nullptr)) {
            c.path_id = scsi_addr.PathId;
            c.target_id = scsi_addr.TargetId;
            c.lun = scsi_addr.Lun;
        }

        // Device number
        STORAGE_DEVICE_NUMBER dev_num{};
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                &dev_num, sizeof(dev_num), &br, nullptr)) {
            c.physical_drive_number = dev_num.DeviceNumber;
        }

        // Disk length
        GET_LENGTH_INFORMATION len{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                &len, sizeof(len), &br, nullptr)) {
            c.length_bytes = len.Length.QuadPart;
        }

        CloseHandle(h);
        candidates.push_back(c);
    }
    return Result<std::vector<DiskCandidate>>::success(candidates);
}

} // anonymous namespace

// ── Snapshot ──────────────────────────────────────────────────

Result<std::vector<std::uint32_t>> snapshot_physical_drive_numbers() {
    auto cand = enumerate_disk_candidates();
    if (!cand) return Result<std::vector<std::uint32_t>>::failure(cand.error());
    std::vector<std::uint32_t> nums;
    for (const auto& c : cand.value()) {
        nums.push_back(c.physical_drive_number);
    }
    return Result<std::vector<std::uint32_t>>::success(nums);
}

// ── Wait loop ─────────────────────────────────────────────────

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::chrono::milliseconds timeout) {

    auto deadline = std::chrono::steady_clock::now() + timeout;
    constexpr auto kInterval = std::chrono::milliseconds(250);

    while (std::chrono::steady_clock::now() < deadline) {
        auto cand = enumerate_disk_candidates();
        if (!cand) {
            std::this_thread::sleep_for(kInterval);
            continue;
        }

        // Mark existed_before
        for (auto& c : cand.value()) {
            for (auto b : baseline) {
                if (c.physical_drive_number == b) {
                    c.existed_before = true;
                    break;
                }
            }
        }

        auto result = match_created_disk(created, cand.value());
        if (result.ok()) return result;

        std::this_thread::sleep_for(kInterval);
    }

    return Result<LocatedDisk>::failure(
        {ErrorDomain::evidence, 0,
         L"wait_for_created_physical_drive", L"timed out"});
}

} // namespace myrd::aim
