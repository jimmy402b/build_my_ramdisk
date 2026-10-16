// The startup sweep against a real orphan in the real job.
//
// T1-03's acceptance is "kill the worker, and the next start sweeps the leftover
// away without leaving a device or a drive letter".  The part of that which needs
// a real process and a real job object is here; the decisions were measured in
// `m5.worker_sweep_policy` and `m5.worker_sweep` over fakes, where a real orphan
// would not be reproducible on demand.
//
// The scenario is built the way a crash builds it, not by simulating one:
//
//   1. launch a worker through the production provider, so the job, the retained
//      handle and the durable fingerprint are all real;
//   2. drop the session without shutting it down -- the worker stays alive, which
//      is exactly the orphan state, and the fingerprint survives on disk;
//   3. run the production sweep, over the real job, with the bindings it derives
//      from that store;
//   4. the orphan must be *gone*.
//
// The assertion that matters is not "the sweep reported success" but "no process
// with that creation time is in the job any more".  A sweep that reported acting
// without acting would pass the first and fail the second.
//
// A silent worker is used rather than a real `myrd-m5-worker`, and that is
// deliberate: the subject here is the *sweep*, and a worker whose private channel
// is reachable would take the cooperative path, which the fake-based cases already
// cover.  A process that cannot be asked down is the case the kill acceptance is
// about.

#include "tests/support/test_framework.h"

// The worker-launch fixtures live in the backend suite and are NOT shared by
// including it, which is a correction worth recording.
//
// The first version of this file did include it -- the mechanism this tree uses for
// shared fixtures -- and that brought the whole backend suite's `TEST_CASE`s with it.
// They launch real workers, some of which outlive the case that started them, and the
// sweep cases then ran *after* them in the same process and found orphans another case
// had created.  Measured: `-Repeat 20` gave 18/20, both failures on
// `second.last_report()->orphans_observed == 0`, from runs that had simply had more
// stragglers.  Not a product defect, and it would have been reported as one.
//
// So the few helpers this file needs are stated below instead.  Duplication is the
// lesser cost here: a properly shared header is the real fix and is recorded as such,
// but a source include that silently enlarges the test's world is not a fixture
// mechanism.

#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_job.h"
#include "src/management/windows_worker_sessions.h"
#include "src/management/worker_binding_store.h"
#include "src/service/startup_sweep.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace myrd;
using namespace myrd::management;
using namespace myrd::service;

