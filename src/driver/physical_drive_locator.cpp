#include "physical_drive_locator.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "src/common/diagnostic_trace.h"

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

// A disk that retires between being enumerated and being opened is not an
// enumeration failure -- it is a disk that is no longer there, and the caller is
// entitled to a picture of what *is* there.  This matters as soon as any device
// on the machine can come and go while this runs, which is exactly what a
// second RAM disk is.
//
// The set is deliberately narrow.  `ERROR_ACCESS_DENIED` in particular stays a
// failure: skipping a disk this process is not allowed to open would turn a real
// refusal into a disk that appears not to exist, and a locator that cannot see a
// device must say so rather than look past it.
bool device_vanished(const DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_NO_SUCH_DEVICE;
}

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
            const DWORD error = GetLastError();
            if (device_vanished(error)) continue; // retired under this scan
            return Result<std::vector<DiskCandidate>>::failure(
                {ErrorDomain::win32,
                 static_cast<std::uint32_t>(error),
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
            if (device_vanished(error)) continue;
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
            if (device_vanished(error)) continue;
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
            if (device_vanished(error)) continue;
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

// How long one enumeration of the disk interfaces is allowed to take.
//
// A worst-case bound, not a performance target: a healthy enumeration is milliseconds, and this
// project's guest has been measured over eighty seconds without returning.
constexpr auto kEnumerateTimeout = std::chrono::seconds(10);

// Runs `enumerate_disk_candidates` with a bound, because it has none of its own.
//
// **This is the fix for an intermittent timeout, not a tidy-up.**  The wait loop below checks its
// deadline once, before the body, and then calls this enumeration -- a sequence of synchronous
// SetupAPI and IOCTL calls with no overlapped form and nothing to cancel.  So that deadline bounded
// how many times the loop turned, not how long a turn took, and one non-returning enumeration outlived
// it.  Measured: a worker hung here for over eighty seconds at 0.23 s of CPU, and the reply carrying
// its own timeout never arrived.
//
// **A `std::async` future cannot express this bound, and using one cost a measurement.**  Its
// destructor blocks until the task finishes, so `wait_for` timing out and then letting the future go
// out of scope waits exactly as long as the thing being bounded -- the bound reads as if it works and
// changes nothing.  The first version of this function did that, and a `-Repeat 5` reading showed the
// same 4107 timeouts as before.  A detached thread is what actually abandons the work.
//
// **Abandoning is acceptable here, and that is the reason for this route.**  The abandoned work is
// read-only: list interfaces, ask each for its SCSI address and its length.  It creates, mounts,
// dismantles and alters nothing, so a thread still inside the kernel cannot leave the machine holding
// a resource nobody owns.  That is materially different from abandoning a teardown, which is why the
// earlier fixes in this project cancelled where a handle allowed it and this one bounds an execution.
Result<std::vector<DiskCandidate>> enumerate_disk_candidates_within(
    const std::chrono::milliseconds timeout = kEnumerateTimeout) {
    // Heap state shared with the detached thread rather than locals it would outlive.  The thread may
    // still be inside the kernel when this function gives up, so everything it touches has to survive
    // this scope by construction instead of by an argument about which paths return.
    struct Shared final {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<Result<std::vector<DiskCandidate>>> outcome;
        bool complete{};
    };
    auto shared = std::make_shared<Shared>();

    // Detached on purpose: the point is that a call which never returns must not hold this one, and
    // joining would do exactly that.  The shared state is kept alive by the thread's own copy.
    std::thread([shared] {
        auto produced = enumerate_disk_candidates();
        {
            std::lock_guard<std::mutex> guard(shared->mutex);
            shared->outcome = std::move(produced);
            shared->complete = true;
        }
        shared->done.notify_one();
    }).detach();

    std::unique_lock<std::mutex> guard(shared->mutex);
    const auto finish = std::chrono::steady_clock::now() + timeout;
    if (!shared->done.wait_until(guard, finish,
                                 [&shared] { return shared->complete; })) {
        return Result<std::vector<DiskCandidate>>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(WAIT_TIMEOUT),
             L"enumerate_disk_candidates",
             L"enumerating the disk interfaces did not finish within its bound"});
    }
    return shared->outcome.has_value()
        ? std::move(*shared->outcome)
        : Result<std::vector<DiskCandidate>>::failure(
              {ErrorDomain::internal, 0, L"enumerate_disk_candidates",
               L"enumeration completed without producing a result"});
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

    // **Instrumented because two mechanism guesses were already falsified by measurement.**
    //
    // The probes in `tools/probe` showed this whole path costing about 2.5 ms and succeeding on an idle
    // guest, while a worker in the reclaim case hung for over eighty seconds with 0.23 s of CPU and a
    // ten-second bound on this very call expired.  Both facts cannot be about the same conditions, so the
    // difference must be the *state*: something about a disk or volume being held.  Guessing produced two
    // wrong answers, so from here the loop reports what it actually did -- how long each turn took, how
    // many candidates it saw, and whether it ever matched -- and the failing run explains itself.
    const auto started = std::chrono::steady_clock::now();
    int turns = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        ++turns;
        // Bounded, because this enumeration has no bound of its own and the deadline above is only
        // consulted once per turn -- see `enumerate_disk_candidates_within` for what that cost.
        const auto turn_started = std::chrono::steady_clock::now();
        auto cand = enumerate_disk_candidates_within(kEnumerateTimeout);
        const auto enumerate_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - turn_started)
                                      .count();
        if (!cand) {
            char line[256]{};
            (void)_snprintf_s(line, _TRUNCATE,
                             "wait_for_physical_drive: enumeration failed on turn %d after "
                             "%lld ms (total %lld ms) -- the bound fired",
                             turns, static_cast<long long>(enumerate_ms),
                             static_cast<long long>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count()));
            diagnostic_trace(line);
            return Result<LocatedDisk>::failure(cand.error());
        }

        mark_existed_before(cand.value());

        // Reported every turn while the loop is slow, because the question is precisely which turns are
        // slow and how many candidates they saw -- the probes saw one candidate and a millisecond.
        if (enumerate_ms > 100 || turns <= 3 || (turns % 25) == 0) {
            char line[256]{};
            (void)_snprintf_s(
                line, _TRUNCATE,
                "wait_for_physical_drive: turn %d saw %zu candidates in %lld ms (total %lld ms)",
                turns, cand.value().size(), static_cast<long long>(enumerate_ms),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count()));
            diagnostic_trace(line);
        }

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
        auto confirmed_candidates =
            enumerate_disk_candidates_within(kEnumerateTimeout);
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

