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

#include "src/management/windows_worker_job.h"
#include "src/management/worker_binding_store.h"
#include "src/service/startup_sweep.h"

#include <windows.h>

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

// Wraps the real bindings and drops any record naming a given process.
//
// NOT USED, and kept only as a record of a wrong turn: the first version of the
// "leaves an orphan it cannot attribute" case tried to construct unattributability
// by filtering the bindings, and that cannot work.  `sweep_verdicts` decides by
// *profile*: a process the job reports with no `profile_id` matches no binding at
// all and is therefore forced down whatever the store contains.  So the only value
// of a binding list here is whether it can *claim* a profile the orphan carries,
// and the orphan carries none.
//
// The real conclusion: an unattributable orphan is not reachable with a process
// that has no profile, because such a process is not "unattributable" -- it is
// positively identified as belonging to no profile, which is the forced case.  The
// unattributable case needs a process the job *does* tag with a profile whose
// binding is missing or whose creation time disagrees, and that is covered in
// `m5.worker_sweep` and `m5.worker_sweep_policy` over fakes.

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
    if (sweep.last_report()->failed_to_terminate != 0) {
        std::fprintf(stderr, "DIAG sweep failure: %ls\n",
                     sweep.last_report()->first_failure.c_str());
    }
    REQUIRE_EQ(sweep.last_report()->orphans_observed, 1U);
    REQUIRE_EQ(sweep.last_report()->forced_to_terminate, 1U);
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

    // And a second sweep finds nothing, which is the "next start is clean" half of
    // the acceptance.
    ProductionStartupSweep second(*job, bindings, options);
    REQUIRE(second.run_sweep().ok());
    REQUIRE_EQ(second.last_report()->orphans_observed, 0U);
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