#include "physical_drive_locator.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#pragma comment(lib, "setupapi.lib")

namespace myrd::aim {

namespace {

constexpr std::uint64_t kM0DiskSizeBytes = 64ull * 1024ull * 1024ull;

} // anonymous namespace

std::uint32_t disk_candidate_open_access() noexcept {
    return GENERIC_READ;
}

// ── Pure matching ─────────────────────────────────────────────

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates) {

    return match_created_disk(created, candidates, kM0DiskSizeBytes);
}

Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates,
    std::uint64_t expected_size_bytes) {

    if (expected_size_bytes == 0) {
        return Result<LocatedDisk>::failure(
            {ErrorDomain::evidence, ERROR_INVALID_PARAMETER,
             L"match_created_disk",
             L"expected disk size must be non-zero"});
    }

    auto addr = decode_device_address(created);

    const DiskCandidate* match = nullptr;
    bool matching_new_address_seen = false;
    std::uint32_t conflicting_number = 0;
    std::uint64_t conflicting_length = 0;
    for (const auto& c : candidates) {
        if (c.path_id != addr.path_id) continue;
        if (c.target_id != addr.target_id) continue;
        if (c.lun != addr.lun) continue;
        if (c.existed_before) continue;
        matching_new_address_seen = true;
        if (c.length_bytes != expected_size_bytes) {
            if (conflicting_number == 0) {
                conflicting_number = c.physical_drive_number;
                conflicting_length = c.length_bytes;
            }
            continue;
        }
        if (match != nullptr) {
            return Result<LocatedDisk>::failure(
                {ErrorDomain::evidence, ERROR_DUP_NAME,
                 L"match_created_disk",
                 L"multiple candidates match"});
        }
        match = &c;
    }

    if (!match) {
        std::wstring message = L"no candidate matches";
        if (matching_new_address_seen) {
            message = L"new matching SCSI address has conflicting length identity";
            message += L" (PhysicalDrive" +
                std::to_wstring(conflicting_number) + L" length=" +
                std::to_wstring(conflicting_length) + L" expected=" +
                std::to_wstring(expected_size_bytes) + L")";
        }
        return Result<LocatedDisk>::failure({
            ErrorDomain::evidence,
            static_cast<std::uint32_t>(
                matching_new_address_seen ? ERROR_INVALID_DATA : ERROR_NOT_FOUND),
            L"match_created_disk", std::move(message)});
    }

    LocatedDisk result{};
    result.physical_drive_number = match->physical_drive_number;
    result.length_bytes = match->length_bytes;
    result.path_id = match->path_id;
    result.target_id = match->target_id;
    result.lun = match->lun;
    result.path = L"\\\\.\\PhysicalDrive" +
                  std::to_wstring(match->physical_drive_number);
    return Result<LocatedDisk>::success(result);
}

