#include "src/management/windows_worker_job.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace myrd::management {
namespace {

Error job_error(const wchar_t* operation, const wchar_t* message,
                const std::uint32_t native_code = 0) {
    return {ErrorDomain::win32, 1, operation, message, native_code};
}

// The creation time, as the identity of *this* process rather than of a pid.
// Read from a handle the caller already holds rather than by reopening the pid:
// reopening would be a second lookup that could land on a different process.
std::optional<std::uint64_t> creation_time_of(void* process_handle) noexcept {
    if (process_handle == nullptr) return std::nullopt;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(static_cast<HANDLE>(process_handle), &creation, &exit,
                        &kernel, &user)
        == FALSE) {
        return std::nullopt;
    }
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
}

} // namespace

const wchar_t* worker_job_name() noexcept {
    return L"Global\\MyRamDisk.M5.Workers";
}

Result<std::unique_ptr<WindowsWorkerProcessSource>>
WindowsWorkerProcessSource::create() {
    // `CreateJobObjectW` opens the existing job when the name is taken, which is
    // exactly the restart case: the predecessor's workers are still in it and
    // this instance must find them, not start a fresh world.
    HANDLE job = CreateJobObjectW(nullptr, worker_job_name());
    if (job == nullptr) {
        return Result<std::unique_ptr<WindowsWorkerProcessSource>>::failure(
            job_error(L"CreateJobObjectW",
                      L"the worker job object could not be created or opened",
                      GetLastError()));
    }

    // `BREAKAWAY_OK`, *not* `SILENT_BREAKAWAY_OK`.
    //
    // Both exist to keep a child the *worker* spawns -- the provisioning helper is
    // one -- from being dragged down with the worker.  They differ in a way that
    // cost a test to find: `SILENT_BREAKAWAY_OK` makes the job non-holding, which
    // is documented to mean `TerminateJobObject` and `KILL_ON_JOB_CLOSE` no longer
    // take effect on processes in it -- and it turns out the process is not
    // meaningfully *in* the job at all, so the job also stops accounting for it.
    // Measured: with `SILENT_BREAKAWAY_OK` set, a worker launched through
    // `PROC_THREAD_ATTRIBUTE_JOB_LIST` survived the launch and then vanished from
    // `JobObjectBasicProcessIdList` once the job handle was closed -- the startup
    // sweep would have been blind to exactly the orphans it exists to find.
    //
    // `BREAKAWAY_OK` grants the permission without waiving membership: a child
    // must still ask with `CREATE_BREAKAWAY_FROM_JOB`, and until today nothing in
    // this tree spawns anything from inside a worker.  Membership is the property
    // the sweep depends on, so membership is what wins.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    // Deliberately *not* set: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.  Setting it
    // would mean a service that died takes its workers with it, and a killed
    // worker cannot remove its drive letter, dismount its volume or stop AIM.
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits))
        == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        return Result<std::unique_ptr<WindowsWorkerProcessSource>>::failure(
            job_error(L"SetInformationJobObject",
                      L"the worker job limits could not be set", error));
    }

    return Result<std::unique_ptr<WindowsWorkerProcessSource>>::success(
        std::unique_ptr<WindowsWorkerProcessSource>(
            new WindowsWorkerProcessSource(job)));
}

WindowsWorkerProcessSource::WindowsWorkerProcessSource(void* job) noexcept
    : job_(job) {}

WindowsWorkerProcessSource::~WindowsWorkerProcessSource() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& entry : launched_) {
        if (entry.second.process_handle != nullptr) {
            CloseHandle(static_cast<HANDLE>(entry.second.process_handle));
        }
    }
    launched_.clear();
    // Closing the job handle does *not* end the processes in it, because
    // KILL_ON_JOB_CLOSE is not set.  That is the point: closing this is how the
    // sweep says "I am finished looking", not "everybody dies".
    if (job_ != nullptr) CloseHandle(static_cast<HANDLE>(job_));
}

bool WindowsWorkerProcessSource::valid() const noexcept {
    return job_ != nullptr;
}

