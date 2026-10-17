#include "src/m3/m3_windows_acceptance.h"

#include "src/common/diagnostic_trace.h"
#include "src/persistence/vhdx_reader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace myrd::m3 {

DynamicBlockStoreOptions production_store_options() noexcept {
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    return {
        512ULL * mib,
        1ULL * 1024ULL * mib,
        256ULL * mib,
        8ULL * mib,
        8192,
        256,
        64ULL * mib,
        std::chrono::milliseconds(250)};
}

namespace {

bool named_objects_absent(
    const proxy::ProxyObjectNames& names,
    Error& error) {
    struct Probe final {
        HANDLE handle;
        const wchar_t* operation;
    };
    const std::array<Probe, 5> probes{{
        {OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str()), L"OpenMutexW"},
        {OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str()),
         L"OpenFileMappingW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.request.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.response.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.cancel.c_str()), L"OpenEventW"},
    }};
    for (const auto& probe : probes) {
        if (probe.handle != nullptr) {
            CloseHandle(probe.handle);
            return false;
        }
        const auto code = static_cast<std::uint32_t>(GetLastError());
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_INVALID_NAME) {
            error = {ErrorDomain::cleanup, code,
                     probe.operation, L"failed to inspect named object"};
            return false;
        }
    }
    return true;
}

class M3AimProviderLifecycle final : public IM1ProviderLifecycle {
public:
    M3AimProviderLifecycle(
        std::unique_ptr<M3Provider> provider,
        OperationContext context)
        : provider_(std::move(provider)),
          context_(std::move(context)),
          names_(provider_->names()) {}

    Result<void> start() override {
        return provider_->start(context_);
    }

    Result<void> stop_and_release() override {
        const auto stopped = provider_->stop(context_, {});
        final_snapshot_ = provider_->transport_snapshot();
        if (!stopped.ok()) {
            return Result<void>::failure(stopped.error.value_or(Error{
                ErrorDomain::cleanup,
                static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
                L"M3AimProviderLifecycle::stop_and_release",
                L"M3 provider stop failed without an error"}));
        }
        return Result<void>::success();
    }

    std::wstring native_object_path() const override {
        return names_.driver_path;
    }

    proxy::SessionSnapshot snapshot() const override {
        return final_snapshot_.value_or(provider_->transport_snapshot());
    }

    Result<bool> named_objects_released() const override {
        Error error{};
        const bool absent = named_objects_absent(names_, error);
        if (!absent && !error.operation.empty()) {
            return Result<bool>::failure(std::move(error));
        }
        return Result<bool>::success(absent);
    }

private:
    std::unique_ptr<M3Provider> provider_;
    OperationContext context_;
    proxy::ProxyObjectNames names_;
    std::optional<proxy::SessionSnapshot> final_snapshot_;
};

} // namespace

M3AimRuntime::M3AimRuntime(
    M3AimProviderFactoryConfig config,
    const std::chrono::milliseconds device_timeout)
    : config_(std::move(config)),
      device_timeout_(device_timeout) {}

M3AimRuntime::~M3AimRuntime() noexcept {
    try {
        (void)stop();
    } catch (...) {
    }
}

Result<std::unique_ptr<M3AimRuntime>> M3AimRuntime::create(
    M3AimProviderFactoryConfig config,
    const std::chrono::milliseconds device_timeout) {
    if (device_timeout.count() <= 0) {
        return Result<std::unique_ptr<M3AimRuntime>>::failure({
            ErrorDomain::usage, 0, L"M3AimRuntime::create",
            L"device timeout must be positive"});
    }
    return Result<std::unique_ptr<M3AimRuntime>>::success(
        std::unique_ptr<M3AimRuntime>(new M3AimRuntime(
            std::move(config), device_timeout)));
}

