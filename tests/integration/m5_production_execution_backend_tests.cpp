// The M5 execution backend against a real worker process.
//
// This is the suite that executes what increments 10-12 wrote: the launcher, the
// session provider, and the adapter that drives them.  Everything before it was
// reviewed-but-unexecuted code, and this is where that stops being true.
//
// Most of what is here is the service-to-worker hop: process launch, inherited
// startup channel, pipe ACL, handshake, binding, and the commands that carry a
// profile's lifecycle across it.  The runtime lifecycle below that hop is the
// M4/M3 suites' subject, and these cases deliberately leave it alone -- a launch
// that adopts a bundle stops at the bundle kind, and one that fails to reach the
// worker at all is distinguished from one that reached it by *which* failure
// comes back.
//
// `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` is the exception, and
// it is the only case in this tree that drives the whole thing: a real
// generation-zero image, a real AIM proxy disk, a real provisioned and formatted
// volume, a real drive letter, mounted and released.  It claims a letter it has
// verified is free and releases it on every path, and it is slow enough that the
// suite's own timeout has to allow for a provisioning round-trip.

#include "tests/support/test_framework.h"

#include "src/common/operation_context.h"
#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_job.h"
#include "src/management/worker_binding_store.h"
#include "src/management/windows_worker_sessions.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"
#include "src/run/windows_volume.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include <tlhelp32.h>

namespace {

using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
}

// A budget for the one command that does real work.  The session refuses a
// deadline beyond what a worker may be asked to wait, and a create provisions a
// volume through PowerShell, so the default 30 seconds is a guess that would
// show up as a transport timeout rather than as the result it was waiting for.
OperationContext provisioning_context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(100)};
}

// `count` drive letters this machine is not using, verified rather than assumed.
// A real lifecycle claims a real letter here, so a case that guessed one would
// take a drive someone else is using -- or fail for a reason that has nothing to
// do with what it is testing.  Scanned from the end of the alphabet downwards,
// because that is where a desktop is least likely to keep something.
std::vector<wchar_t> free_drive_letters(const std::size_t count) {
    const DWORD in_use = GetLogicalDrives();
    std::vector<wchar_t> letters;
    for (wchar_t letter = L'Z'; letter >= L'R' && letters.size() != count;
         --letter) {
        if ((in_use & (1u << (letter - L'A'))) == 0) letters.push_back(letter);
    }
    REQUIRE(letters.size() == count);
    return letters;
}

wchar_t free_drive_letter() { return free_drive_letters(1).front(); }

bool is_letter_assigned(const wchar_t letter) {
    return (GetLogicalDrives() & (1u << (letter - L'A'))) != 0;
}

// Waits for a letter to come free again.  The teardown is asynchronous once the
// channel closes -- the worker notices, runs cleanup, and releases the mount --
// so a bare check would be a race, and sleeping a fixed guess would be a test
// that passes for the wrong reason.
bool wait_for_letter_release(const wchar_t letter) {
    for (int attempt = 0; attempt != 150; ++attempt) {
        if (!is_letter_assigned(letter)) return true;
        Sleep(100);
    }
    return !is_letter_assigned(letter);
}

// Serializes the heavy cases: the next one starts only once the previous case's
// worker has exited.
//
// **What this is for, exactly, because it was removed once for the wrong
// reason.** The heavy cases each bring up an AIM proxy disk, and PhysicalDrive
// devnodes are a *machine-wide* namespace.  One case's worker still retiring a
// device while the next case enumerates or creates its own is interference
// between two cases, not a defect in either: the symptom is
// `CreateFileW(disk interface)` / `failed to open a present disk interface for
// identity` on the create side, or a removal refused because a device that is
// not its own is appearing.  Waiting for the previous worker to exit is the
// point at which its teardown has run, so this closes the window.
//
// It is **not** a fix for anything, and it is deliberately not load-bearing for
// the two-disk case (??.38): that case creates *both* of its workers inside
// itself, so a barrier taken at case start cannot affect it and its
// failing-before / passing-after evidence stands on the M3 fix alone.
//
// It was deleted in ??.38 on the argument that it made "fixed" self-certifying.
// That argument does not hold for the defect ??.38 found, and the deletion was
// measured afterwards to reintroduce this inter-case class: with the same M3
// code, the suite failed 2 of 5 isolated runs without this barrier and 0 of 4
// with it (??.39).
// Whether a snapshot entry names a process that is still running.
//
// `CreateToolhelp32Snapshot` lists a process that has exited but whose handle is
// still open somewhere.  A test run killed from outside leaves exactly that --
// the worker is gone, but it lingers in the snapshot, and a barrier that counted
// it would wait out its whole timeout on every case for the rest of the boot
// session.  Such a process cannot be tearing a device down, so it must not hold
// the barrier.
//
// "Could not tell" holds the barrier, which is the direction it is allowed to be
// wrong in.
enum class ProcessState { exited, alive, unknown };

ProcessState process_state(const DWORD pid) {
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return ProcessState::unknown;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(process, &code);
    CloseHandle(process);
    if (ok == FALSE) return ProcessState::unknown;
    return code == STILL_ACTIVE ? ProcessState::alive : ProcessState::exited;
}

const char* process_state_name(const ProcessState state) {
    switch (state) {
    case ProcessState::exited: return "exited";
    case ProcessState::alive: return "alive";
    case ProcessState::unknown: return "unknown";
    }
    return "unknown";
}

bool wait_for_no_worker_process() {
    for (int attempt = 0; attempt != 300; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        DWORD found_pid = 0;
        ProcessState found_state = ProcessState::unknown;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (std::wstring_view(entry.szExeFile) != L"myrd-m5-worker.exe") {
                    continue;
                }
                const auto state = process_state(entry.th32ProcessID);
                if (state == ProcessState::exited) continue;
                found = true;
                found_pid = entry.th32ProcessID;
                found_state = state;
                break;
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!found) return true;
        if (attempt == 299) {
            // Named rather than silent, for the same reason `require_action`
            // exists: a bare timeout says nothing about what was still running.
            std::fprintf(stderr,
                         "myrd-test: barrier held by myrd-m5-worker pid=%lu state=%s\n",
                         static_cast<unsigned long>(found_pid),
                         process_state_name(found_state));
        }
        Sleep(100);
    }
    return false;
}

// The worker binary the build just produced, found relative to this test's own
// module.  The relative path comes from CMake so the build root is not baked
// into the binary, and existence is asserted rather than assumed: a layout
// change must fail with the path it tried, not launch nothing and report a
// product defect.
std::wstring worker_executable() {
    std::wstring module(32 * 1024, L'\0');
    const auto written = GetModuleFileNameW(nullptr, module.data(),
                                            static_cast<DWORD>(module.size()));
    REQUIRE(written != 0 && written < module.size());
    module.resize(written);
    const auto path =
        (std::filesystem::path(module).parent_path()
         / std::filesystem::path(MYRD_M5_WORKER_RELPATH))
            .lexically_normal();
    REQUIRE(std::filesystem::exists(path));
    return path.wstring();
}