Result<void> WindowsWorkerProcessSource::retain_launched(
    const std::uint32_t process_id, const std::uint64_t process_creation_time,
    void* const process_handle) {
    if (process_id == 0 || process_handle == nullptr) {
        return Result<void>::failure(job_error(
            L"retain_launched", L"a launched worker needs a pid and a handle"));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    // An entry keyed by this pid where the creation time differs is a *different*
    // process that reused the pid.  Its handle is closed rather than overwritten,
    // because a handle nobody holds is how a process leaks out of reach.
    const auto existing = launched_.find(process_id);
    if (existing != launched_.end()) {
        if (existing->second.process_handle != nullptr) {
            CloseHandle(static_cast<HANDLE>(existing->second.process_handle));
        }
        launched_.erase(existing);
    }
    launched_.emplace(process_id,
                      Entry{process_creation_time, process_handle});
    return Result<void>::success();
}

bool WindowsWorkerProcessSource::is_ours(
    const std::uint32_t process_id,
    const std::uint64_t process_creation_time) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = launched_.find(process_id);
    if (found == launched_.end()) return false;
    // Both halves, always.  A pid match with a different creation time is a
    // different process, which is the entire reason creation time is carried.
    return found->second.process_creation_time == process_creation_time;
}

Result<bool> WindowsWorkerProcessSource::wait_for_exit(
    const std::uint32_t process_id, const std::uint32_t timeout_ms) {
    HANDLE handle = nullptr;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = launched_.find(process_id);
        if (found == launched_.end() || found->second.process_handle == nullptr) {
            // No handle means nothing to wait on.  Reported as "not known to have
            // exited" rather than as a failure: the caller's next step is the same
            // either way, and the process may well be gone.
            return Result<bool>::success(false);
        }
        handle = static_cast<HANDLE>(found->second.process_handle);
    }
    const DWORD waited = WaitForSingleObject(handle, timeout_ms);
    if (waited == WAIT_OBJECT_0) return Result<bool>::success(true);
    if (waited == WAIT_TIMEOUT) return Result<bool>::success(false);
    return Result<bool>::failure(job_error(
        L"WaitForSingleObject", L"waiting for a worker to exit failed",
        GetLastError()));
}

std::vector<std::uint32_t>
WindowsWorkerProcessSource::launched_process_ids() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::uint32_t> ids;
    ids.reserve(launched_.size());
    for (const auto& entry : launched_) {
        ids.push_back(entry.first);
    }
    return ids;
}

Result<std::vector<OrphanedProcess>>
WindowsWorkerProcessSource::observe_orphans() const {
    if (job_ == nullptr) {
        return Result<std::vector<OrphanedProcess>>::failure(
            job_error(L"observe_orphans", L"the worker job is not open"));
    }

    // A snapshot of the live process table, not a job query.
    //
    // This started as `JobObjectBasicProcessIdList`, which is the obvious call
    // and which was measured *not to work here*: on the guest,
    // `IsProcessInJob(child, job)` reported 1 while the same job's
    // `JobObjectBasicProcessIdList` reported zero processes -- an enumeration that
    // silently omits every member is worse than one that fails, because the sweep
    // would conclude the world was clean.  Membership and observation are
    // therefore asked separately, and observation comes from the table.
    //
    // The cost of the change, stated: this reads processes this service did not
    // start, so "is it ours" is decided in two steps -- membership (a primitive
    // that measurably works) and then the recorded binding (pid *and* creation
    // time) in `sweep_verdicts`.  Neither step alone is sufficient, which is the
    // same division of labour the job query would have needed anyway.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return Result<std::vector<OrphanedProcess>>::failure(
            job_error(L"CreateToolhelp32Snapshot",
                      L"the process table could not be read", GetLastError()));
    }

    std::vector<OrphanedProcess> orphans;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(snapshot);
        // An empty table is not an error; `ERROR_NO_MORE_FILES` is how the
        // enumeration reports "there was nothing to walk".
        if (error == ERROR_NO_MORE_FILES) {
            return Result<std::vector<OrphanedProcess>>::success({});
        }
        return Result<std::vector<OrphanedProcess>>::failure(
            job_error(L"Process32FirstW", L"the process table is unreadable",
                      error));
    }

    do {
        const auto pid = static_cast<std::uint32_t>(entry.th32ProcessID);
        if (pid == 0) continue;

        // `PROCESS_QUERY_LIMITED_INFORMATION` and `SYNCHRONIZE`: enough to read
        // the creation time and to test whether it is still running, and nothing
        // that could end or command it.
        HANDLE probe = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (probe == nullptr) {
            // A process that cannot be opened is one this service cannot attribute
            // or terminate.  Skipped rather than guessed at: including it with no
            // identity would make it a `terminate_forced` candidate in the
            // verdicts, which is exactly the name-based cleanup the design
            // forbids.
            continue;
        }

        BOOL in_job = FALSE;
        if (IsProcessInJob(probe, static_cast<HANDLE>(job_), &in_job) == FALSE
            || in_job == FALSE) {
            CloseHandle(probe);
            continue;
        }

        // A process still inside the job that has already exited is not an orphan
        // to clean up -- its resources are the reclamation path's problem, not the
        // sweep's.  `WAIT_TIMEOUT` is the only outcome that means "still running".
        if (WaitForSingleObject(probe, 0) != WAIT_TIMEOUT) {
            CloseHandle(probe);
            continue;
        }

        OrphanedProcess orphan{};
        orphan.process_id = pid;
        orphan.process_creation_time = creation_time_of(probe);
        CloseHandle(probe);

        {
            // Processes this session launched are not orphans.  Matching on pid
            // *and* creation time, because either alone would eventually exclude
            // the wrong process.
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = launched_.find(pid);
            if (found != launched_.end() && orphan.process_creation_time.has_value()
                && found->second.process_creation_time
                    == *orphan.process_creation_time) {
                continue;
            }
        }

        // `profile_id` is left unset here.  The job can say which processes are in
        // it; it cannot say which profile each served -- that lives in the durable
        // records, which are the service layer's to read.  Leaving it unset is the
        // honest answer, and `sweep_verdicts` treats "no binding" as its own case
        // rather than as a match.
        orphans.push_back(orphan);
    } while (Process32NextW(snapshot, &entry) != FALSE);

    CloseHandle(snapshot);
    return Result<std::vector<OrphanedProcess>>::success(std::move(orphans));
}