// ── Present-disk wait (attach mode) ───────────────────────────

Result<LocatedDisk> wait_for_present_physical_drive(
    DeviceId created, std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout) {
    // Deliberately the *same* matching and confirmation logic as the create wait, with an empty
    // baseline -- and the empty baseline is the whole of the difference.
    //
    // `match_created_disk` skips any candidate flagged `existed_before`, which is exactly right
    // when waiting for a disk the adapter has just created and exactly wrong when adopting one
    // that was already there.  Marking nothing as pre-existing makes the match turn on the facts
    // that identify the device -- its SCSI address and its length -- rather than on when the
    // machine happened to enumerate it.
    //
    // Written as a call to the create wait rather than as a copy of it on purpose: the polling
    // interval, the repeat-confirmation step and the decision to treat a transient length
    // mismatch as "not yet" rather than as a wrong disk are all one behaviour, and duplicating
    // them would be duplicating the subtlest part of this file.
    return wait_for_created_physical_drive(
        created, std::span<const std::uint32_t>{}, expected_size_bytes, timeout);
}

// ── Retirement wait ───────────────────────────────────────────

namespace {

// True while a candidate sits at the created device's SCSI address carrying its
// expected length.
//
// `existed_before` is deliberately *not* consulted, unlike `match_created_disk`.
// There, a candidate wrongly marked pre-existing makes the create wait time out,
// which fails closed.  Here it would make the wait decide the device is already
// gone while it is still there -- a teardown reporting a removal that did not
// happen, which is the one direction this must never fail in.  The cost of the
// stricter reading is that a device which somehow reassumed the same address and
// length would hold the wait until its timeout, and that is the right way round.
bool present_at_identity(
    DeviceId own, std::span<const DiskCandidate> candidates,
    std::uint64_t expected_size_bytes) {

    const auto addr = decode_device_address(own);
    for (const auto& c : candidates) {
        if (c.path_id == addr.path_id && c.target_id == addr.target_id &&
            c.lun == addr.lun && c.length_bytes == expected_size_bytes) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

Result<void> wait_for_physical_drive_retired(
    DeviceId owned, std::uint64_t expected_size_bytes,
    std::chrono::milliseconds timeout) {

    if (expected_size_bytes == 0) {
        return Result<void>::failure(
            {ErrorDomain::evidence, ERROR_INVALID_PARAMETER,
             L"wait_for_physical_drive_retired",
             L"expected disk size must be non-zero"});
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto cand = enumerate_disk_candidates();
        if (!cand) return Result<void>::failure(cand.error());
        if (!present_at_identity(owned, cand.value(), expected_size_bytes)) {
            return Result<void>::success();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<void>::failure(
                {ErrorDomain::cleanup, WAIT_TIMEOUT,
                 L"wait_for_physical_drive_retired",
                 L"the created PhysicalDrive was still present at its identity"});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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
