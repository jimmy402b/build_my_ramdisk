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
#include "src/management/windows_worker_sessions.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
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
// the two-disk case (搂1.38): that case creates *both* of its workers inside
// itself, so a barrier taken at case start cannot affect it and its
// failing-before / passing-after evidence stands on the M3 fix alone.
//
// It was deleted in 搂1.38 on the argument that it made "fixed" self-certifying.
// That argument does not hold for the defect 搂1.38 found, and the deletion was
// measured afterwards to reintroduce this inter-case class: with the same M3
// code, the suite failed 2 of 5 isolated runs without this barrier and 0 of 4
// with it (搂1.39).
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

} // namespace

TEST_CASE(m5_backend_launches_and_authenticates_a_real_worker) {
    // The whole point of the increment: a real process starts, reads the spec
    // from the inherited channel, connects to a pipe whose ACL admits only the
    // service and itself, and proves it did by echoing the startup secret.  None
    // of that can be faked without testing a different object than the one that
    // runs.
    TempBundle bundle(L"launch");
    WindowsWorkerSessionProvider provider(worker_executable());
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

TEST_CASE(m5_backend_two_profiles_never_share_a_worker) {
    // Each profile gets its own process and its own pipe name, and the binding
    // each channel authenticated names that profile and no other.  A service
    // that could confuse the two would command one disk while recording
    // operations against another.
    TempBundle first(L"two-a");
    TempBundle second(L"two-b");
    WindowsWorkerSessionProvider provider(worker_executable());

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
    WindowsWorkerSessionProvider missing(std::wstring{});
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
    WindowsWorkerSessionProvider provider(worker_executable());
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
    WindowsWorkerSessionProvider provider(worker_executable());
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
    // 搂1.25's case, brought back now that the operation it needed exists. Every
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

    WindowsWorkerSessionProvider provider(worker_executable());
    auto launched = provider.launch(launch_request.value(), context());
    REQUIRE(launched.ok());
    auto& session = *launched.value();
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    // The command 搂1.26 designed: it creates the bundle, provisions the volume,
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
        WindowsWorkerSessionProvider provider(worker_executable());
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
        WindowsWorkerSessionProvider provider(worker_executable());
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
        // case below and manifest 搂1.33.
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
        WindowsWorkerSessionProvider provider(worker_executable());
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
        // The finding and its minimal reproduction are manifest 搂1.33, and it is
        // not fixed here: the expectation lives inside M3's abort handling, and
        // the transfer manifest's mandatory gate forbids M5 from changing M3's
        // bundle, journal or manifest formats.
    }
    REQUIRE(wait_for_letter_release(letter));
}

TEST_CASE(m5_backend_stop_walks_quiesce_flush_commit_remove) {
    // The whole stop plan against a real worker, which is the other half of
    // 搂1.22's unexecuted list: `quiesce_mutations`, `remove_mount` and
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

    WindowsWorkerSessionProvider provider(worker_executable());
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
    // recorded in 搂1.34.
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

    WindowsWorkerSessionProvider provider(worker_executable());
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
    // The epoch confirmation is not optional, and its absence is 搂1.34(b): a
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
    // is still mounted.  This is the regression test for 搂1.38.
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

    WindowsWorkerSessionProvider provider(worker_executable());
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
// unrepresentable.  A separate case that fabricated a second lock to observe the
// refusal was written, run, and removed: what it measured is a property of
// `CreateFileW` plus a fabricated `ProductVolume`, and while it was in the tree
// it wedged the volume on the VM instead of discriminating anything.  The
// behaviour that actually matters -- a stop that freezes, flushes, commits and
// then dismantles cleanly, 20 times out of 20 -- is measured by
// `m5_backend_stop_walks_quiesce_flush_commit_remove` above, whose freeze step
// cannot pass unless the real lock is really held.
// ---------------------------------------------------------------------------