bool same_located_disk_identity(
    const LocatedDisk& left, const LocatedDisk& right) noexcept {
    return left.physical_drive_number == right.physical_drive_number &&
        left.path == right.path &&
        left.length_bytes == right.length_bytes &&
        left.path_id == right.path_id &&
        left.target_id == right.target_id &&
        left.lun == right.lun;
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

    const HDEVINFO raw_device_info = SetupDiGetClassDevsW(
        &kDiskGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (raw_device_info == INVALID_HANDLE_VALUE) {
        return Result<std::vector<DiskCandidate>>::failure(
            {ErrorDomain::win32,
             static_cast<std::uint32_t>(GetLastError()),
             L"SetupDiGetClassDevsW(disk)",
             L"failed to enumerate disk interfaces"});
    }
    DevInfoGuard di(raw_device_info);

    SP_DEVICE_INTERFACE_DATA iface{sizeof(SP_DEVICE_INTERFACE_DATA)};
    for (DWORD idx = 0;; ++idx) {
        if (!SetupDiEnumDeviceInterfaces(
                di.get(), nullptr, &kDiskGuid, idx, &iface)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) break;
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(error),
                 L"SetupDiEnumDeviceInterfaces(disk)",
                 L"disk interface enumeration failed"});
        }

        DWORD req = 0;
        SetLastError(ERROR_SUCCESS);
        if (SetupDiGetDeviceInterfaceDetailW(
                di.get(), &iface, nullptr, 0, &req, nullptr) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            req < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(GetLastError()),
                 L"SetupDiGetDeviceInterfaceDetailW(size)",
                 L"failed to size disk interface detail"});
        }

        std::vector<std::byte> dbuf(req);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            dbuf.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(di.get(), &iface, detail,
                req, nullptr, nullptr)) {
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(GetLastError()),
                 L"SetupDiGetDeviceInterfaceDetailW(disk)",
                 L"failed to read disk interface detail"});
        }

        HANDLE h = CreateFileW(detail->DevicePath,
            disk_candidate_open_access(),
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(GetLastError()),
                 L"CreateFileW(disk interface)",
                 L"failed to open a present disk interface for identity"});
        }

        DiskCandidate c{};

        // SCSI address
        SCSI_ADDRESS scsi_addr{};
        DWORD br = 0;
        if (!DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, nullptr, 0,
                &scsi_addr, sizeof(scsi_addr), &br, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(h);
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(error),
                 L"IOCTL_SCSI_GET_ADDRESS(disk interface)",
                 L"failed to identify a present disk interface"});
        }
        c.path_id = scsi_addr.PathId;
        c.target_id = scsi_addr.TargetId;
        c.lun = scsi_addr.Lun;

        // Device number
        STORAGE_DEVICE_NUMBER dev_num{};
        if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                &dev_num, sizeof(dev_num), &br, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(h);
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(error),
                 L"IOCTL_STORAGE_GET_DEVICE_NUMBER(disk interface)",
                 L"failed to number a present disk interface"});
        }
        c.physical_drive_number = dev_num.DeviceNumber;

        // Disk length
        GET_LENGTH_INFORMATION len{};
        if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                &len, sizeof(len), &br, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(h);
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(error),
                 L"IOCTL_DISK_GET_LENGTH_INFO(disk interface)",
                 L"failed to size a present disk interface"});
        }
        c.length_bytes = len.Length.QuadPart;

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

    return wait_for_created_physical_drive(
        created, baseline, kM0DiskSizeBytes, timeout);
}

Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout) {

    if (expected_size_bytes == 0) {
        return Result<LocatedDisk>::failure(
            {ErrorDomain::evidence, ERROR_INVALID_PARAMETER,
             L"wait_for_created_physical_drive",
             L"expected disk size must be non-zero"});
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    constexpr auto kInterval = std::chrono::milliseconds(250);
    constexpr auto kConfirmationDelay = std::chrono::milliseconds(100);
    std::optional<Error> last_identity_conflict;

    auto mark_existed_before = [&](std::vector<DiskCandidate>& candidates) {
        for (auto& candidate : candidates) {
            for (const auto number : baseline) {
                if (candidate.physical_drive_number == number) {
                    candidate.existed_before = true;
                    break;
                }
            }
        }
    };

    while (std::chrono::steady_clock::now() < deadline) {
        auto cand = enumerate_disk_candidates();
        if (!cand) {
            return Result<LocatedDisk>::failure(cand.error());
        }

        mark_existed_before(cand.value());

        auto first = match_created_disk(
            created, cand.value(), expected_size_bytes);
        if (!first) {
            // Windows may publish the new disk interface before its length
            // information has settled. Keep polling for the exact identity;
            // a transient same-address/length mismatch is not proof that a
            // different disk was created. The timeout remains the fail-safe.
            if (first.error().code != ERROR_NOT_FOUND &&
                first.error().code != ERROR_INVALID_DATA) {
                return Result<LocatedDisk>::failure(first.error());
            }
            if (first.error().code == ERROR_INVALID_DATA) {
                last_identity_conflict = first.error();
            }
            std::this_thread::sleep_for(kInterval);
            continue;
        }

        std::this_thread::sleep_for(kConfirmationDelay);
        auto confirmed_candidates = enumerate_disk_candidates();
        if (!confirmed_candidates) {
            return Result<LocatedDisk>::failure(
                confirmed_candidates.error());
        }
        mark_existed_before(confirmed_candidates.value());
        auto confirmed = match_created_disk(
            created, confirmed_candidates.value(), expected_size_bytes);
        if (!confirmed ||
            !same_located_disk_identity(first.value(), confirmed.value())) {
            return Result<LocatedDisk>::failure(
                {ErrorDomain::evidence, ERROR_INVALID_DATA,
                 L"wait_for_created_physical_drive(confirm)",
                 L"PhysicalDrive identity changed during repeated confirmation"});
        }
        return confirmed;
    }

    if (last_identity_conflict.has_value()) {
        return Result<LocatedDisk>::failure(*last_identity_conflict);
    }
    return Result<LocatedDisk>::failure({
        ErrorDomain::evidence, WAIT_TIMEOUT,
        L"wait_for_created_physical_drive", L"timed out"});
}

Result<std::vector<LocatedDisk>> enumerate_present_physical_drives() {
    auto candidates = enumerate_disk_candidates();
    if (!candidates) {
        return Result<std::vector<LocatedDisk>>::failure(candidates.error());
    }
    std::vector<LocatedDisk> disks;
    try {
        disks.reserve(candidates.value().size());
        for (const auto& candidate : candidates.value()) {
            disks.push_back({
                candidate.physical_drive_number,
                std::wstring(L"\\\\.\\PhysicalDrive")
                    + std::to_wstring(candidate.physical_drive_number),
                candidate.length_bytes,
                candidate.path_id,
                candidate.target_id,
                candidate.lun});
        }
    } catch (...) {
        return Result<std::vector<LocatedDisk>>::failure({
            ErrorDomain::internal, ERROR_OUTOFMEMORY,
            L"enumerate_present_physical_drives",
            L"failed to allocate bounded physical-drive observations"});
    }
    return Result<std::vector<LocatedDisk>>::success(std::move(disks));
}

} // namespace myrd::aim