// The bundle path only has to be non-empty: the worker validates its config
// before the handshake but does not open the bundle until `start`, and these
// cases never reach a bundle.  Creating the directory keeps the config honest
// rather than pointing at somewhere that does not exist.
class TempBundle final {
public:
    explicit TempBundle(const wchar_t* tag) {
        root = std::filesystem::temp_directory_path()
            / (std::wstring(L"myrd-m5-backend-") + tag + L"-"
               + std::to_wstring(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()));
        std::filesystem::create_directories(root);
    }
    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

// `profile::Profile` holds a `product::Uuid`, which has no public default
// constructor, so it cannot be value-initialized -- every case builds it through
// the aggregate, and the field order below is the struct's own.
// The lifecycle is a parameter because it decides which lifecycle a launch
// runs, and that is not a detail of this fixture.  It defaults to `ready`
// deliberately: a `provisioning` profile makes the worker *create and provision*
// a real volume on this machine, and a case that only wants to prove the worker
// was reached must not do that as a side effect of the fixture it borrowed.
profile::ProfileRecord profile_record(const product::Uuid& profile_id,
                                      const product::Uuid& image_id,
                                      const std::wstring& bundle_path,
                                      const std::wstring& mount_target = L"R:",
                                      const std::uint64_t revision = 7,
                                      const profile::ProfileLifecycle lifecycle =
                                          profile::ProfileLifecycle::ready) {
    profile::Profile value{
        profile_id,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        lifecycle,
        bundle_path,
        "disk.rdm",
        image_id,
        8ULL * 1024 * 1024,
        512,
        4096,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        profile::MountKind::drive_letter,
        mount_target,
        false,
        profile::MemoryPolicy{},
        profile::CheckpointPolicy{},
        profile::CompactionPolicy{}};
    return profile::ProfileRecord{revision, std::move(value)};
}

profile::ProfileRecord valid_profile(const TempBundle& bundle) {
    return profile_record(uuid("11111111-2222-3333-4444-555555555555"),
                          uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"),
                          bundle.root.wstring());
}

// What an end-to-end create asks for.  Built as the *intent*, not as a durable
// record, because that is what production has at this point: the profile is not
// published until after the bundle action the case below drives.
constexpr std::uint64_t kCapacityBytes = 64ull * 1024 * 1024;

LifecycleOperationIntent create_intent_for(const product::Uuid& profile_id,
                                           const product::Uuid& image_id,
                                           const TempBundle& bundle,
                                           const std::wstring& mount_target) {
    return LifecycleOperationIntent{
        LifecycleOperationKind::create,
        profile_id,
        image_id,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        bundle.root.wstring(),
        mount_target,
        kCapacityBytes,
        false,
        control::MemoryMode::automatic,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        PhysicalMemorySnapshot{},
        0};
}

LifecycleOperationIntent create_intent(const product::Uuid& image_id,
                                       const TempBundle& bundle,
                                       const std::wstring& mount_target) {
    return create_intent_for(uuid("11111111-2222-3333-4444-555555555555"),
                             image_id, bundle, mount_target);
}

// The launched image id is a product uuid and the worker reports the
// persistence layer's sixteen raw bytes.  The bytes are the same sixteen in the
// same order, so this is the comparison the product itself makes.
persistence::ImageUuid image_bytes(const product::Uuid& id) noexcept {
    persistence::ImageUuid value{};
    const auto bytes = id.bytes();
    std::memcpy(value.bytes.data(), bytes.data(), value.bytes.size());
    return value;
}

// The durable root the installer created, which is where the fingerprint store lives
// in production.  Read from the environment rather than hardcoded: the installer decides
// where it lives, and a test that hardcoded it would pass here and mislead elsewhere.
std::wstring durable_root() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer,
                                                 static_cast<DWORD>(MAX_PATH));
    REQUIRE(length != 0);
    REQUIRE(length < MAX_PATH);
    return std::wstring(buffer, length);
}

// The provider takes a launch request, not a profile: a launch no longer always
// starts from a durable record, so the seam's input is what both producers
// translate into.  Going through the translation rather than building a request
// by hand is deliberate -- a hand-built one would not be the thing production
// sends.
WorkerLaunchRequest request_for(const profile::ProfileRecord& profile) {
    auto translated = make_launch_request(profile);
    REQUIRE(translated.ok());
    return translated.value();
}

// The worker job, kept alive for one test case.
//
// Every launch has to go into the job (see `launch_private_worker`): a worker
// outside it is invisible to a startup sweep, so the production composition
// always has one and these cases must not test a shape production never builds.
// The fixture holds the source so its lifetime brackets the provider's, which
// only borrows it.
class WorkerJobFixture final {
public:
    WorkerJobFixture() {
        auto created = WindowsWorkerProcessSource::create();
        REQUIRE(created.ok());
        source_ = created.move_value();
    }

    [[nodiscard]] WindowsWorkerProcessSource& source() const noexcept {
        return *source_;
    }

private:
    std::unique_ptr<WindowsWorkerProcessSource> source_;
};

// A job the test can open a *second* time, to see what a restarted service would
// see.  `WindowsWorkerProcessSource::create` opens the named job rather than
// making a new one, so this is the restart path itself and not a simulation of
// it.
std::unique_ptr<WindowsWorkerProcessSource> second_job_source() {
    auto opened = WindowsWorkerProcessSource::create();
    REQUIRE(opened.ok());
    return opened.move_value();
}

} // namespace

TEST_CASE(m5_backend_launches_and_authenticates_a_real_worker) {
    // The whole point of the increment: a real process starts, reads the spec
    // from the inherited channel, connects to a pipe whose ACL admits only the
    // service and itself, and proves it did by echoing the startup secret.  None
    // of that can be faked without testing a different object than the one that
    // runs.
    TempBundle bundle(L"launch");
    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    auto session = provider.launch(request_for(valid_profile(bundle)), context());
    REQUIRE(session.ok());
    REQUIRE(session.value() != nullptr);

    // The binding is the child's *own* pid and process creation time, checked
    // against the process the launcher started.  A pid alone would not be an
    // identity -- Windows recycles them -- so both travel.
    const auto& binding = session.value()->binding();
    REQUIRE(binding.profile_id == uuid("11111111-2222-3333-4444-555555555555"));
    REQUIRE(binding.process_id != 0);
    REQUIRE(binding.process_creation_time != 0);

    // The runtime has not been started, so a status read is refused.  That is
    // what proves the worker authenticated *before* touching a bundle: a worker
    // that had opened one would have a runtime to report on.
    //
    // The assertion is on *whose* refusal it is, not merely that the call
    // failed.  `observe` is the operation the worker's own command loop names
    // when it refuses a status for a runtime it has not started, so an error
    // carrying it can only have come from that loop, over the authenticated
    // channel.  A launch that never happened, or a handshake that silently
    // no-opped, would fail here with something else -- and a bare `!status.ok()`
    // could not tell the two apart.
    const auto status = session.value()->status(context());
    REQUIRE(!status.ok());
    REQUIRE(status.error().operation == L"observe");

    // Reading a reply also advances the session's sequence, which only moves
    // when a request was actually written.
    REQUIRE(session.value()->last_sequence() >= 1);

    // Shutdown is accepted with no runtime: a worker that was never started is
    // still a worker that can be told to go away.
    REQUIRE(session.value()->shutdown(context(),
                                      uuid("99999999-8888-7777-6666-555555555555"))
                .ok());
}