namespace {

product::Uuid sweep_profile() {
    auto parsed = product::Uuid::parse("cccccccc-1111-2222-3333-444444444444");
    REQUIRE(parsed.ok());
    return parsed.value();
}

// This file creates real processes and terminates them, so by `AGENTS.md` section 4
// it may only run on the guest.
//
// Registered like every other test so it is selectable by name on the VM -- but
// guarded, because the first version was not and it *did* run here: a
// `ctest -R "m5\."` on the development machine spent 60 seconds launching and
// killing `cmd.exe` children.  Nothing was left behind, but that is luck rather than
// design, and the same mistake with an AIM device would not have been.
//
// The marker is an environment variable the *guest runner sets inside the cmd shell
// it spawns the test from*.  Not a path, and not `ProcessStartInfo.EnvironmentVariables`:
// both were tried and both were wrong.  A path cannot distinguish the guest from the
// development machine, because staging mirrors the development layout (that is the
// whole point of the mirroring), so the first path-based version let the test run
// here -- the exact thing the guard exists to prevent.  The process-environment form
// is lost in the `cmd /c` wrapper, so it refused on the guest too.
//
// Refusing *loudly* is deliberate: a silently skipped test would leave the
// acceptance unmeasured while the suite reported success.
[[nodiscard]] bool guest_environment() {
    wchar_t marker[8]{};
    return GetEnvironmentVariableW(L"MYRD_M5VM_GUEST", marker, 8) != 0;
}

struct GuestGuard final {
    GuestGuard() {
        if (guest_environment()) return;
        std::fprintf(stderr,
                     "m5.startup_sweep creates real worker processes and may only "
                     "run on the guest (AGENTS.md section 4).  Use "
                     "tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^m5\\.startup_sweep$' "
                     "rather than ctest on the development machine.\n");
        std::_Exit(2);
    }
};

const GuestGuard kGuestGuard{};

// ---------------------------------------------------------------------------
// The worker-launch fixtures, stated locally.  See the note above for why these are
// duplicated rather than included from the backend suite.
// ---------------------------------------------------------------------------

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

class TempBundle final {
public:
    explicit TempBundle(const wchar_t* tag) {
        root = std::filesystem::temp_directory_path()
            / (std::wstring(L"myrd-m5-sweep-") + tag + L"-"
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
// constructor, so it is built through the aggregate.  The lifecycle is `ready`
// deliberately: a `provisioning` profile makes the worker create and provision a real
// volume, and these cases are about the sweep, not about provisioning.
profile::ProfileRecord valid_profile(const TempBundle& bundle) {
    profile::Profile value{
        uuid("11111111-2222-3333-4444-555555555555"),
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        profile::ProfileLifecycle::ready,
        bundle.root.wstring(),
        "disk.rdm",
        uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"),
        8ULL * 1024 * 1024,
        512,
        4096,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        profile::MountKind::drive_letter,
        L"R:",
        false,
        profile::MemoryPolicy{},
        profile::CheckpointPolicy{},
        profile::CompactionPolicy{}};
    return profile::ProfileRecord{7, std::move(value)};
}

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
}

// The worker binary the build just produced, found relative to this test's own
// module.  The relative path comes from CMake so the build root is not baked into the
// binary, and existence is asserted rather than assumed.
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

// The worker job, kept alive for one case.
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

// The provider needs a launch request, and building one by hand would not be the
// thing production sends -- so it goes through the same translation the service uses.
WorkerLaunchRequest request_for(const profile::ProfileRecord& profile) {
    auto translated = make_launch_request(profile);
    REQUIRE(translated.ok());
    return translated.value();
}

// Runs sweeps until the job reports no orphans, up to a deadline, and says whether it
// ever got there.
//
// One sweep is one *instant*, and this job is shared: it is the named job the whole
// service uses, and in this harness another case's worker may still be finishing when
// this one sweeps.  A single sweep reporting an orphan is therefore not a product
// failure -- it is the sweep correctly reporting a process that is going away, which a
// real startup sees too, because a previous instance's worker takes a moment to leave
// after its parent dies.
//
// What must hold is the *convergence*: repeated sweeps reach a clean job.  That is the
// property a service depends on, and asserting a single instant's reading instead is
// what made `-Repeat 20` report 19/20 for a defect that did not exist.
[[nodiscard]] bool sweep_until_clean(ProductionStartupSweep& sweep,
                                     const std::uint32_t deadline_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        if (!sweep.run_sweep().ok()) return false;
        if (sweep.last_report().has_value()
            && sweep.last_report()->orphans_observed == 0) {
            return true;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= deadline_ms) return false;
        Sleep(100);
    }
}

std::unique_ptr<WindowsWorkerProcessSource> open_sweep_job() {
    auto opened = WindowsWorkerProcessSource::create();
    REQUIRE(opened.ok());
    return opened.move_value();
}

// The durable root the installer created, which is where the fingerprint store
// lives in production.  Read from the environment rather than hardcoded: the
// installer decides where it lives, and a test that hardcoded it would pass on
// this machine and mislead on one where the decision changed.
std::wstring durable_root() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer,
                                                 static_cast<DWORD>(MAX_PATH));
    REQUIRE(length != 0);
    REQUIRE(length < MAX_PATH);
    return std::wstring(buffer, length);
}


// A process in the job that nothing can ask to leave.
//
// `cmd.exe /c ping -n 60` waits roughly a minute, which is far longer than the
// sweep's cooperative budget but short enough that a leaked one cleans itself up.
// It exits immediately if the command fails, so its liveness is asserted rather
// than assumed -- a dead process cannot be an orphan, and an earlier version of
// this scenario was wrong for exactly that reason.
struct SilentProcess final {
    std::uint32_t pid{};
    std::uint64_t creation_time{};
};