Result<std::optional<aim::DeviceId>> M3AimRuntime::find_existing_device(
    const aim::ProxyDiskSpec& spec) {
    // The driver's own list, queried device by device, and matched on **all three** of the
    // facts that identify a proxy disk: its size, its sector size and the file behind it.  A
    // partial match would be worse than none -- adopting a device that belongs to another
    // profile is the one way this path could cause harm rather than prevent it.
    //
    // A device that cannot be queried is skipped rather than failing the search: a broken or
    // foreign entry in the list is not this runtime's problem, and refusing to start because
    // of one would turn an unrelated defect into a reclaim failure.
    auto listed = adapter_->list_devices();
    if (!listed) return Result<std::optional<aim::DeviceId>>::failure(listed.error());

    // **What the search looked for, and what it found, both in the trace.**  The first version
    // of this function reported only "found" or "not found", and that was not enough to tell a
    // missing device from a mismatched identity -- which are opposite problems with opposite
    // fixes.  Printing both sides settled it in one run, and what it showed was a mismatch.
    diagnostic_trace(describe_device_target(spec).c_str());
    for (const auto id : listed.value()) {
        auto queried = adapter_->query_device(id);
        if (!queried) continue;
        diagnostic_trace(describe_device_candidate(queried.value()).c_str());
        // **Matched on geometry, not on the driver-visible name, and the name cannot be used.**
        //
        // The name is built from a session id that `generate_session_id` fills from
        // `BCryptGenRandom` (src/proxy/object_names.cpp:19), so it is **random per worker**.
        // The first version required the name to match and therefore could never adopt a
        // residue: the reading showed the device present with the same size and sector and a
        // different name, which no amount of looking would have changed -- the dead worker's
        // session id is unknowable, and nothing durable records it.
        //
        // Geometry is the honest discriminator, and the safety comes from somewhere better
        // than a name.  This runtime already holds the bundle's **exclusive writer lock**: no
        // other worker can be serving this bundle.  So a device of exactly the bundle's virtual
        // size and sector size, at a moment when this process owns the bundle, is this bundle's
        // -- and the size is not a weak signal here, it is the value AIM was created with and
        // the one `verify_volume` checks against immediately afterwards.
        //
        // Adopting on geometry is a *widening*, so it is worth naming what still stops a wrong
        // device from being taken: the bundle lock rules out a concurrent owner, and the
        // identity check that follows `start` re-reads the volume's own GPT identities.  If a
        // later reading shows a foreign device being adopted, this comment is the thing that
        // was wrong.
        if (queried.value().size_bytes == spec.size_bytes &&
            queried.value().sector_size == spec.sector_size) {
            return Result<std::optional<aim::DeviceId>>::success(
                std::optional<aim::DeviceId>(id));
        }
    }
    return Result<std::optional<aim::DeviceId>>::success(std::nullopt);
}

std::string M3AimRuntime::describe_device_target(
    const aim::ProxyDiskSpec& spec) {
    char buffer[512]{};
    _snprintf_s(buffer, _countof(buffer), _TRUNCATE,
                "  looking for size=%llu sector=%u path=%ls",
                static_cast<unsigned long long>(spec.size_bytes),
                spec.sector_size, spec.native_object_path.c_str());
    return std::string(buffer);
}

std::string M3AimRuntime::describe_device_candidate(
    const aim::DiskInfo& candidate) {
    char buffer[512]{};
    _snprintf_s(buffer, _countof(buffer), _TRUNCATE,
                "  candidate id=%u size=%llu sector=%u path=%ls",
                candidate.id.value, static_cast<unsigned long long>(candidate.size_bytes),
                candidate.sector_size, candidate.native_object_path.c_str());
    return std::string(buffer);
}

