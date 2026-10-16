#pragma once

// The worker job object, and the one decision the service makes about the
// processes it finds inside it.
//
// Design spec section 5 puts three obligations on this: a *named* job so a
// restarted service can find its predecessor's workers, **no**
// `KILL_ON_JOB_CLOSE` so a killed service does not take its workers' ordered
// teardown with it, and **per-process** termination only, because terminating
// the whole job would let one stuck disk stop every other disk -- G4's isolation
// rule.
//
// The decision is separated from the Win32 for the same reason the reclaim
// decision is: a sweep's dangerous failure is a *conclusion*, not a syscall.  So
// `sweep_verdicts` is a pure function over values and is tested directly, and the
// job object is a thin source of observations behind `IWorkerProcessSource`.
//
// **The absence of a job-wide termination in this header is load-bearing.**  If a
// caller could reach `TerminateJobObject`, one disk's failure could take the
// others with it, and no test would notice until two disks were running.  There
// is deliberately no such method, no job handle in the interface, and no
// "terminate all" convenience.

#include "src/common/result.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myrd::management {

// One process the service found inside its worker job object.
struct OrphanedProcess final {
    std::uint32_t process_id{};
    // Read from the live process.  Absent when it could not be read at all,
    // which is *not* the same as zero: it is "I could not ask".
    std::optional<std::uint64_t> process_creation_time;
    // The profile whose binding recorded this process, when the service has such
    // a record.  Absent means the job holds a process the service cannot place.
    std::optional<product::Uuid> profile_id;
};

// What the service recorded about one profile's worker, so the sweep can tell
// "the worker I launched" from "a process that inherited its pid".
struct ExpectedWorkerBinding final {
    product::Uuid profile_id;
    // The creation time recorded with the binding.  Absent when a record exists
    // but carries no creation time -- which is a pid and nothing more.
    std::optional<std::uint64_t> recorded_creation_time;
    // Whether this profile's disk is expected to be *running* right now.
    //
    // The one case that is not a termination.  A profile the operator expects to
    // be serving has a worker that is supposed to be alive; a sweep that killed it
    // would stop a disk somebody believes is running, and the only honest answer
    // is to adopt it rather than to clean up after it.
    bool expects_running_worker{};
};

enum class SweepVerdict : std::uint8_t {
    // Our worker, for a profile we serve, and it owes an ordered teardown:
    // remove the drive letter, dismount, stop AIM.  Asked to shut down first,
    // forced only if it will not.
    terminate_cooperatively = 0,
    // Our worker, for a profile whose disk is expected to be running.  Adopt it;
    // do not terminate.
    adopt,
    // A process in our job with no binding we recognise.  Ours by construction
    // -- `AssignProcessToJobObject` is how it got there -- and unaddressable by
    // any operation, so it goes.  Forced, because there is nothing to ask: it has
    // no profile whose teardown it could run.
    terminate_forced,
    // Cannot be classified: no creation time to compare, or a pid that does not
    // match the binding's recorded process.  Left alone and reported.  "I cannot
    // tell" is not a licence to terminate.
    leave_alone,
};

struct SweepVerdictEntry final {
    std::uint32_t process_id{};
    std::optional<product::Uuid> profile_id;
    SweepVerdict verdict{SweepVerdict::leave_alone};
    bool operator==(const SweepVerdictEntry&) const noexcept = default;
};

// Decides what to do with every process observed inside the job.
//
// Pure.  Every input is a value the caller read; nothing here opens, enumerates
// or terminates anything.  The order of `observed` is preserved so a caller can
// pair verdicts with the processes it holds handles to.
[[nodiscard]] std::vector<SweepVerdictEntry> sweep_verdicts(
    const std::vector<OrphanedProcess>& observed,
    const std::vector<ExpectedWorkerBinding>& expected);

// The job object, as the service sees it.
class IWorkerProcessSource {
public:
    virtual ~IWorkerProcessSource() noexcept = default;

    // The processes currently in the job.  Never fails for "the job is empty" --
    // that is an empty vector and the normal state of a fresh service.
    [[nodiscard]] virtual Result<std::vector<OrphanedProcess>>
    observe_orphans() const = 0;

    // Asks one process to leave, using the private worker channel.  Returns
    // whether the request was delivered; delivery is not exit, and the caller
    // waits for the process it already holds a handle to.
    [[nodiscard]] virtual Result<void> request_cooperative_exit(
        const OrphanedProcess& process) = 0;

    // Ends one process.  Per-process by name and by signature: there is no
    // job-wide counterpart, and adding one would be the G4 violation this
    // interface exists to prevent.
    [[nodiscard]] virtual Result<void> terminate_process(
        const OrphanedProcess& process) = 0;
};

} // namespace myrd::management