TEST_CASE(m5_backend_launch_places_the_worker_in_the_named_job) {
    // The job membership is a property of the process from its first instruction,
    // because it rides the creation attributes.  A separate
    // `AssignProcessToJobObject` after `CreateProcessW` returns would leave a
    // window in which the worker runs outside every job, and anything it created
    // in that window would not be the job's.
    //
    // Measured from a *second* handle to the same named job -- which is exactly
    // what a restarted service opens -- while the worker is still running: a live
    // worker this session launched is invisible to this session (it is not an
    // orphan), but a *reopened* job has no memory of launching it, so the
    // worker's presence there is the statement that membership is real.
    TempBundle bundle(L"job-membership");
    WorkerJobFixture job;
    WindowsWorkerSessionProvider provider(worker_executable(), job.source());

    auto session = provider.launch(request_for(valid_profile(bundle)), context());
    REQUIRE(session.ok());
    const std::uint32_t worker_pid = session.value()->binding().process_id;
    const std::uint64_t worker_creation_time =
        session.value()->binding().process_creation_time;
    REQUIRE(worker_pid != 0);
    REQUIRE(worker_creation_time != 0);

    // Held open, so the worker has no reason to leave while this is measured.
    auto observer = second_job_source();
    auto orphans = observer->observe_orphans();
    REQUIRE(orphans.ok());

    bool found = false;
    for (const auto& orphan : orphans.value()) {
        if (orphan.process_id == worker_pid) {
            found = true;
            // The same process, by the identity that cannot be reused.  A pid
            // alone would be satisfied by a recycled one.
            REQUIRE(orphan.process_creation_time.has_value());
            REQUIRE_EQ(*orphan.process_creation_time, worker_creation_time);
        }
    }
    // Present.  Had the launch not joined the job, the reopened job would not
    // report it at all -- the difference between a worker a sweep can account for
    // and one nothing can.
    REQUIRE(found);

    REQUIRE(session.value()->shutdown(context(),
                                      uuid("99999999-8888-7777-6666-555555555555"))
                .ok());
    REQUIRE(job.source().wait_for_exit(worker_pid, 30'000).ok());
    REQUIRE(job.source().wait_for_exit(worker_pid, 30'000).value());
}

TEST_CASE(m5_backend_bundle_registration_persists_the_identity_it_just_read) {
    // The producing half of design section 5.5, end to end: a worker opens and
    // verifies a bundle, and the identity it read off that handle reaches the durable
    // fingerprint rather than dying with the return value.
    //
    // Before this existed, `amend_bundle_identity` had no caller at all -- the same
    // "no consumer" pattern the reclamation decision itself had -- so every profile's
    // leftovers were unprovable and the sweep could only report them.
    //
    // Everything is real: a real worker, a real bundle it creates, and the real
    // `WorkerBindingStore` as the sink.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"fingerprint-amend");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("3f3f3f3f-4e4e-4d4d-5c5c-6d6d6d6d6d6d");
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto operation = uuid("88888888-7777-6666-5555-444444444444");

    auto store = WorkerBindingStore::over_protected_root(durable_root());
    REQUIRE(store.ok());
    // The record a launch would have written: a process identity and no bundle
    // identity, which is exactly the state this case is about improving.
    REQUIRE(store.value()
                .record(WorkerFingerprintRecord{profile, 4242,
                                                133'700'000'000'000ULL, 0})
                .ok());
    {
        auto before = store.value().find(profile);
        REQUIRE(before.ok());
        REQUIRE(before.value().has_value());
        REQUIRE(!before.value()->bundle_file_identity.has_value());
    }

    {
        WorkerJobFixture job;
        WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider, kDefaultWorkerControlTimeoutMs,
                                     &store.value());

        WorkerActionRequest request{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        const auto result = control.execute(request);
        REQUIRE(result.ok());
        REQUIRE(result.value().lifecycle.bundle.has_value());
    }

    // The identity the worker read is now durable, and it is the *worker's* identity
    // rather than something the service computed.
    auto after = store.value().find(profile);
    REQUIRE(after.ok());
    REQUIRE(after.value().has_value());
    REQUIRE(after.value()->bundle_file_identity.has_value());
    REQUIRE(after.value()->bundle_file_identity->volume_serial != 0);
    // And the amendment did not disturb the process identity, which is what makes the
    // worker attributable in the first place.
    REQUIRE_EQ(after.value()->worker_pid, 4242U);
    REQUIRE_EQ(after.value()->worker_process_creation_time, 133'700'000'000'000ULL);

    REQUIRE(wait_for_letter_release(letter));
    static_cast<void>(store.value().forget(profile));
}

// The AIM disk number behind a drive letter, when there is one.
//
// Used to assert that the *device* residue is real and not only the letter: a letter
// that survives is visible to a user, and the disk behind it is what keeps the letter
// from being reusable.
[[nodiscard]] std::optional<std::uint32_t> disk_number_for_letter(
    const wchar_t letter) {
    const std::wstring root = std::wstring(1, letter) + L":\\";
    wchar_t volume_name[MAX_PATH]{};
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name, MAX_PATH)
        == FALSE) {
        return std::nullopt;
    }
    // The disk number comes from the volume's *extents*, which is the only mapping
    // Windows exposes from a mount point to a physical disk without a device handle.
    HANDLE volume = CreateFileW(volume_name, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return std::nullopt;
    VOLUME_DISK_EXTENTS extents{};
    DWORD returned = 0;
    const BOOL queried = DeviceIoControl(
        volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents,
        sizeof(extents), &returned, nullptr);
    static_cast<void>(CloseHandle(volume));
    if (queried == FALSE || extents.NumberOfDiskExtents == 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(extents.Extents[0].DiskNumber);
}

TEST_CASE(m5_backend_a_killed_worker_leaves_no_process_mount_or_device) {
    // T1-03's kill acceptance, the half that needs a real device.  A worker is killed
    // **while it holds a real, provisioned volume**, and the machine must come back to
    // the state it started in: no process, no drive letter, no orphaned worker.
    //
    // This is the measurement the sweep exists for and the one nothing had made.  The
    // earlier kill case took a worker that had never opened a bundle -- it proved the
    // process half, which is why it could be written without a device; the residue it
    // is about, a mounted volume and an assigned letter, only exists once a worker has
    // mounted one.
    //
    // The order is what a crash does, not what a shutdown does: kill first, then drop
    // the session.  A worker asked to leave would run its ordered teardown and there
    // would be nothing to measure.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"kill-with-volume");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("5a5a5a5a-6b6b-7c7c-8d8d-9e9e9e9e9e9e");
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto operation = uuid("77777777-6666-5555-4444-333333333333");

    REQUIRE(wait_for_no_worker_process());
    {
        WorkerJobFixture job;
        WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);

        // A real create: the worker provisions a volume and assigns the letter, which
        // is what makes this the device half rather than the process half.
        WorkerActionRequest request{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        const auto created = control.execute(request);
        REQUIRE(created.ok());
        REQUIRE(created.value().lifecycle.bundle.has_value());

        // The letter is really assigned, so the state under test is real and not
        // assumed -- a case that killed a worker before it mounted anything would
        // measure nothing while looking like it measured this.
        REQUIRE(is_letter_assigned(letter));

        const auto status = control.require_runtime_observation(profile);
        REQUIRE(status.ok());
        REQUIRE(status.value().worker_alive);

        // Killed, not asked: `TerminateProcess` on the pid the provider's own registry
        // holds, which is the identity the sweep would use rather than something this
        // test invented.  Waiting for it settles that the kill took effect before the
        // session is dropped -- otherwise the teardown this case is measuring could be
        // the orderly one a closing pipe causes.
        const std::uint32_t worker_pid =
            *job.source().launched_process_ids().begin();
        REQUIRE(worker_pid != 0);
        HANDLE victim = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                    worker_pid);
        REQUIRE(victim != nullptr);
        REQUIRE(TerminateProcess(victim, 1) != FALSE);
        REQUIRE_EQ(WaitForSingleObject(victim, 30'000), WAIT_OBJECT_0);
        static_cast<void>(CloseHandle(victim));
    }

    // The session is dropped by leaving the scope above, which is what a service death
    // does -- and the kill happened *before* it, so the worker had no chance to run its
    // ordered teardown in response to a closing pipe.
    //
    // **The process half is clean and the device half is not**, and that is a measured
    // fact rather than an expectation.  Measured on the guest: after the kill,
    // `Get-Process myrd-m5-worker` is 0, `Get-Disk` still lists the AIM disk, and the
    // volume is still mounted with its letter assigned.
    //
    // That is exactly the state design section 5.5 exists for.  A `TerminateProcess`ed
    // worker cannot remove its own drive letter, dismount its volume or stop AIM -- the
    // reason the job does not set `KILL_ON_JOB_CLOSE` is that a worker which is *asked*
    // to leave does all three, and this one was not asked.  The residue is therefore the
    // reclaimer's job, not the worker's, and this case asserts the residue is real so
    // the reclaimer has a measurement to satisfy when it lands.
    //
    // Asserted as *present* rather than merely observed, so that implementing the
    // reclaimer turns this case red until it is also asserted as cleared.  A case that
    // only printed the residue would let the implementation land unnoticed.
    //
    // Only the drive letter is asserted here, and the device behind it is recorded as
    // *manually observed* in the evidence document instead.  Two attempts at asserting
    // the disk automatically both failed for reasons unrelated to what this measures --
    // `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` against the mount point returned nothing,
    // and `Get-Disk` is not callable from here -- and a device probe on this machine is
    // how the guest's storage stack gets degraded.  The letter is the user-visible
    // residue and the one that keeps the letter from being reusable is the disk; the
    // letter is what this case can hold reliably.
    REQUIRE(wait_for_no_worker_process());
    REQUIRE(is_letter_assigned(letter));
}

TEST_CASE(m5_backend_a_reclaim_dismantles_the_leftover_a_kill_created) {
    // **T1-03's acceptance, end to end.**  A worker is killed while it holds a real,
    // provisioned volume, and then a reclaim has to take that residue away: no process, no
    // drive letter, no mounted volume.
    //
    // The case above this one stops at *measuring* the residue and asserts it is present,
    // deliberately, so that implementing the reclaim would turn it red.  This is the case
    // that does the reclaiming, and it is separate because the residue has to survive the
    // scope that created it: a reclaim that reused the dead worker's session would be
    // dismantling through the very session the kill invalidated, which is not the situation
    // a crash leaves behind.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"reclaim-after-kill");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("6b6b6b6b-7c7c-8d8d-9e9e-afafafafafaf");
    const auto profile_id = uuid("11111111-2222-3333-4444-555555555555");
    const auto operation = uuid("66666666-7777-8888-9999-bbbbbbbbbbbb");

    // Phase one: a real create, then a kill, then the scope drops the dead session.  This
    // mirrors the case above so that the residue under test is produced the same way.
    {
        WorkerJobFixture job;
        WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);
        WorkerActionRequest request{
            operation,
            operation,
            profile_id,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        const auto created = control.execute(request);
        REQUIRE(created.ok());
        REQUIRE(is_letter_assigned(letter));

        const std::uint32_t worker_pid =
            *job.source().launched_process_ids().begin();
        HANDLE victim = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                    worker_pid);
        REQUIRE(victim != nullptr);
        REQUIRE(TerminateProcess(victim, 1) != FALSE);
        REQUIRE_EQ(WaitForSingleObject(victim, 30'000), WAIT_OBJECT_0);
        static_cast<void>(CloseHandle(victim));
    }
    REQUIRE(wait_for_no_worker_process());
    // The residue this case is about, asserted before the reclaim so a case that quietly
    // had nothing to reclaim cannot pass by doing nothing.
    REQUIRE(is_letter_assigned(letter));

    // Phase two: the reclaim, from a fresh control and a fresh job -- which is what a
    // service restart leaves behind.
    {
        WorkerJobFixture job;
        WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);

        // The durable record is the input, and its `ready` lifecycle is what makes the
        // translation ask for `adopt_existing` -- the mode the reclaim then overrides.
        // A create-shaped record would have asked for `provision_new` and the override is
        // precisely what stops this from provisioning a second volume.
        auto profile = profile_record(profile_id, image_id, bundle.root.wstring(),
                                      mount_target);
        WorkerActionRequest reclaim{
            operation,
            operation,
            profile_id,
            ManagementCommand::stop,
            ActionKind::reclaim_leftover,
            std::nullopt,
            profile,
            std::nullopt};
        const auto reclaimed = control.reclaim_leftover(reclaim);
        REQUIRE(reclaimed.ok());
    }

    // **The acceptance itself.**  The letter is gone and no worker outlived the reclaim --
    // which is the whole of what the residue was, in the two facts an operator can see.
    REQUIRE(wait_for_letter_release(letter));
    REQUIRE(!is_letter_assigned(letter));
    REQUIRE(wait_for_no_worker_process());
}

TEST_CASE(m5_backend_a_reopened_job_still_holds_its_workers) {
    // The design's core premise for the startup sweep, tested directly: a worker
    // launched into the named job is still *in* that job when a second handle
    // opens it by name.  Without this, "recover the previous service session's
    // workers" is unimplementable, because the sweep has no way to enumerate them.
    //
    // Two observations around a close-and-reopen, both while the worker is alive
    // and its session is held open.  The first is the control: if it does not see
    // the worker, the enumeration is broken and the second proves nothing.  This
    // ordering is the correction to an earlier version that closed the session
    // first -- the worker then exited cleanly, and "not in the job" was read from
    // a process that had simply finished.
    TempBundle bundle(L"job-reopen");
    WorkerJobFixture job;
    WindowsWorkerSessionProvider provider(worker_executable(), job.source());

    auto session = provider.launch(request_for(valid_profile(bundle)), context());
    REQUIRE(session.ok());
    const std::uint32_t worker_pid = session.value()->binding().process_id;
    const std::uint64_t worker_creation_time =
        session.value()->binding().process_creation_time;
    REQUIRE(worker_pid != 0);
    REQUIRE(worker_creation_time != 0);

    const auto lists_worker = [worker_pid](const WindowsWorkerProcessSource& source) {
        auto orphans = source.observe_orphans();
        REQUIRE(orphans.ok());
        return std::any_of(orphans.value().begin(), orphans.value().end(),
                           [worker_pid](const OrphanedProcess& orphan) {
                               return orphan.process_id == worker_pid;
                           });
    };

    // Control: a second handle to the same job sees the live worker.
    {
        auto observer = second_job_source();
        REQUIRE(lists_worker(*observer));
    }

    // Close that handle, open a fresh one, and ask again.  The worker has not been
    // touched: same session, same pipe, still running.
    {
        auto observer = second_job_source();
        REQUIRE(lists_worker(*observer));
    }

    // And the identity travels with it, so a sweep can tell this process from one
    // that merely inherited its pid.
    {
        auto observer = second_job_source();
        auto orphans = observer->observe_orphans();
        REQUIRE(orphans.ok());
        bool matched = false;
        for (const auto& orphan : orphans.value()) {
            if (orphan.process_id == worker_pid) {
                REQUIRE(orphan.process_creation_time.has_value());
                REQUIRE_EQ(*orphan.process_creation_time, worker_creation_time);
                matched = true;
            }
        }
        REQUIRE(matched);
    }

    REQUIRE(session.value()->shutdown(context(),
                                      uuid("99999999-8888-7777-6666-555555555555"))
                .ok());
    REQUIRE(job.source().wait_for_exit(worker_pid, 30'000).ok());
    REQUIRE(job.source().wait_for_exit(worker_pid, 30'000).value());
}

// The "closing the job does not kill its workers" case used to live here, and it
// is deliberately absent rather than passing vacuously.
//
// What it was trying to establish is real and is not in doubt: a job without
// `KILL_ON_JOB_CLOSE` does not end its members when its last handle closes, which
// is what lets an orphaned worker run its ordered teardown.  What cannot be
// established *here* is the measurement, for two reasons that were both measured:
//
//   * `AssignProcessToJobObject` fails with `ERROR_ACCESS_DENIED` (5) inside the
//     elevated SSH session, so every process in this harness is already nested in
//     a job this agent did not create -- and `IsProcessInJob(process, nullptr,
//     &r)` then reports true for reasons unrelated to the worker job.  The probe
//     cannot distinguish "still in our job" from "in the harness's job";
//   * closing the last handle to the job and reopening it by name does not
//     reliably observe the same object, so a reopened handle can be a *new*,
//     empty job that looks exactly like "the worker left".
//
// The property is therefore left to the VM acceptance in T1-03's part 3, where a
// real service is killed and the real orphan is measured, and it is recorded as a
// known limit in `docs/validation/m5-t1-03-worker-job.md` rather than dressed up
// as a passing case.

TEST_CASE(m5_backend_two_profiles_never_share_a_worker) {
    // Each profile gets its own process and its own pipe name, and the binding
    // each channel authenticated names that profile and no other.  A service
    // that could confuse the two would command one disk while recording
    // operations against another.
    TempBundle first(L"two-a");
    TempBundle second(L"two-b");
    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());

    auto left = provider.launch(
        request_for(profile_record(
            uuid("11111111-2222-3333-4444-555555555555"),
            uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"), first.root.wstring())),
        context());
    auto right = provider.launch(
        request_for(profile_record(
            uuid("12121212-3434-5656-7878-909090909090"),
            uuid("abababab-cdcd-efef-0101-232323232323"), second.root.wstring())),
        context());
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    REQUIRE(left.value()->binding().profile_id
            != right.value()->binding().profile_id);
    // Distinct processes, not one worker answering for both.
    REQUIRE(left.value()->binding().process_id
            != right.value()->binding().process_id);
}

TEST_CASE(m5_backend_refuses_a_launch_it_cannot_complete) {
    // A launch that cannot be completed must leave nothing behind.  An empty
    // executable path is refused before a process exists at all.
    TempBundle bundle(L"no-exe");
    WorkerJobFixture missing_job; WindowsWorkerSessionProvider missing(std::wstring{}, missing_job.source());
    const auto refused = missing.launch(request_for(valid_profile(bundle)), context());
    REQUIRE(!refused.ok());
}

// The mount-target rule used to be a case here, because the provider was where
// it lived.  It lives in the translation now -- one gate for both producers, and
// the profile path additionally has to name a drive-letter *mount kind*, which
// this case never checked -- so the case moved to `unit.m5.service_worker_control`
// rather than being kept as a second copy of one rule.

TEST_CASE(m5_backend_attach_finds_nothing_and_says_so) {
    // Adopting a worker this provider did not launch needs a registry of live
    // workers, which belongs to the composition.  A successful `nullopt` means
    // "none running", which the adapter turns into an explicit refusal -- the
    // honest answer until that registry exists, and different from claiming a
    // worker was found.
    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    const auto attached =
        provider.attach(uuid("11111111-2222-3333-4444-555555555555"), context());
    REQUIRE(attached.ok());
    REQUIRE(attached.value() == nullptr);
}

TEST_CASE(m5_backend_adapter_reaches_the_worker_it_launched) {
    // The adapter's `create_runtime_and_worker` maps onto a launch and then a
    // `start`.  `start` cannot succeed here -- an empty directory is not a
    // manifest bundle, so recovery refuses it before touching a volume -- but the
    // *failure* is what this case is about: it must be the runtime's failure,
    // arriving over an authenticated channel, and not `execution_not_bound`,
    // which is what a launch that never happened would produce.  The two are
    // indistinguishable from a bare `!ok()`.
    //
    // The profile is `ready`, so the launch adopts.  A `provisioning` one would
    // run the create lifecycle instead, which provisions a real volume and
    // claims a real drive letter on this machine -- a side effect this case has
    // no business causing, and one it would have caused silently.
    TempBundle bundle(L"adapter");
    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    ServiceWorkerControl control(provider);
    const auto profile = valid_profile(bundle);
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    WorkerActionRequest request{
        operation,
        operation,
        profile.profile.profile_uuid,
        ManagementCommand::start,
        ActionKind::create_runtime_and_worker,
        std::nullopt,
        profile,
        std::nullopt};
    const auto result = control.execute(request);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code
            != static_cast<std::uint32_t>(
                ServiceWorkerControlError::execution_not_bound));
}

TEST_CASE(m5_backend_creates_a_bundle_and_mounts_it_end_to_end) {
    // ??.25's case, brought back now that the operation it needed exists. Every
    // step below has been run before in isolation against a fake, and none of it
    // has ever run in order against the real thing: a real worker process, a real
    // generation-zero image, a real AIM proxy disk, a real provisioned and
    // formatted volume, a real drive letter.
    //
    // It is also the first case in this tree to touch any of that, so its side
    // effects are stated rather than discovered: it claims a letter it has
    // verified is free, and every exit path releases it -- `cleanup` removes the
    // drive letter and dismounts, and a worker whose channel is closed without
    // one runs the same teardown as it exits.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"e2e");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("1f1f1f1f-2e2e-3d3d-4c4c-5b5b5b5b5b5b");

    auto launch_request =
        make_launch_request(create_intent(image_id, bundle, mount_target));
    REQUIRE(launch_request.ok());
    // A create asks for a new volume, which is the whole of what unit B added:
    // the same launch used to recover and fail on an empty bundle.
    REQUIRE(launch_request.value().volume_mode
            == m5::WorkerVolumeMode::provision_new);

    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    auto launched = provider.launch(launch_request.value(), context());
    REQUIRE(launched.ok());
    auto& session = *launched.value();
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    // The command ??.26 designed: it creates the bundle, provisions the volume,
    // and answers with the identity the service records as its registration.
    // A failure here exits the process at the assertion, so the reason is not
    // visible from this run -- the framework has no message form.  Locating one
    // means a temporary `fprintf` of `created.error()`, which is exactly how the
    // two defects this case found on its first run were located: a zero RAM
    // budget and a missing provisioning resource.
    const auto created = session.create_bundle(provisioning_context(), operation);
    REQUIRE(created.ok());
    REQUIRE(created.value().bundle.has_value());
    REQUIRE(created.value().bundle->complete());
    // The image is the one the launch named -- not a random one -- because the
    // service derived it before the bundle existed and validates the recorded
    // evidence against it.
    REQUIRE(created.value().bundle->image_id == image_bytes(image_id));
    REQUIRE_EQ(created.value().bundle->virtual_bytes, kCapacityBytes);
    REQUIRE_EQ(created.value().bundle->logical_sector_size, 512u);
    REQUIRE_EQ(created.value().bundle->physical_sector_size, 4096u);

    // The runtime the create brought up is observable and holds a volume with a
    // complete persistent identity. This is what the plan's provisioning-family
    // steps read instead of performing their own.
    const auto observed = session.require_runtime_observation(context());
    REQUIRE(observed.ok());
    REQUIRE(observed.value().worker_alive);
    REQUIRE(observed.value().volume.has_value());
    REQUIRE(observed.value().volume->complete());
    REQUIRE(observed.value().image_id == image_bytes(image_id));

    // And the volume is really there: the letter this case claimed is assigned.
    REQUIRE(is_letter_assigned(letter));

    const auto cleaned = session.cleanup(context(), operation);
    REQUIRE(cleaned.ok());
    // Released, which is the half of the side effect that has to be stated.
    REQUIRE(!is_letter_assigned(letter));
}

TEST_CASE(m5_backend_adapter_registers_the_bundle_it_creates) {
    // The adapter's own path to a registration, which nothing exercised until
    // now: the case above drives `ServiceWorkerSession` directly, and the unit
    // suite drives `ServiceWorkerControl` with a provider that can never launch.
    // Between them, `register_bundle`'s success branch -- mapping the worker's
    // evidence, deciding whether to hold the session, converting the image id --
    // had never run.  It is the adapter the service actually calls.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"adapter-create");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("2e2e2e2e-3d3d-4c4c-5b5b-5b5b5b5b5b5b");
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);

        WorkerActionRequest request{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        const auto result = control.execute(request);
        REQUIRE(result.ok());
        REQUIRE(result.value().lifecycle.bundle.has_value());
        const auto& evidence = result.value().lifecycle.bundle.value();

        // Every field is either the worker's authenticated reading or a product
        // constant the service already holds.  The geometry and the uuid come
        // from the inspection that verified the image; the file identity names
        // the file object, not the path.
        REQUIRE(evidence.source == BundleEvidenceSource::created_and_verified);
        REQUIRE(evidence.image_id == image_id);
        REQUIRE_EQ(evidence.logical_capacity_bytes, kCapacityBytes);
        REQUIRE_EQ(evidence.logical_sector_bytes, kProductLogicalSectorBytes);
        REQUIRE_EQ(evidence.physical_sector_bytes, kProductPhysicalSectorBytes);
        REQUIRE_EQ(evidence.payload_block_bytes, kProductPayloadBlockBytes);
        REQUIRE(evidence.manifest_name == kProductManifestName);
        REQUIRE(evidence.file_identity.volume_serial != 0);

        // The create's session is held, so the plan's later
        // `create_runtime_and_worker` finds it instead of launching a second
        // worker for one bundle -- and the steps after that read this runtime.
        const auto observed = control.require_runtime_observation(profile);
        REQUIRE(observed.ok());
        REQUIRE(observed.value().worker_alive);
        REQUIRE(observed.value().committed_generation >= 1);

        // `record_volume_identity` is a second adapter path that had never run.
        // It reads the volume's persistent GPT identities out of the live
        // worker's status and turns them into the durable registration, so a
        // complete identity here is proof the worker really provisioned and
        // adopted a volume rather than merely answering a question.
        WorkerActionRequest record{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::record_volume_identity,
            std::nullopt,
            std::nullopt,
            std::nullopt};
        const auto recorded = control.execute(record);
        REQUIRE(recorded.ok());
        REQUIRE(recorded.value().lifecycle.volume.has_value());

        REQUIRE(is_letter_assigned(letter));
    }
    // Past this scope the adapter has dropped the session, which closes the
    // channel.  That is the path a service crash takes, and it has to release
    // the mount on its own: the worker notices the closed pipe, runs its ordered
    // teardown, and removes the drive letter.
    REQUIRE(wait_for_letter_release(letter));
}

// A failed action names itself.
//
// The framework's REQUIRE exits the process at the first failure and prints only
// a line number, so a bare `REQUIRE(created.ok())` says nothing about *why* --
// and these cases depend on real devices, real provisioning and real timing, so
// "why" is the whole value of the run.  Kept rather than removed as a probe:
// three distinct intermittent failures were found in one session, every one of
// them unnamed until this existed (manifest 1.34).
void require_created(const Result<WorkerActionResult>& created) {
    if (!created.ok()) {
        std::fprintf(stderr,
                     "myrd-test: create failed: op=%ls msg=%ls native=%u code=%u",
                     created.error().operation.c_str(),
                     created.error().message.c_str(),
                     created.error().native_code, created.error().code);
        std::fputc(10, stderr);
    }
    REQUIRE(created.ok());
}

// The same, for the actions that are not creates.
void require_action(const Result<WorkerActionResult>& result, const char* what) {
    if (!result.ok()) {
        std::fprintf(stderr, "myrd-test: %s failed: op=%ls msg=%ls native=%u code=%u",
                     what, result.error().operation.c_str(),
                     result.error().message.c_str(),
                     result.error().native_code, result.error().code);
        std::fputc(10, stderr);
    }
    REQUIRE(result.ok());
}

// The profile a checkpoint cut is authorized against.  It is built from the
// *same* image id the create used, because the cut checks the live runtime's
// image against the durable record's -- a worker cutting a snapshot for another
// profile's image is exactly what that check refuses.
profile::ProfileRecord checkpoint_profile(const product::Uuid& profile_id,
                                          const product::Uuid& image_id,
                                          const std::wstring& bundle_path,
                                          const std::wstring& mount_target) {
    auto record = profile_record(profile_id, image_id, bundle_path, mount_target);
    record.profile.lifecycle = profile::ProfileLifecycle::ready;
    return record;
}

TEST_CASE(m5_backend_checkpoint_crosses_the_boundary_and_commits) {
    // The two-phase checkpoint boundary against a real worker.  This is the
    // largest block of the adapter that nothing had ever run:
    // `flush_exact_volume`, `cut_candidate`, `checkpoint_across_boundary` and
    // `abort_held_candidate` all go through here for the first time, and so does
    // the P4/P5 shape they carry.  The model suite drives the *decisions* with a
    // fake; only this can show that they survive a real snapshot and a real
    // commit.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"boundary");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("3f3f3f3f-4e4e-5d5d-6c6c-7b7b7b7b7b7b");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);

        WorkerActionRequest create{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        require_created(control.execute(create));

        auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                          mount_target);
        WorkerActionRequest flush{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::flush_exact_volume,
            std::nullopt,
            durable,
            std::nullopt};
        // The flush cuts the candidate a later commit publishes.  It reaches the
        // worker's two-phase prepare, which had never run outside the model.
        REQUIRE(control.execute(flush).ok());

        // The boundary is where the service decides, with the candidate already
        // frozen in the worker.  Here it continues.
        WorkerActionRequest boundary_request{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::checkpoint,
            std::nullopt,
            durable,
            std::nullopt};
        auto committed = control.execute_checkpoint(
            boundary_request,
            [&](const CheckpointCommitBoundaryEvidence& evidence) {
                // The evidence is the live worker's own report -- the frozen
                // candidate's slot and digest, and the operation this session
                // asked for.  A boundary handed a different identity would be
                // deciding about a checkpoint nobody cut.
                REQUIRE(evidence.product_operation_id == operation);
                REQUIRE(evidence.profile_id == profile);
                REQUIRE(evidence.candidate_generation
                        == evidence.prior_generation + 1);
                return Result<CheckpointCommitBoundaryDecision>::success(
                    CheckpointCommitBoundaryDecision{true, 0, 0});
            });
        REQUIRE(committed.ok());
        REQUIRE(committed.value().checkpoint.has_value());
        const auto& checkpoint = committed.value().checkpoint.value();
        REQUIRE(checkpoint.disposition == CheckpointCommitDisposition::committed);
        REQUIRE_EQ(checkpoint.generation,
                   checkpoint.correlation.candidate_generation);
        // A landed commit is what the provisioning start path acknowledges.
        REQUIRE(committed.value().lifecycle.first_checkpoint_acknowledged);

        // And the session is still usable afterwards: a landed commit releases
        // the candidate, so a later flush can cut a fresh one.  Asserted here
        // because the withdrawal path does *not* have this property -- see the
        // case below and manifest ??.33.
        REQUIRE(control.execute(flush).ok());
    }
    REQUIRE(wait_for_letter_release(letter));
}

TEST_CASE(m5_backend_checkpoint_boundary_withdrawal_publishes_nothing) {
    // The other half of the boundary: the service withdraws the commit after the
    // candidate is frozen.  Nothing may be published, and the candidate must be
    // resolved rather than left outstanding -- an abort the worker confirmed is
    // what makes "not committed" a fact instead of an assumption.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"boundary-withdraw");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("4f4f4f4f-5e5e-6d6d-7c7c-8b8b8b8b8b8b");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
        ServiceWorkerControl control(provider);

        WorkerActionRequest create{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        require_created(control.execute(create));

        auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                          mount_target);
        WorkerActionRequest flush{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::flush_exact_volume,
            std::nullopt,
            durable,
            std::nullopt};
        REQUIRE(control.execute(flush).ok());

        WorkerActionRequest boundary_request{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::checkpoint,
            std::nullopt,
            durable,
            std::nullopt};
        auto withdrawn = control.execute_checkpoint(
            boundary_request,
            [](const CheckpointCommitBoundaryEvidence&) {
                return Result<CheckpointCommitBoundaryDecision>::success(
                    CheckpointCommitBoundaryDecision{false, 0, 0});
            });
        REQUIRE(withdrawn.ok());
        REQUIRE(withdrawn.value().checkpoint.has_value());
        // Reported as not committed, at the prior generation -- because nothing
        // was published, and the generation is the runtime's own number rather
        // than one derived from what the service expected.
        REQUIRE(withdrawn.value().checkpoint->disposition
                == CheckpointCommitDisposition::not_committed);
        REQUIRE_EQ(withdrawn.value().checkpoint->generation,
                   withdrawn.value().checkpoint->correlation.prior_generation);
        REQUIRE(!withdrawn.value().lifecycle.first_checkpoint_acknowledged);

        // Deliberately *not* asserted here: that a later flush can cut a fresh
        // candidate.  It cannot.  A withdrawn candidate leaves the runtime
        // unable to prepare another one -- `image_path.open_target` reports the
        // bundle file identity differs from the expected one -- while a
        // *committed* candidate leaves the session perfectly usable (asserted in
        // the case above, which is what makes the two paths distinguishable).
        // The finding and its minimal reproduction are manifest ??.33, and it is
        // not fixed here: the expectation lives inside M3's abort handling, and
        // the transfer manifest's mandatory gate forbids M5 from changing M3's
        // bundle, journal or manifest formats.
    }
    REQUIRE(wait_for_letter_release(letter));
}

TEST_CASE(m5_backend_stop_walks_quiesce_flush_commit_remove) {
    // The whole stop plan against a real worker, which is the other half of
    // ??.22's unexecuted list: `quiesce_mutations`, `remove_mount` and
    // `confirm_removal` all run here for the first time, in the order the plan
    // puts them in.
    //
    // The order is the point, not an incidental.  Quiescing witnesses the
    // generation a removal may be authorized at; the checkpoint commits and is
    // what *moves* that witness; the removal must happen at the generation the
    // service proved, and the two steps after it are no-ops *proven* by that
    // removal rather than assumed from it.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"stop");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("5a5a5a5a-6b6b-7c7c-8d8d-9e9e9e9e9e9e");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    ServiceWorkerControl control(provider);
    auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                      mount_target);
    auto stop_request = [&](const ActionKind action) {
        return WorkerActionRequest{operation, operation, profile,
                                   ManagementCommand::stop, action,
                                   std::nullopt, durable, std::nullopt};
    };

    WorkerActionRequest create{
        operation,
        operation,
        profile,
        ManagementCommand::create_and_start,
        ActionKind::create_and_verify_bundle,
        create_intent(image_id, bundle, mount_target),
        std::nullopt,
        std::nullopt};
    const auto created = control.execute(create);
    if (!created.ok()) {
        std::fprintf(stderr, "PROBE-STOP op=%ls msg=%ls native=%u code=%u\n",
                     created.error().operation.c_str(),
                     created.error().message.c_str(),
                     created.error().native_code, created.error().code);
    }
    REQUIRE(created.ok());
    REQUIRE(is_letter_assigned(letter));

    // Quiesce proves the live runtime is the one the durable record describes
    // and witnesses its generation.  It takes no write freeze -- this adapter
    // holds none -- so what protects the write stream is downstream and real.
    require_action(control.execute(stop_request(ActionKind::quiesce_mutations)), "stop step");

    // The write freeze.  This is the step the case below
    // (`m5_backend_holding_the_volume_lock_refuses_a_second_lock`) exists to
    // justify, and leaving it out here was how the first version of this case
    // certified a sequence production does not run.
    require_action(control.execute(stop_request(ActionKind::freeze_writes)), "stop step");

    // The flush cuts the candidate, the boundary continues, the commit lands and
    // advances the witness the removal will be checked against.
    require_action(control.execute(stop_request(ActionKind::flush_exact_volume)), "stop step");
    auto committed = control.execute_checkpoint(
        stop_request(ActionKind::checkpoint),
        [](const CheckpointCommitBoundaryEvidence&) {
            return Result<CheckpointCommitBoundaryDecision>::success(
                CheckpointCommitBoundaryDecision{true, 0, 0});
        });
    REQUIRE(committed.ok());
    REQUIRE(committed.value().checkpoint.has_value());
    REQUIRE(committed.value().checkpoint->disposition
            == CheckpointCommitDisposition::committed);

    // Production does not stop here either: ServiceCore confirms the epoch after
    // the commit, and the runtime is only clean once it has.  Leaving it out was
    // the first version of this case, and the intermittency it produced is
    // recorded in ??.34.
    REQUIRE(control
                .publish_checkpoint_epoch(
                    stop_request(ActionKind::checkpoint),
                    committed.value().checkpoint->correlation,
                    [](const CheckpointEpochObservation&) {
                        return Result<void>::success();
                    })
                .ok());

    // The removal barrier.  It must remove and stop in one authenticated call --
    // a reply claiming one without the other was refused on the wire -- and it
    // releases the mount as part of that.
    require_action(control.execute(stop_request(ActionKind::remove_mount)), "stop step");
    REQUIRE(wait_for_letter_release(letter));

    // The two steps after it describe the same teardown the barrier performed,
    // and may only be reported as done on a removal that was *proven*.  Before
    // the removal they would fail; here they succeed, which is the proof being
    // taken rather than assumed.
    require_action(control.execute(stop_request(ActionKind::remove_exact_device)), "stop step");
    require_action(
        control.execute(stop_request(ActionKind::stop_runtime_and_release_lock)),
        "stop step");
}

TEST_CASE(m5_backend_confirmed_removal_without_a_removal_is_refused) {
    // The mirror of the case above, and the reason it is worth having: the two
    // post-removal steps are the ones that would let a service record a disk as
    // gone without anything having removed it.  With no removal proven for this
    // operation they fail closed, and the worker is never asked.
    TempBundle bundle(L"removal-unproven");
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("6a6a6a6a-7b7b-8c8c-9d9d-aeaeaeaeaeae");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    ServiceWorkerControl control(provider);
    auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                      L"R:");
    for (const auto action :
         {ActionKind::remove_exact_device,
          ActionKind::stop_runtime_and_release_lock}) {
        WorkerActionRequest request{operation, operation, profile,
                                    ManagementCommand::stop, action,
                                    std::nullopt, durable, std::nullopt};
        const auto refused = control.execute(request);
        REQUIRE(!refused.ok());
        REQUIRE_EQ(static_cast<std::uint32_t>(refused.error().code),
                   static_cast<std::uint32_t>(
                       ServiceWorkerControlError::removal_unproven));
    }
}

// The single-disk stop plan as one call, returning the result of the removal
// barrier -- the step a second live profile breaks.
//
// Factored out rather than copied because the case below walks the identical
// plan for two profiles, and a second copy is a second thing that can drift
// from what production does without anything noticing.
Result<WorkerActionResult> run_stop_plan(ServiceWorkerControl& control,
                                        const product::Uuid& operation,
                                        const product::Uuid& profile,
                                        const profile::ProfileRecord& durable) {
    auto request = [&](const ActionKind action) {
        return WorkerActionRequest{operation, operation, profile,
                                   ManagementCommand::stop, action,
                                   std::nullopt, durable, std::nullopt};
    };
    require_action(control.execute(request(ActionKind::quiesce_mutations)),
                   "stop plan");
    // The write freeze, in the position the plan puts it.  Omitting it here
    // would make this helper certify a stop sequence production never runs --
    // and the removal below depends on it: an unfrozen commit is not the last
    // word on the volume, which is what the 25% `runtime_dirty` refusal was.
    require_action(control.execute(request(ActionKind::freeze_writes)),
                   "stop plan");
    require_action(control.execute(request(ActionKind::flush_exact_volume)),
                   "stop plan");
    auto committed = control.execute_checkpoint(
        request(ActionKind::checkpoint),
        [](const CheckpointCommitBoundaryEvidence&) {
            return Result<CheckpointCommitBoundaryDecision>::success(
                CheckpointCommitBoundaryDecision{true, 0, 0});
        });
    REQUIRE(committed.ok());
    REQUIRE(committed.value().checkpoint.has_value());
    REQUIRE(committed.value().checkpoint->disposition
            == CheckpointCommitDisposition::committed);
    // The epoch confirmation is not optional, and its absence is ??.34(b): a
    // runtime is dirty until the commit it made has been confirmed, and a dirty
    // runtime is refused its own removal.
    REQUIRE(control
                .publish_checkpoint_epoch(
                    request(ActionKind::checkpoint),
                    committed.value().checkpoint->correlation,
                    [](const CheckpointEpochObservation&) {
                        return Result<void>::success();
                    })
                .ok());
    auto removed = control.execute(request(ActionKind::remove_mount));
    if (removed.ok()) {
        // Only on a removal that was proven, exactly as production does it.
        require_action(
            control.execute(request(ActionKind::remove_exact_device)),
            "stop plan");
        require_action(
            control.execute(request(ActionKind::stop_runtime_and_release_lock)),
            "stop plan");
    }
    return removed;
}

TEST_CASE(m5_backend_stopping_one_profile_does_not_require_the_other_gone) {
    // Two profiles alive at once, and the *first* one stopped while the second
    // is still mounted.  This is the regression test for ??.38.
    //
    // It failed before the fix, on `stop A`, with
    // `M3AimRuntime::remove_owned_device` / "PhysicalDrive inventory did not
    // return to baseline" (`WAIT_TIMEOUT`).  Why it could not have passed is
    // worth keeping beside it: M3's device removal waited for the machine-wide
    // physical drive inventory to return to the set it saw when that runtime
    // started, and a second profile's disk appeared after this one took its
    // baseline and cannot un-appear.
    //
    // Nothing here is a race, which is what makes it decisive: create A, create
    // B, stop A is sequential, and it was enough.  That is also why a harness
    // barrier could never have been the answer -- a barrier serialises *cases*,
    // and these two disks are both supposed to be alive.
    //
    // The two stops below were chosen to cover both ways the old predicate
    // failed: A is stopped while a disk that appeared after its baseline is up,
    // and B is stopped after A's disk is gone -- a disk B's own baseline still
    // contained, so the inventory could not return to it either.
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle_a(L"isolate-a");
    TempBundle bundle_b(L"isolate-b");
    const auto letters = free_drive_letters(2);
    const std::wstring mount_a = std::wstring(1, letters[0]) + L":";
    const std::wstring mount_b = std::wstring(1, letters[1]) + L":";
    const auto profile_a = uuid("11111111-2222-3333-4444-555555555555");
    const auto profile_b = uuid("12121212-3434-5656-7878-909090909090");
    const auto image_a = uuid("7a7a7a7a-8b8b-9c9c-adad-bebebebebebe");
    const auto image_b = uuid("8b8b8b8b-9c9c-adad-bebe-cfcfcfcfcfcf");
    // Distinct product operations: the removal proof is per operation, so one
    // profile's proven removal must not be readable as the other's.
    const auto operation_a = uuid("99999999-8888-7777-6666-555555555551");
    const auto operation_b = uuid("99999999-8888-7777-6666-555555555552");

    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    ServiceWorkerControl control(provider);
    const auto durable_a =
        checkpoint_profile(profile_a, image_a, bundle_a.root.wstring(), mount_a);
    const auto durable_b =
        checkpoint_profile(profile_b, image_b, bundle_b.root.wstring(), mount_b);

    auto create = [&](const product::Uuid& operation,
                      const product::Uuid& profile,
                      const product::Uuid& image,
                      const TempBundle& bundle,
                      const std::wstring& mount_target) {
        WorkerActionRequest request{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent_for(profile, image, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        return control.execute(request);
    };
    require_created(create(operation_a, profile_a, image_a, bundle_a, mount_a));
    require_created(create(operation_b, profile_b, image_b, bundle_b, mount_b));
    REQUIRE(is_letter_assigned(letters[0]));
    REQUIRE(is_letter_assigned(letters[1]));

    // The stop that cannot succeed today.  Every step before the removal is the
    // same plan the single-disk case walks -- quiesce, flush, commit, confirm
    // the epoch -- so it arrives at the device teardown with nothing else
    // outstanding, and that is where it fails.
    require_action(run_stop_plan(control, operation_a, profile_a, durable_a),
                   "stop A");
    REQUIRE(!is_letter_assigned(letters[0]));
    // The other half of the property, and the reason this is not merely "does
    // the first stop work": stopping one disk must leave the other mounted.  A
    // service that took both down to satisfy the first would otherwise pass.
    REQUIRE(is_letter_assigned(letters[1]));

    require_action(run_stop_plan(control, operation_b, profile_b, durable_b),
                   "stop B");
    REQUIRE(!is_letter_assigned(letters[1]));
}

// ---------------------------------------------------------------------------
// The write freeze, and the one thing about it that had to be learned from a
// device rather than from reasoning.
//
// `FSCTL_LOCK_VOLUME` is not re-entrant through this adapter, and the reason is
// one line of `Win32VolumeAdapter::lock`: it opens the volume with *no* sharing
// mode, so while this process holds the lock, a second acquisition cannot open
// the volume at all and comes back as `busy` -- the adapter's documented answer
// for "someone else holds it".  It cannot tell "someone else" from "this same
// runtime", and it must not try.
//
// That is a rule about the *implementation*, and the first version of T1-11
// broke against it twice, both times on a real volume:
//
//   * `flush_exact_volume` after the freeze failed with
//     `op=flush msg=opening volume failed code=21001` in 20 of 20 runs, because
//     a flush that opened its own handle could not open the frozen volume;
//   * the same applies to the dismantle, which is why
//     `DiskRuntimeOperations::dismount_volume` takes the retained lock as an
//     argument instead of acquiring one.
//
// So the invariant is *structural*, not procedural: once a runtime has frozen a
// volume, every later step that touches the volume runs through the handle that
// holds the freeze, and the type signature is what makes re-acquiring it
// unrepresentable.  The behaviour that matters -- a stop that freezes, flushes,
// commits and then dismantles cleanly, 20 times out of 20 -- is measured by
// `m5_backend_stop_walks_quiesce_flush_commit_remove` above, whose freeze step
// cannot pass unless the real lock is really held.
//
// It is also why `Win32VolumeAdapter::lock` retries inside its budget: building
// this case showed that a volume whose last handle was closed a moment ago
// refuses the exclusive open the teardown needs, which read as a deterministic
// "volume is busy" until the worker was instrumented and the retry was measured
// to succeed.
// ---------------------------------------------------------------------------

// What one lock attempt did, out of the probe thread.
struct LockProbeResult final {
    bool ran{};
    bool ok{};
    bool acquired{};
    bool busy{};
    bool held{};
    std::uint32_t error_code{};
    std::uint32_t native_code{};
    std::wstring message;
};

// The adopted volume, restated in the engine's own type from what the volume
// itself reports.
//
// The device path is resolved through the mount point rather than pasted from the
// worker's observation: the worker reports the *GPT identity* as canonical text
// (`{guid}` with no prefix) while `ProductVolume` carries the device path
// `\\?\Volume{guid}`, and building one from the other by concatenation would be a
// second encoding of the same fact.
ProductVolume read_adopted_volume(const wchar_t letter,
                                  const std::wstring& owner_token) {
    const std::wstring mount{letter, L':', L'\\', L'\0'};
    std::array<wchar_t, MAX_PATH> device_path{};
    if (GetVolumeNameForVolumeMountPointW(mount.c_str(), device_path.data(),
                                          static_cast<DWORD>(device_path.size()))
        != TRUE) {
        std::fprintf(stderr, "PROBE-VOLUME: no device path for %c: err=%lu\n",
                     letter, static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
        return {};
    }
    ProductVolume volume{};
    volume.owner_token = owner_token;
    volume.volume_guid = device_path.data();
    volume.drive_letter = letter;
    volume.mounted = true;
    volume.operation_owned = true;

    std::wstring device = volume.volume_guid;
    while (!device.empty() && device.back() == L'\\') device.pop_back();
    HANDLE handle = CreateFileW(device.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "PROBE-VOLUME: opening for extents failed err=%lu\n",
                     static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
        return {};
    }
    std::array<std::byte, 64 * 1024> bytes{};
    DWORD returned = 0;
    const BOOL queried = DeviceIoControl(
        handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, bytes.data(),
        static_cast<DWORD>(bytes.size()), &returned, nullptr);
    const DWORD query_error = GetLastError();
    CloseHandle(handle);
    const auto header = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (queried != TRUE || returned < header + sizeof(DISK_EXTENT)) {
        std::fprintf(stderr, "PROBE-VOLUME: extents query failed err=%lu\n",
                     static_cast<unsigned long>(query_error));
        std::fflush(stderr);
        return {};
    }
    const auto* extents =
        reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
    for (DWORD index = 0; index != extents->NumberOfDiskExtents; ++index) {
        const auto& extent = extents->Extents[index];
        volume.extents.push_back(
            {extent.DiskNumber,
             static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
             static_cast<std::uint64_t>(extent.ExtentLength.QuadPart)});
    }
    return volume;
}

// Runs one lock attempt on its own thread so the case can prove it *returned*:
// the failure being ruled out is a hang, and a hung test reports a timeout
// instead of a reason.
LockProbeResult probe_lock(Win32VolumeAdapter& adapter,
                           const ProductVolume& volume,
                           std::optional<LockedVolume>& out) {
    LockProbeResult result{};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        auto attempt = adapter.lock(volume, 30000);
        result.ran = true;
        if (attempt.ok()) {
            result.ok = true;
            result.acquired = attempt.value().acquired;
            result.busy = attempt.value().busy;
            if (attempt.value().locked.has_value()) {
                out = std::move(*attempt.value().locked);
                result.held = true;
            }
        } else {
            result.error_code = attempt.error().code;
            result.native_code = attempt.error().native_code;
            result.message = attempt.error().message;
        }
        finished.store(true);
    });
    for (std::uint32_t waited = 0; waited < 60000; waited += 50) {
        if (finished.load()) {
            worker.join();
            return result;
        }
        Sleep(50);
    }
    // Wedged inside a device call with no cancellation.  Detached rather than
    // joined: joining would hang the suite, which is the one outcome this guard
    // exists to prevent.  `ran` stays false so the case fails loudly.
    worker.detach();
    return result;
}

TEST_CASE(m5_backend_holding_the_volume_lock_refuses_a_second_lock) {
    REQUIRE(wait_for_no_worker_process());
    TempBundle bundle(L"reentrant-lock");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("7c7c7c7c-8d8d-9e9e-afaf-b0b0b0b0b0b0");

    WorkerJobFixture job; WindowsWorkerSessionProvider provider(worker_executable(), job.source());
    auto launched = provider.launch(
        make_launch_request(create_intent(image_id, bundle, mount_target))
            .value(),
        context());
    REQUIRE(launched.ok());
    auto& session = *launched.value();
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");
    const auto created = session.create_bundle(provisioning_context(), operation);
    REQUIRE(created.ok());
    REQUIRE(is_letter_assigned(letter));

    const ProductVolume volume = read_adopted_volume(letter, L"probe");
    REQUIRE(!volume.volume_guid.empty());
    REQUIRE(!volume.extents.empty());

    Win32VolumeAdapter adapter;

    // The first lock, standing in for the write freeze a runtime holds.
    std::optional<LockedVolume> held;
    const auto first = probe_lock(adapter, volume, held);
    const bool first_held =
        first.ran && first.ok && first.acquired && first.held;

    // The attempt that answers the question, while the first is still held.
    std::optional<LockedVolume> second;
    const auto again = probe_lock(adapter, volume, second);
    second.reset();

    // The holder is undisturbed by a refused attempt.
    const bool holder_survived = held.has_value() && held->valid();
    held.reset();

    // And the volume is winnable again once given back -- without this the case
    // could pass because the volume had been left unusable.
    std::optional<LockedVolume> third;
    const auto recovered = probe_lock(adapter, volume, third);
    const bool third_held =
        recovered.ran && recovered.ok && recovered.acquired && recovered.held;
    third.reset();

    REQUIRE(first_held);
    REQUIRE(again.ran);
    // The measured rule: the second acquisition does not happen.  What was
    // measured on a real AIM-backed NTFS volume is narrower than the guess this
    // case was written from -- the refusal is not `busy`: `CreateFileW` on the
    // device path with no sharing mode, while this process already holds the
    // lock, fails with `ERROR_NOT_READY` (21), and `lock` only maps
    // sharing/lock/busy violations to `busy`.  Either way no lock is handed out,
    // which is the fact the teardown is built on.  The native code is *recorded*
    // rather than pinned: asserting it would turn a platform observation into a
    // contract this project does not control.
    if (again.acquired) {
        std::fprintf(stderr,
                     "PROBE-REENTRANT: a SECOND lock was granted while the first "
                     "was held\n");
        std::fflush(stderr);
    }
    REQUIRE(!again.acquired);
    REQUIRE(!again.held);
    REQUIRE(holder_survived);
    REQUIRE(third_held);

    // Every probe handle is closed by here, so the worker can still tear its own
    // volume down.  This is the step that failed 25-50% of the time before the
    // lock retry was added.
    const auto cleaned = session.cleanup(context(), operation);
    if (!cleaned.ok()) {
        std::fprintf(stderr,
                     "PROBE-REENTRANT cleanup failed: op=%ls msg=%ls native=%u "
                     "code=%u\n",
                     cleaned.error().operation.c_str(),
                     cleaned.error().message.c_str(),
                     cleaned.error().native_code, cleaned.error().code);
        std::fflush(stderr);
    }
    REQUIRE(cleaned.ok());
    REQUIRE(wait_for_letter_release(letter));
}