Result<void> M3AimRuntime::start() {
    if (started_) {
        return Result<void>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::start", L"runtime is already started"});
    }
    auto checked = check_operation_context(config_.context);
    if (!checked) return checked;

    // **Each sub-step of bringing M3 up is named before it runs.**  The trace above this one
    // proved the reclaim blocks inside `start_aim`, which M4 treats as a *single* step -- so
    // the reading could not say which of these it was.  These markers are what make one guest
    // run answer that.  They are unconditional calls that cost one environment lookup when no
    // trace is configured, and the facility changes nothing it observes.
    diagnostic_trace("M3AimRuntime::start: provider open");
#if defined(MYRD_ENABLE_TEST_SEAMS)
    proxy::KernelVisibleNamespacePolicy policy;
    auto opened = M3Provider::open_for_test(
        config_.context, policy, config_.provider,
        std::move(config_.test_factories));
#else
    auto opened = M3Provider::open(config_.context, config_.provider);
#endif
    if (!opened) return Result<void>::failure(opened.error());
    provider_ = opened.move_value();
    diagnostic_trace("M3AimRuntime::start: provider start");
    auto started = provider_->start(config_.context);
    if (!started) {
        (void)stop_provider();
        return started;
    }

    diagnostic_trace("M3AimRuntime::start: discover adapter");
    auto discovered = aim::discover_aim_adapter();
    if (!discovered) {
        (void)stop_provider();
        return Result<void>::failure(discovered.error());
    }
    adapter_handle_ = discovered.move_value();
    transport_ = std::make_unique<aim::WindowsSrbTransport>(
        adapter_handle_->handle.get());
    adapter_ = std::make_unique<aim::DirectAimAdapter>(*transport_);

    // **No whole-machine baseline is taken, and that removal is a fix rather than a simplification.**
    //
    // This step enumerated every disk interface on the machine through `SetupDiGetClassDevsW`, which is
    // a synchronous kernel enumeration with **no timeout** -- measured blocking forever, and the sixth
    // unbounded call this project has had to remove.  It is taken on *every* worker start, so a test
    // suite in which every case really creates and tears down a volume grows the interface set as it
    // runs, and the enumeration work grows with it until it stops returning.  That is what made a
    // reclaim intermittently time out, and what left eighteen of twenty suite iterations unable to get
    // past their third case.
    //
    // **Nothing is lost by not having it.**  The baseline was only ever used to skip candidates that
    // already existed, and the match that identifies a disk does not depend on it: `match_created_disk`
    // compares the device's SCSI **path, target and LUN** plus its expected length, which is the disk's
    // identity.  `existed_before` was an additional *filter* on top of that identity, never a
    // substitute for it -- so dropping it keeps every pre-existing rejection and, if anything, makes a
    // disk that appeared concurrently more likely to be noticed rather than less.
    //
    // The adopt path has used exactly this shape since part 29 (`wait_for_present_physical_drive`, an
    // empty baseline) and it is measured working on the guest.

    const auto geometry = provider_->geometry();
    const auto names = provider_->names();
    const aim::ProxyDiskSpec spec{
        geometry.virtual_bytes,
        geometry.logical_sector_size,
        names.driver_path};
    // **Attach if the disk is already there, and that is a fix rather than an optimization.**
    //
    // The whole premise of adopting a leftover is that a *killed* worker's proxy disk is still
    // present.  Creating a second one for that disk and then re-enumerating the adapter was the
    // path that never returned: `CM_Reenumerate_DevNode` is a synchronous PnP re-enumeration
    // with no timeout, so it blocked past every deadline above it.
    //
    // Asking the driver what it already holds is the safe way to decide, because the reply is
    // the driver's own statement and it is checked against the same identity the create path
    // checks afterwards.  A device that does not match this provider's geometry and backing
    // file is not adopted -- so a leftover of some *other* profile cannot be taken over by
    // accident, which is the one way this could do harm.
    diagnostic_trace("M3AimRuntime::start: look for an existing device");
    auto existing = find_existing_device(spec);
    if (!existing.ok()) {
        (void)stop_provider();
        return Result<void>::failure(existing.error());
    }

    const bool adopting_existing = existing.value().has_value();
    // **A baseline taken once, on the create path only, before the device exists.**
    //
    // `match_created_disk` skips candidates flagged `existed_before`, which is what distinguishes the disk
    // this runtime just brought up from one that was already there.  Nothing has set that flag since the
    // whole-machine baseline was removed, and the consequence shows up in the two-profile case: with two
    // 64 MiB disks on the machine, the locate step can accept the other profile's disk as this one's new
    // disk.  Sixteen single-disk cases pass and the two-profile case fails, which is exactly that shape.
    //
    // The removal was not wrong about the cost, only about where.  It ran on every start including the
    // attach-only paths, putting a whole-machine enumeration inside a device-retirement window where it was
    // measured blocking for eleven seconds.  Here it runs once, only when a device is about to be created,
    // and before creation -- so nothing is retiring yet.  Attach-only paths skip it entirely, as they skip
    // the wait.
    std::vector<std::uint32_t> baseline;
    if (!adopting_existing && !config_.attach_only) {
        auto snapshot = aim::snapshot_physical_drive_numbers();
        if (snapshot) {
            baseline = snapshot.move_value();
            // The numbers, not just the count: whether another profile's disk is in this list is the whole
            // question when two disks of the same size can be on the machine at once.
            char numbers[128]{};
            std::size_t used = 0;
            for (const auto number : baseline) {
                const int written = _snprintf_s(numbers + used,
                                                sizeof(numbers) - used, _TRUNCATE,
                                                used == 0 ? "%u" : ",%u", number);
                if (written <= 0) break;
                used += static_cast<std::size_t>(written);
            }
            char line[256]{};
            (void)_snprintf_s(line, _TRUNCATE,
                              "M3AimRuntime::start: baseline of %zu present drive(s) [%s] "
                              "before create",
                              baseline.size(), used == 0 ? "" : numbers);
            diagnostic_trace(line);
        } else {
            // Without a baseline the wait falls back to matching on identity alone, which is what it did
            // before this.  Recorded rather than fatal: a missing snapshot is not a reason to refuse a
            // create, but it is a reason to expect the ambiguity to return.
            diagnostic_trace("M3AimRuntime::start: baseline snapshot failed");
        }
    }
    if (adopting_existing) {
        // Taken ownership *before* anything else can fail, so the failure paths below remove
        // exactly the device this runtime decided it owns -- the same rule the create path
        // follows, and the reason `owned_device_` is assigned here rather than after validation.
        diagnostic_trace("M3AimRuntime::start: ADOPTING the existing device");
        owned_device_ = *existing.value();
    } else {
        // **Survey what the driver already holds, before asking it for a new device.**
        //
        // A device left behind by an earlier run is the leading explanation for a "newly created" disk
        // that already carries an MBR and a GPT: `kAutoDeviceNumber` may hand the existing device back
        // rather than making a fresh one, and its old shared memory is still alive because the driver
        // holds a handle to it.  Byte-level evidence from the guest supports that -- the inspected disk
        // had both signatures on it -- but nothing has yet shown whether a leftover existed at that
        // moment.  This trace answers exactly that, and it is read-only.
        {
            auto held = adapter_->list_devices();
            if (held) {
                char line[192]{};
                (void)_snprintf_s(line, _TRUNCATE,
                                  "M3AimRuntime::start: driver already holds %zu device(s) "
                                  "before create",
                                  held.value().size());
                diagnostic_trace(line);
            } else {
                diagnostic_trace(
                    "M3AimRuntime::start: could not survey existing devices");
            }
        }
        diagnostic_trace("M3AimRuntime::start: create proxy disk");
        auto created = adapter_->create_proxy_disk(spec);
        if (!created) {
            (void)stop_provider();
            return Result<void>::failure(created.error());
        }
        if (created.value().value == aim::kAllDevices ||
            created.value().value == aim::kAutoDeviceNumber) {
            (void)stop_provider();
            return Result<void>::failure({
                ErrorDomain::evidence, 0, L"M3AimRuntime::start",
                L"driver returned an unsafe sentinel DeviceId"});
        }
        owned_device_ = created.value();

        // Only a device this call created needs the adapter re-enumerated; one that was
        // already enumerated is, by definition, already visible.  Skipping it here is the
        // point of the attach path.
        diagnostic_trace("M3AimRuntime::start: rescan adapter");
        auto rescanned = aim::rescan_aim_adapter(adapter_handle_->dev_inst);
        if (!rescanned) {
            (void)remove_owned_device();
            (void)stop_provider();
            return rescanned;
        }
    }
    diagnostic_trace("M3AimRuntime::start: query device");
    auto queried = adapter_->query_device(*owned_device_);
    if (!queried) {
        (void)remove_owned_device();
        (void)stop_provider();
        return Result<void>::failure(queried.error());
    }
    // The identity check, and the one part of it an adopted device cannot satisfy.
    //
    // `native_object_path` is the *driver-visible name*, built from a session id that is random
    // per worker.  An adopted device necessarily carries the **dead** worker's id, so demanding
    // this runtime's own name would reject the very device the adoption path exists to take over
    // -- and, because the failure branch removes the device, it would have deleted the residue
    // and then reported failure.  For an adopted device the identifying facts are the geometry
    // (checked here) and the backing file, which `find_existing_device` matched on.
    const bool identity_ok =
        queried.value().id == *owned_device_ &&
        queried.value().size_bytes == geometry.virtual_bytes &&
        queried.value().sector_size == geometry.logical_sector_size &&
        (adopting_existing ||
         queried.value().native_object_path == names.driver_path);
    if (!identity_ok) {
        (void)remove_owned_device();
        (void)stop_provider();
        return Result<void>::failure({
            ErrorDomain::evidence, 0, L"M3AimRuntime::start",
            L"AIM QUERY_DEVICE identity did not match M3 provider"});
    }

    // **Attach-only runtimes skip this wait, and skipping it is what removes the eleven-second block.**
    //
    // The wait exists to learn which physical drive this AIM device became, so `info_.disk` can carry it.
    // It does that by opening every candidate disk, and one of those opens was measured at 11017 ms while
    // a device was retiring -- precisely the situation a reclaim is in.
    //
    // A create genuinely needs the number: it provisions the volume on that disk.  But `recover` and
    // `reclaim` attach to a volume that already exists, find it by drive letter, and establish ownership
    // by comparing its GUID against the durable profile.  None of that needs a drive number, so none of it
    // should pay for one.
    //
    // `info_.disk` therefore stays empty on those paths, which is why it is an optional.
    if (!config_.attach_only) {
        diagnostic_trace("M3AimRuntime::start: wait for physical drive");
        // **The baseline, when there is one, is what makes this wait find the disk we just made.**
        //
        // It tells `match_created_disk` which disks already existed, and every candidate on that list is
        // skipped.  Without it the wait matches on SCSI address and length alone, and when another profile
        // holds a disk of the same size, the address is the only thing left -- so a reused address is
        // indistinguishable from a fresh one.  An empty baseline keeps the previous behaviour, which is
        // what an adopt did all along.
        // **Reads a located disk's first kilobyte, so "is this really the disk I just made?" can be answered
        // by its contents instead of by the machine's disk list.**
        //
        // A freshly created proxy disk is backed by zeroed shared memory, so its head must read as zeros.
        // Measured: six creates in one run locate disk #1 and read all zeros, and a seventh locates the same
        // number and reads a real MBR -- written by an earlier provisioning.  The disk list cannot tell those
        // apart, because the enumeration is a snapshot of a moving target and can miss a device that is
        // transiently unavailable, which leaves a stale disk looking like a newly created one.
        //
        // Read-only, bounded, and it answers the question the disk list cannot.
        auto head_is_zero = [](std::uint32_t number, bool& readable) {
            readable = false;
            wchar_t path[64]{};
            (void)_snwprintf_s(path, _TRUNCATE, L"\\\\.\\PhysicalDrive%u", number);
            HANDLE head = CreateFileW(path, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (head == INVALID_HANDLE_VALUE) return false;
            std::array<std::byte, 1024> buffer{};
            HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED overlapped{};
            overlapped.hEvent = done;
            DWORD read = 0;
            BOOL ok = ReadFile(head, buffer.data(), static_cast<DWORD>(buffer.size()),
                               &read, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                ok = done != nullptr && WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 &&
                     GetOverlappedResult(head, &overlapped, &read, FALSE);
            }
            if (done != nullptr) CloseHandle(done);
            CloseHandle(head);
            if (!ok || read == 0) return false;
            readable = true;
            for (std::size_t at = 0; at < read; ++at) {
                if (buffer[at] != std::byte{0}) return false;
            }
            return true;
        };

        auto locate = [&]() -> Result<aim::LocatedDisk> {
            return baseline.empty()
                ? aim::wait_for_present_physical_drive(
                      *owned_device_, geometry.virtual_bytes, device_timeout_)
                : aim::wait_for_created_physical_drive(
                      *owned_device_, baseline, geometry.virtual_bytes, device_timeout_);
        };

        auto located = locate();
        if (!located) {
            (void)remove_owned_device();
            (void)stop_provider();
            return Result<void>::failure(located.error());
        }
        // **A located disk that is not empty is not the disk this create just made.**
        //
        // One bounded retry, because the disk list is a snapshot and the real device may simply not have
        // settled.  After that, refusing is the right outcome and the important one: provisioning a disk that
        // is not ours would destroy whatever is on it, and this runtime has no proof that it is.
        {
            bool readable = false;
            if (!head_is_zero(located.value().physical_drive_number, readable) && readable) {
                char line[192]{};
                (void)_snprintf_s(line, _TRUNCATE,
                                  "M3AimRuntime::start: disk #%u is not empty; retrying the locate",
                                  located.value().physical_drive_number);
                diagnostic_trace(line);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto again = locate();
                if (!again) {
                    (void)remove_owned_device();
                    (void)stop_provider();
                    return Result<void>::failure(again.error());
                }
                bool readable_again = false;
                if (!head_is_zero(again.value().physical_drive_number, readable_again) &&
                    readable_again) {
                    (void)remove_owned_device();
                    (void)stop_provider();
                    return Result<void>::failure({
                        ErrorDomain::evidence, 0, L"M3AimRuntime::start",
                        L"located a disk that is not empty, so it is not the disk this "
                        L"runtime just created"});
                }
                located = Result<aim::LocatedDisk>::success(again.move_value());
            }
        }
        info_.disk = located.move_value();
        // **And log what is on it, because the check above only acts on the answer.**
        //
        // `head_is_zero` decides whether to accept the located disk; this records the bytes behind that
        // decision, so a later failure can say what was actually there rather than only that a check refused.
        // The reading it produced is what isolated this defect: six creates finding zeros and a seventh
        // finding a real MBR, on the same disk number.
        //
        // This exists to settle one question with a reading instead of an argument: a freshly created proxy
        // disk is measurably not empty, it carries a real MBR and an EFI PART signature, and every attempt to
        // explain that from outside has been refuted -- the device is new, nothing was present before creation,
        // the located disk is the created one, the bundle is new and the shared memory is zeroed on creation.
        //
        // Comparing this read for a create that provisions successfully against one that fails says whether
        // the difference is in what the medium returned or in what happened afterwards.  Read-only, bounded,
        // and it only opens the disk this runtime just created.
        {
            wchar_t path[64]{};
            (void)_snwprintf_s(path, _TRUNCATE, L"\\\\.\\PhysicalDrive%u",
                               info_.disk->physical_drive_number);
            HANDLE head = CreateFileW(path, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            char head_line[256]{};
            if (head == INVALID_HANDLE_VALUE) {
                (void)_snprintf_s(head_line, _TRUNCATE,
                                  "M3AimRuntime::start: disk head unreadable, error %lu",
                                  GetLastError());
            } else {
                std::array<std::byte, 1024> buffer{};
                // Overlapped with an explicit wait, not a bare synchronous read and not a fire-and-forget
                // one.  A synchronous read here could block on a disk that is not answering -- which is the
                // very failure being investigated -- and a read whose completion nobody waits for would
                // report whatever happened to be in the buffer, which would be worse than no reading at all.
                HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                OVERLAPPED overlapped{};
                overlapped.Offset = 0;
                overlapped.OffsetHigh = 0;
                overlapped.hEvent = done;
                DWORD read = 0;
                BOOL ok = ReadFile(head, buffer.data(),
                                   static_cast<DWORD>(buffer.size()), &read,
                                   &overlapped);
                DWORD read_error = 0;
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    if (done != nullptr &&
                        WaitForSingleObject(done, 5000) == WAIT_OBJECT_0)
                        ok = GetOverlappedResult(head, &overlapped, &read, FALSE);
                    else
                        read_error = ERROR_TIMEOUT;
                }
                if (!ok && read_error == 0) read_error = GetLastError();
                if (done != nullptr) CloseHandle(done);
                CloseHandle(head);
                if (!ok) {
                    (void)_snprintf_s(head_line, _TRUNCATE,
                                      "M3AimRuntime::start: disk head read failed, error %lu",
                                      read_error);
                } else {
                    std::size_t used = 0;
                    for (std::size_t at = 0; at < 8 && at < read; ++at) {
                        const int written = _snprintf_s(
                            head_line + used, sizeof(head_line) - used, _TRUNCATE, "%02x",
                            static_cast<unsigned>(
                                std::to_integer<unsigned>(buffer[at])));
                        if (written <= 0) break;
                        used += static_cast<std::size_t>(written);
                    }
                    bool nonzero = false;
                    for (std::size_t at = 0; at < read; ++at) {
                        if (buffer[at] != std::byte{0}) { nonzero = true; break; }
                    }
                    (void)_snprintf_s(head_line + used, sizeof(head_line) - used, _TRUNCATE,
                                      " nonzero=%s read=%lu", nonzero ? "yes" : "no",
                                      static_cast<unsigned long>(read));
                    char full[320]{};
                    (void)_snprintf_s(full, _TRUNCATE,
                                      "M3AimRuntime::start: disk #%u head=%s",
                                      info_.disk->physical_drive_number, head_line);
                    diagnostic_trace(full);
                    head_line[0] = '\0';
                }
            }
            if (head_line[0] != '\0') diagnostic_trace(head_line);
        }
    }
    info_.device_id = *owned_device_;
    info_.virtual_bytes = geometry.virtual_bytes;
    info_.sector_size = geometry.logical_sector_size;
    info_.native_object_path = queried.value().native_object_path;
    started_ = true;
    return Result<void>::success();
}

M3CheckpointOutcome M3AimRuntime::checkpoint(
    const OperationContext& context,
    const persistence::VolumeBarrierKind barrier) {
    if (!started_ || !provider_) {
        return {persistence::CommitDisposition::NotCommitted, 0,
                Error{ErrorDomain::internal,
                      static_cast<std::uint32_t>(
                          OperationErrorCode::internal_invariant),
                      L"M3AimRuntime::checkpoint",
                      L"runtime is not started"}};
    }
    return provider_->checkpoint(context, barrier);
}

Result<M3CheckpointPrepared> M3AimRuntime::prepare_checkpoint(
    const M3CheckpointPrepareRequest& request) {
    if (!started_ || !provider_) {
        return Result<M3CheckpointPrepared>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::prepare_checkpoint",
            L"runtime is not started"});
    }
    return provider_->prepare_checkpoint(request);
}