Result<SilentProcess> start_silent_process_in_job(void* const job) {
    const std::wstring command = L"cmd.exe /c ping -n 60 127.0.0.1 > nul";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    STARTUPINFOEXW startup_info{};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    constexpr DWORD kAttributeCount = 1;
    SIZE_T attribute_bytes = 0;
    static_cast<void>(InitializeProcThreadAttributeList(nullptr, kAttributeCount,
                                                        0, &attribute_bytes));
    std::vector<std::byte> attribute_storage(attribute_bytes);
    startup_info.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage.data());
    if (InitializeProcThreadAttributeList(startup_info.lpAttributeList,
                                          kAttributeCount, 0, &attribute_bytes)
        == FALSE) {
        return Result<SilentProcess>::failure(
            {ErrorDomain::win32, 1, L"InitializeProcThreadAttributeList",
             L"attribute list failed", GetLastError()});
    }
    HANDLE job_list[1] = {static_cast<HANDLE>(job)};
    if (UpdateProcThreadAttribute(startup_info.lpAttributeList, 0,
                                  PROC_THREAD_ATTRIBUTE_JOB_LIST, job_list,
                                  sizeof(job_list), nullptr, nullptr)
        == FALSE) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(startup_info.lpAttributeList);
        return Result<SilentProcess>::failure(
            {ErrorDomain::win32, 2, L"UpdateProcThreadAttribute",
             L"job attribute failed", error});
    }

    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup_info.StartupInfo, &info);
    const DWORD start_error = GetLastError();
    DeleteProcThreadAttributeList(startup_info.lpAttributeList);
    if (started == FALSE) {
        return Result<SilentProcess>::failure(
            {ErrorDomain::win32, 3, L"CreateProcessW",
             L"the silent process could not be started", start_error});
    }
    static_cast<void>(CloseHandle(info.hThread));

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(info.hProcess, &creation, &exit, &kernel, &user)
        == FALSE) {
        const DWORD error = GetLastError();
        static_cast<void>(TerminateProcess(info.hProcess, 1));
        static_cast<void>(CloseHandle(info.hProcess));
        return Result<SilentProcess>::failure(
            {ErrorDomain::win32, 4, L"GetProcessTimes",
             L"the silent process's creation time could not be read", error});
    }
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;

    SilentProcess process{static_cast<std::uint32_t>(info.dwProcessId),
                          value.QuadPart};
    static_cast<void>(CloseHandle(info.hProcess));
    return Result<SilentProcess>::success(process);
}

} // namespace

