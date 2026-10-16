#include "src/management/windows_worker_job.h"

#include <windows.h>

#include <cstddef>
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

    // `SILENT_BREAKAWAY_OK` so a child the *worker* spawns -- the provisioning
    // helper is one -- does not join this job.  Without it, terminating a worker
    // would drag its helper down, and the escrow between the two is not
    // something this service is entitled to decide.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
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

Result<std::vector<OrphanedProcess>>
WindowsWorkerProcessSource::observe_orphans() const {
    if (job_ == nullptr) {
        return Result<std::vector<OrphanedProcess>>::failure(
            job_error(L"observe_orphans", L"the worker job is not open"));
    }

    // Two calls by contract: ask for the byte count, then ask for the list.  The
    // count can grow between them, so an `ERROR_MORE_DATA` is a retry rather than
    // a failure -- a worker launched by something else while we enumerate must not
    // make the sweep refuse to run.
    DWORD needed = 0;
    (void)QueryInformationJobObject(job_, JobObjectBasicProcessIdList, nullptr, 0,
                                    &needed);
    if (needed == 0) {
        // A job with no processes reports zero bytes needed, which is an empty
        // list and not an error.  A fresh service finds exactly this.
        return Result<std::vector<OrphanedProcess>>::success({});
    }
    std::vector<std::byte> buffer(needed + sizeof(ULONG_PTR) * 16);
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    if (QueryInformationJobObject(job_, JobObjectBasicProcessIdList, list, 
                                  static_cast<DWORD>(buffer.size()), &needed)
        == FALSE) {
        return Result<std::vector<OrphanedProcess>>::failure(
            job_error(L"QueryInformationJobObject",
                      L"the worker job's processes could not be listed",
                      GetLastError()));
    }

    std::vector<OrphanedProcess> orphans;
    for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
        const auto pid = static_cast<std::uint32_t>(list->ProcessIdList[index]);
        if (pid == 0) continue;

        OrphanedProcess orphan{};
        orphan.process_id = pid;

        // The creation time needs a handle, but not a *retained* one.  Opened for
        // query only, so a process this session launched is recognised and then
        // released -- the retained handle is the launcher's, and holding a second
        // one here would keep a dead worker's process object alive for as long as
        // the sweep took.
        HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (probe != nullptr) {
            orphan.process_creation_time = creation_time_of(probe);
            CloseHandle(probe);
        }

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
    }
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

    // Opened with the rights to end it and nothing else.  `PROCESS_TERMINATE`
    // alone is not enough to wait on it, which is deliberate: waiting is the
    // retained handle's job, and this call is the escalation that only happens
    // after that wait timed out.
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
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
