#pragma once

// The production worker job object.
//
// Three obligations from design spec section 5 are encoded here, and each is
// something a later refactor could quietly undo:
//
//   1. **Named.**  `Global\MyRamDisk.M5.Workers`, because a restarted service has
//      to find the workers its predecessor left behind.  An unnamed job would be
//      a fresh, empty world every restart and the sweep would have nothing to
//      sweep.
//   2. **No `KILL_ON_JOB_CLOSE`.**  Setting it would mean "the service died, so
//      the workers die" -- and a `TerminateProcess`ed worker never gets to remove
//      its drive letter, dismount its volume or stop AIM.  The whole point of the
//      sweep is that an orphan is still able to run its own ordered teardown.
//   3. **Per-process termination only.**  See `worker_process_source.h`: there is
//      no job-wide terminate, on purpose, because that is G4's isolation rule.
//
// It also owns the live-worker registry: the processes *this* session launched,
// with the creation time each was launched with.  `observe_orphans` reports what
// is in the job but not in that registry, which is precisely "left over from a
// previous life".  The registry is what `attach()` has been waiting for.

#include "src/management/worker_process_source.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace myrd::management {

// The name a restarted service reopens.  A function rather than a literal at the
// call site so the job the sweep opens and the job the launcher assigns into
// cannot drift apart.
[[nodiscard]] const wchar_t* worker_job_name() noexcept;

class WindowsWorkerProcessSource final : public IWorkerProcessSource {
public:
    // Creates the job if it does not exist, or opens the one a previous service
    // instance left.  Fails rather than proceeding jobless: a launcher that could
    // not assign to a job would produce workers the sweep can never find, which is
    // worse than not starting at all.
    [[nodiscard]] static Result<std::unique_ptr<WindowsWorkerProcessSource>>
    create();

    ~WindowsWorkerProcessSource() noexcept override;
    WindowsWorkerProcessSource(const WindowsWorkerProcessSource&) = delete;
    WindowsWorkerProcessSource& operator=(const WindowsWorkerProcessSource&) =
        delete;

    // Records a worker this session launched, and returns the process handle the
    // caller must keep.  Taking ownership here is what stops the handle being
    // dropped on the floor the way `close_launched_worker` used to: the source
    // needs it to wait for the process, which is how an ordered teardown is
    // allowed to finish before anything escalates.
    [[nodiscard]] Result<void> retain_launched(
        std::uint32_t process_id, std::uint64_t process_creation_time,
        void* process_handle);

    // Whether this session launched that exact process.
    [[nodiscard]] bool is_ours(std::uint32_t process_id,
                               std::uint64_t process_creation_time) const;

    // Waits, bounded, for one retained process to exit.  Returns whether it did.
    [[nodiscard]] Result<bool> wait_for_exit(std::uint32_t process_id,
                                             std::uint32_t timeout_ms);

    // The processes this session launched, by pid.
    //
    // For a caller that needs to act on *this session's* worker rather than on an
    // orphan -- a test that wants to kill one, and a future teardown that wants to walk
    // the profiles it owns.  It is deliberately only the pids: the handles stay private,
    // because a caller that could close one would leave a worker observable and
    // un-waitable, which is the state `retain_launched` exists to make impossible.
    [[nodiscard]] std::vector<std::uint32_t> launched_process_ids() const;

    [[nodiscard]] Result<std::vector<OrphanedProcess>> observe_orphans()
        const override;
    [[nodiscard]] Result<void> request_cooperative_exit(
        const OrphanedProcess& process) override;
    [[nodiscard]] Result<void> terminate_process(
        const OrphanedProcess& process) override;

    // The job handle, for the launcher to place a new process into.  Exposed as
    // an opaque pointer rather than a `HANDLE` so callers of this header do not
    // take a dependency on `<windows.h>`, and deliberately *only* for creation
    // attributes: there is no accessor that could be handed to
    // `TerminateJobObject`, which is the G4 rule this class exists to enforce.
    [[nodiscard]] void* job_handle() const noexcept { return job_; }

    // For a case that needs to reason about the job without a process in it.
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit WindowsWorkerProcessSource(void* job) noexcept;

    struct Entry final {
        std::uint64_t process_creation_time{};
        void* process_handle{};
    };

    void* job_{};
    mutable std::mutex mutex_;
    // Keyed by pid, but a match is only a match when the creation time agrees --
    // see `is_ours`.  Pids are recycled, so the key alone would eventually name a
    // process this session never launched.
    std::map<std::uint32_t, Entry> launched_;
};

} // namespace myrd::management