Result<void> WindowsWorkerProcessSource::request_cooperative_exit(
    const OrphanedProcess& process) {
    // Reaching a *previous* service session's worker over the private channel
    // needs that session's startup secret, which by construction this process does
    // not have -- the secret is delivered once, through the worker's inherited
    // channel, and it died with its parent.
    //
    // So there is no cooperative path to an orphan, and saying so is the honest
    // answer rather than a fabricated success.  What an orphan *can* still do is
    // be ended, and its ordered teardown is then owed by the recovery path that
    // reconciles its durable records -- which is T1-06's subject, not this one's.
    (void)process;
    return Result<void>::failure(job_error(
        L"request_cooperative_exit",
        L"a worker from a previous service session cannot be reached over the "
        L"private channel: its startup secret died with that session"));
}

Result<void> WindowsWorkerProcessSource::terminate_process(
    const OrphanedProcess& process) {
    if (process.process_id == 0) {
        return Result<void>::failure(job_error(
            L"terminate_process", L"refusing to terminate pid zero"));
    }

    // Opened with the rights to end it, to wait on it, and to *read its creation
    // time* -- and the third is not optional even though nothing here sets out to
    // read it.  The re-check below needs it, and without it `GetProcessTimes` fails,
    // the live creation time comes back absent, and the check rejects the very
    // process it was invoked to end.  Measured: every forced termination failed with
    // `observed=134366055718126761 live=0` until this right was added, and the
    // symptom looked like pid recycling rather than like a missing access right.
    //
    // `PROCESS_QUERY_LIMITED_INFORMATION` rather than `PROCESS_QUERY_INFORMATION`:
    // the limited right is enough for a creation time and is granted where the full
    // one is not, which matters for a worker running under a different token.
    HANDLE handle = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        process.process_id);
    if (handle == nullptr) {
        const DWORD error = GetLastError();
        // A process that is already gone is the outcome that was wanted.  Reported
        // as success, because a caller that treats "already dead" as a failure
        // would escalate on a process that has done nothing wrong.
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
            return Result<void>::success();
        }
        return Result<void>::failure(job_error(
            L"terminate_process", L"the worker process could not be opened",
            error));
    }

    // The creation time is re-checked against the live process *at the moment of
    // termination*, because the observation the verdict was based on could be
    // stale: between reading the job's pid list and getting here, that pid could
    // have exited and been recycled.  Terminating on a stale pid is exactly the
    // stranger-killing case the creation time exists to prevent.
    if (process.process_creation_time.has_value()) {
        const auto live = creation_time_of(handle);
        if (!live.has_value() || *live != *process.process_creation_time) {
            CloseHandle(handle);
            return Result<void>::failure(job_error(
                L"terminate_process",
                L"the pid no longer names the process that was observed; "
                L"refusing to terminate a recycled pid"));
        }
    }

    const BOOL ended = TerminateProcess(handle, 1);
    const DWORD error = GetLastError();
    // Waited for with the handle already in hand, so the caller does not have to
    // come back and find out whether it worked.
    const DWORD waited = WaitForSingleObject(handle, 30'000);
    CloseHandle(handle);

    if (ended == FALSE) {
        return Result<void>::failure(job_error(
            L"TerminateProcess", L"the worker process could not be ended",
            error));
    }
    if (waited != WAIT_OBJECT_0) {
        return Result<void>::failure(job_error(
            L"WaitForSingleObject",
            L"the terminated worker did not retire within the budget", waited));
    }
    return Result<void>::success();
}

} // namespace myrd::management