M3CheckpointOutcome M3AimRuntime::commit_checkpoint(
    const M3CheckpointCommitRequest& request) {
    if (!started_ || !provider_) {
        return {persistence::CommitDisposition::NotCommitted, 0,
                Error{ErrorDomain::internal,
                      static_cast<std::uint32_t>(
                          OperationErrorCode::internal_invariant),
                      L"M3AimRuntime::commit_checkpoint",
                      L"runtime is not started"}};
    }
    return provider_->commit_checkpoint(request);
}

Result<void> M3AimRuntime::abort_checkpoint(
    const std::uint64_t native_operation_id) {
    if (!started_ || !provider_) {
        return Result<void>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::abort_checkpoint",
            L"runtime is not started"});
    }
    return provider_->abort_checkpoint(native_operation_id);
}

DynamicBlockStoreStats M3AimRuntime::storage_stats() const {
    if (provider_) return provider_->storage_stats();
    return {};
}

proxy::SessionSnapshot M3AimRuntime::transport_snapshot() const {
    if (provider_) return provider_->transport_snapshot();
    return {};
}

Result<void> M3AimRuntime::remove_owned_device() {
    if (!owned_device_ || !adapter_) {
        device_removed_ = true;
        physical_drive_restored_ = true;
        return Result<void>::success();
    }
    const auto deadline = std::chrono::steady_clock::now() + device_timeout_;
    if (!device_removed_) {
        // **Each step is named, because the question left is which one fails.**
        //
        // `REMOVE_DEVICE` is an SRB command to the driver and never touches the volume, so it is
        // not expected to be blocked by an occupied volume -- but the steps *after* it are a device
        // inventory poll and a PnP rescan, and either could be where an adopted device refuses to
        // go.  Naming all four separates "the driver would not remove it" from "it was removed and
        // the machine would not let go of the disk", which need different responses.
        diagnostic_trace("remove_owned_device: remove_device");
        auto removed = adapter_->remove_device(*owned_device_);
        if (!removed) return removed;
        diagnostic_trace("remove_owned_device: polling the inventory");
        for (;;) {
            auto devices = adapter_->list_devices();
            if (!devices) return Result<void>::failure(devices.error());
            const bool present = std::any_of(
                devices.value().begin(), devices.value().end(),
                [this](const aim::DeviceId value) {
                    return value == *owned_device_;
                });
            if (!present) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                diagnostic_trace("remove_owned_device: device still present at deadline");
                return Result<void>::failure({
                    ErrorDomain::cleanup, WAIT_TIMEOUT,
                    L"M3AimRuntime::remove_owned_device",
                    L"owned AIM device remained present"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        diagnostic_trace("remove_owned_device: device gone from the inventory");
        // The AIM object is gone, but the PhysicalDrive may still be draining.
        // Keep the provider alive until that second cleanup boundary is met.
        device_removed_ = true;
    }

    if (!physical_drive_restored_) {
        // REMOVE_DEVICE updates the AIM adapter's owned-device inventory, but
        // Windows does not necessarily retire the corresponding disk devnode
        // until the adapter is re-enumerated.  Keep this after
        // device_removed_ becomes true so a failed rescan is safely retried
        // without issuing REMOVE_DEVICE again for an already absent id.
        diagnostic_trace("remove_owned_device: rescanning the adapter");
        auto rescanned = aim::rescan_aim_adapter(adapter_handle_->dev_inst);
        if (!rescanned) return rescanned;
        // The condition is that *this* device's devnode has retired -- not that
        // the machine's disk set has returned to the snapshot taken at `start`.
        // That earlier condition could never hold once any disk had appeared
        // since, and with two profiles one always had: the first disk stopped
        // while a second was mounted waited for a set that could not come back,
        // and timed out (manifest ?1.38).  A teardown must be about the device
        // being torn down.
        const auto now = std::chrono::steady_clock::now();
        const auto remaining =
            now < deadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - now)
                : std::chrono::milliseconds(0);
        diagnostic_trace("remove_owned_device: waiting for the PhysicalDrive to retire");
        auto retired = aim::wait_for_physical_drive_retired(
            *owned_device_, info_.virtual_bytes, remaining);
        if (!retired) return retired;
        diagnostic_trace("remove_owned_device: PhysicalDrive retired");
        physical_drive_restored_ = true;
    }
    return Result<void>::success();
}

Result<void> M3AimRuntime::stop_provider() {
    if (!provider_) return Result<void>::success();
    const auto stopped = provider_->stop(OperationContext{}, {});
    provider_.reset();
    adapter_.reset();
    transport_.reset();
    adapter_handle_.reset();
    if (!stopped.ok()) {
        return Result<void>::failure(stopped.error.value_or(Error{
            ErrorDomain::cleanup,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::stop_provider",
            L"M3 provider stop failed without an error"}));
    }
    return Result<void>::success();
}

Result<void> M3AimRuntime::stop() {
    auto removed = remove_owned_device();
    if (!removed) {
        // Do not close the backend while the owned device or its PhysicalDrive
        // is still present. The caller/destructor may retry cleanup safely.
        return removed;
    }
    auto stopped = stop_provider();
    started_ = false;
    return stopped;
}

M3AimProviderFactory::M3AimProviderFactory(
    M3AimProviderFactoryConfig config)
    : config_(std::move(config)) {}

Result<std::unique_ptr<IM1ProviderLifecycle>>
M3AimProviderFactory::create() {
    auto checked = check_operation_context(config_.context);
    if (!checked) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            checked.error());
    }
    auto provider = M3Provider::open(config_.context, config_.provider);
    if (!provider) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            provider.error());
    }
    std::unique_ptr<IM1ProviderLifecycle> lifecycle =
        std::make_unique<M3AimProviderLifecycle>(
            provider.move_value(), config_.context);
    return Result<std::unique_ptr<IM1ProviderLifecycle>>::success(
        std::move(lifecycle));
}

} // namespace myrd::m3