TEST_CASE(m5_sweep_terminates_a_real_orphan_it_can_attribute) {
    // The kill acceptance, in one case.  A process is in the real named job, the
    // durable store names it, and the production sweep must end it.
    auto job = open_sweep_job();
    REQUIRE(job->valid());

    const auto started = start_silent_process_in_job(job->job_handle());
    REQUIRE(started.ok());
    const auto orphan = started.value();
    REQUIRE(orphan.pid != 0);

    // Liveness first.  A process that already exited is not an orphan, and a sweep
    // "succeeding" against a corpse would prove nothing.
    {
        HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, orphan.pid);
        REQUIRE(probe != nullptr);
        REQUIRE_EQ(WaitForSingleObject(probe, 0), WAIT_TIMEOUT);
        static_cast<void>(CloseHandle(probe));
    }

    // The store a previous service session would have left behind, in the real
    // on-disk format.  This is the durable half of the attribution: without it the
    // sweep answers `leave_alone` for everything, correctly, and never acts.
    // The *installer's* durable root, not a temporary directory.
    //
    // The store refuses a root whose ACL is not protected, and that refusal is the
    // point of the check -- a fingerprint outside the protected root is one an
    // unprivileged process could rewrite to attribute its own process to a
    // profile.  A temp directory is therefore not usable, and using the real root
    // is also the better test: this is the path production takes.
    const auto store_root = durable_root();
    // `over_protected_root`, not `open`: the leaf's *creation* has its own
    // coverage, and what this case needs is the real atomic directory -- so the
    // four-step publish is measured against real Windows file I/O rather than
    // against an in-memory map, which is where a publish that never actually
    // worked would hide.
    auto store = WorkerBindingStore::over_protected_root(store_root);
    if (!store.ok()) {
        std::fprintf(stderr, "DIAG store open failed op=%ls native=%u msg=%ls\n",
                     store.error().operation.c_str(), store.error().native_code,
                     store.error().message.c_str());
    }
    REQUIRE(store.ok());
    const auto recorded = store.value().record(
        WorkerFingerprintRecord{sweep_profile(), orphan.pid, orphan.creation_time,
                                3});
    if (!recorded.ok()) {
        std::fprintf(stderr, "DIAG record failed op=%ls native=%u msg=%ls\n",
                     recorded.error().operation.c_str(),
                     recorded.error().native_code,
                     recorded.error().message.c_str());
    }
    REQUIRE(recorded.ok());

    // The orphan has no profile in the job enumeration -- the job cannot know one
    // -- so the binding is supplied the way the composition root supplies it.
    // The *real* adapter over the real store, not a fake: the record-then-list
    // path is where this store's two parser defects lived, so a case that stubbed
    // the bindings would have been measuring the stub.
    FingerprintWorkerBindings bindings(
        store.value(), [](const product::Uuid&) { return false; });

    WorkerSweepOptions options{};
    // The orphan cannot be asked down: it is not a worker and has no channel.  The
    // escalation is what this case is about, so it is enabled explicitly.
    options.force_when_unreachable = true;
    ProductionStartupSweep sweep(*job, bindings, options);

    const auto swept = sweep.run_sweep();
    REQUIRE(swept.ok());
    REQUIRE(sweep.last_report().has_value());
    REQUIRE_EQ(sweep.last_report()->orphans_observed, 1U);
    REQUIRE_EQ(sweep.last_report()->forced_to_terminate, 1U);
    // The reason is in the report, so a failure here says *why* rather than only that
    // a termination did not happen: `first_failure` carries the message the source
    // produced.
    REQUIRE_EQ(sweep.last_report()->failed_to_terminate, 0U);

    // The assertion that matters: the process is gone, by the identity that cannot
    // be recycled rather than by the pid.
    {
        HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, orphan.pid);
        if (probe != nullptr) {
            // It may still be a zombie for a moment; the wait is what settles it.
            REQUIRE_EQ(WaitForSingleObject(probe, 30'000), WAIT_OBJECT_0);
            static_cast<void>(CloseHandle(probe));
        }
    }

    // And repeated sweeps reach a clean job, which is the "next start is clean" half
    // of the acceptance.  Convergence rather than one instant -- see
    // `sweep_until_clean` for why a single reading is not the property.
    ProductionStartupSweep second(*job, bindings, options);
    REQUIRE(sweep_until_clean(second, 30'000));
    REQUIRE(!second.needs_attention());

    // The fingerprint is removed, not the root: the root is the installer's and is shared.
    static_cast<void>(store.value().forget(sweep_profile()));
}

// The "leaves an orphan it cannot attribute" case is deliberately absent, and the
// reason is a property of the design rather than a gap in the test.
//
// An unattributable orphan is a process the job reports *with a profile* whose
// binding is missing or whose creation time disagrees.  A process the job reports
// with no profile at all is not unattributable -- it is positively identified as
// belonging to no profile, which is the *forced* case, and the first case above
// exercises exactly that.  So the unattributable path needs an orphan the job tags
// with a profile, and the job cannot tag one: which profile a worker served lives in
// the durable records, which the job does not read.
//
// That path is therefore measured where it can be constructed --
// `m5.worker_sweep` and `m5.worker_sweep_policy`, over fakes, where a process can be
// given a profile whose binding disagrees.  An integration case here would have to
// manufacture a job-tagged process, which the production code has no way to produce
// and therefore no way to get wrong.

TEST_CASE(m5_sweep_finds_nothing_after_a_worker_is_killed) {
    // Half of T1-03's kill acceptance, and the half that needs no disk: **a killed
    // worker must not leave a process nobody can account for.**
    //
    // The other half -- "and no device or drive letter either" -- needs a real AIM
    // device, because the residue it is about is created by the worker's own mount
    // path.  This case establishes what can be established without one, and does not
    // claim the rest.
    //
    // The worker is launched through the production provider, so the job membership
    // and the durable fingerprint are both the real thing.  Then it is killed the way
    // a crash kills it -- `TerminateProcess`, no ordered teardown -- and the next
    // start's sweep must find the job clean.  A worker that somehow left a second
    // process behind, or that kept its own process alive through a handle this
    // service holds, would show up here as an orphan.
    TempBundle bundle(L"sweep-kill");
    WorkerJobFixture job;
    WindowsWorkerSessionProvider provider(worker_executable(), job.source());

    // Scoped so the session is released before the sweep runs: a service death
    // closes the private pipe, and leaving it open would give the worker a reason to
    // exit that has nothing to do with being killed.
    const std::uint32_t worker_pid = [&] {
        auto session =
            provider.launch(request_for(valid_profile(bundle)), context());
        REQUIRE(session.ok());
        const std::uint32_t pid = session.value()->binding().process_id;
        REQUIRE(pid != 0);

        // Killed abruptly, with the session still open -- so nothing this test does
        // *asks* it to leave, and a worker that only leaves when asked would fail
        // here.
        HANDLE victim = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        REQUIRE(victim != nullptr);
        REQUIRE(TerminateProcess(victim, 1) != FALSE);
        REQUIRE_EQ(WaitForSingleObject(victim, 30'000), WAIT_OBJECT_0);
        static_cast<void>(CloseHandle(victim));
        return pid;
    }();

    // The next start, over the same job and the real durable store.
    auto store = WorkerBindingStore::over_protected_root(durable_root());
    REQUIRE(store.ok());
    FingerprintWorkerBindings bindings(
        store.value(), [](const product::Uuid&) { return false; });

    WorkerSweepOptions options{};
    ProductionStartupSweep sweep(job.source(), bindings, options);

    // No orphan, once the job has settled.  The killed worker is gone, so there is no
    // process for a sweep to reclaim -- which is the property that keeps a crash from
    // accumulating processes nobody can attribute.  Convergence, because this job is
    // shared and another case's worker may still be finishing.
    REQUIRE(sweep_until_clean(sweep, 30'000));
    REQUIRE(sweep.last_report().has_value());
    REQUIRE_EQ(sweep.last_report()->orphans_observed, 0U);
    REQUIRE(!sweep.needs_attention());

    // And it is *gone*, not merely unlisted: a pid that no longer opens is the
    // statement that the process object has been reclaimed too.
    {
        HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, worker_pid);
        if (probe != nullptr) {
            REQUIRE_EQ(WaitForSingleObject(probe, 0), WAIT_OBJECT_0);
            static_cast<void>(CloseHandle(probe));
        }
    }

    // The fingerprint was recorded, which is what made the launch attributable in
    // the first place; it is removed here because the store is the installer's shared
    // durable root and this case must not leave state for another to trip over.
    static_cast<void>(store.value().forget(sweep_profile()));
}

TEST_CASE(m5_sweep_finds_nothing_after_a_worker_leaves_cleanly) {
    // The other way a worker stops, and the one that shows why `KILL_ON_JOB_CLOSE` is
    // unset rather than merely undocumented.
    //
    // A worker whose pipe closes -- because the service went away -- exits on its
    // own, which is its *ordered* teardown: it is the path that removes the drive
    // letter, dismounts the volume and stops AIM.  A job that killed its members on
    // close would `TerminateProcess` them instead and leave the residue the sweep
    // then has to clean up.
    //
    // So this case asserts the observable consequence: after a clean exit the job is
    // clean too, and the next start has nothing to reclaim.
    TempBundle bundle(L"sweep-clean-exit");
    WorkerJobFixture job;
    WindowsWorkerSessionProvider provider(worker_executable(), job.source());

    const std::uint32_t worker_pid = [&] {
        auto session =
            provider.launch(request_for(valid_profile(bundle)), context());
        REQUIRE(session.ok());
        const std::uint32_t pid = session.value()->binding().process_id;
        REQUIRE(pid != 0);

        // Asked to shut down, which is what the service does on its way out.
        REQUIRE(session.value()
                    ->shutdown(context(),
                               uuid("99999999-8888-7777-6666-555555555555"))
                    .ok());
        // Waited for on the *retained* handle, which is what that handle is for.
        REQUIRE(job.source().wait_for_exit(pid, 30'000).ok());
        REQUIRE(job.source().wait_for_exit(pid, 30'000).value());
        return pid;
    }();
    static_cast<void>(worker_pid);

    auto store = WorkerBindingStore::over_protected_root(durable_root());
    REQUIRE(store.ok());
    FingerprintWorkerBindings bindings(
        store.value(), [](const product::Uuid&) { return false; });

    WorkerSweepOptions options{};
    ProductionStartupSweep sweep(job.source(), bindings, options);
    REQUIRE(sweep_until_clean(sweep, 30'000));
    REQUIRE(!sweep.needs_attention());

    static_cast<void>(store.value().forget(sweep_profile()));
}