#pragma once

// **The consumers the checkpoint policy never had.**
//
// `profile::CheckpointPolicy` has carried `interval_seconds`,
// `dirty_threshold_bytes`, `on_memory_pressure` and `on_suspend` since the profile
// schema was written, and every one of them was serialized, validated and read --
// and then dropped on the floor.  Nothing in the service ever looked at them, so a
// profile configured to checkpoint every 60 seconds checkpointed whenever the user
// typed the command.  The acceptance for T1-09 ("a power cut loses at most one
// interval") is not a statement about the recovery path until this exists.
//
// This object is the whole consumer: it evaluates the policy for every running
// profile against facts the worker reports, and hands the work to the durable
// operation queue.  It never executes a checkpoint itself -- admission is durable,
// execution belongs to the scheduler -- which is what makes the suspend path
// bounded: the service says "checkpoint these" and the machine may go away.

#include "src/management/execution_scheduler.h"
#include "src/management/service_core.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace myrd::management {

// Why a checkpoint was asked for, and whether the service accepted the request.
// A string rather than an enum because it is written into the service log and read
// by a person, and a later reason must not have to borrow an old name.
struct MaintenanceDecision final {
    product::Uuid profile_id;
    std::string reason;
    bool dispatched{};
};

struct ServiceMaintenanceOptions final {
    // How often the policy is evaluated.  A tick with nothing to do costs one list
    // and no worker commands beyond the status reads that list already takes.
    std::uint32_t tick_milliseconds{15'000};
    // How long a tick waits for the execution gate.  A tick that cannot enter the
    // core is *skipped*, not queued: the state it would act on has moved on, and the
    // next tick is seconds away.
    std::uint32_t gate_timeout_milliseconds{250};
};

// Everything one decision is made of, with no I/O in it.
struct MaintenanceInput final {
    ProfileState state{};
    profile::CheckpointPolicy checkpoint{};
    profile::MemoryPolicy memory{};
    std::uint64_t resident_bytes{};
    std::uint64_t dirty_bytes{};
    std::uint64_t since_last_checkpoint_ms{};
};

// The policy itself.  Returns the reason a checkpoint is due, or an empty string.
//
// Order is deliberate and stated rather than incidental: memory pressure first
// (it is the condition that gets worse on its own), then the dirty threshold, then
// the interval.  A profile whose runtime is not running never checkpoints.
[[nodiscard]] std::string checkpoint_due_reason(const MaintenanceInput& input);

// The suspend/hibernate half of the checkpoint policy, as the service host sees it.
//
// A machine going to sleep is not a shutdown: nothing is torn down, the workers stay
// up, and the only thing that has to happen is that the writes a profile is holding
// get a chance to reach the journal.  So this is deliberately *not* part of `stop` --
// a suspend that stopped the service would lose the very state it exists to protect.
//
// It is bounded by construction: the implementation admits durably and returns, so a
// power budget cannot be outlived by the work.  What happens if the machine goes away
// before a commit lands is the recovery path's business (T1-09).
class ISuspendCheckpoint {
public:
    virtual ~ISuspendCheckpoint() noexcept = default;

    // Hands every eligible running profile a checkpoint.  Returns how many were
    // admitted; a policy that admits nothing is a legitimate answer.
    virtual std::size_t checkpoint_for_suspend() = 0;
};

// The service's policy clock and dispatcher.
class ServiceMaintenance final : public IMaintenanceTick,
                                 public ISuspendCheckpoint {
public:
    ServiceMaintenance(ServiceCore& core, IServiceClock& clock,
                       IServiceLogger& logger, std::timed_mutex& execution_gate,
                       ServiceMaintenanceOptions options = {}) noexcept;

    ServiceMaintenance(const ServiceMaintenance&) = delete;
    ServiceMaintenance& operator=(const ServiceMaintenance&) = delete;

    // One policy evaluation, called from the scheduler's poll loop.
    void tick() override;

    // Suspend or hibernate is imminent.  Every running profile whose `on_suspend`
    // policy is `best_effort` is handed a checkpoint now, whatever the interval and
    // thresholds say, and the call returns as soon as the work is durably admitted.
    // That bound is the design: the machine may be gone before the commit lands, and
    // the recovery path is what covers that.
    std::size_t checkpoint_for_suspend() override;

    // What the last tick decided, for the diagnostics a test or an operator asks for.
    [[nodiscard]] std::vector<MaintenanceDecision> last_decisions() const;

private:
    struct Candidate final {
        product::Uuid profile_id;
        std::wstring target;
        std::string reason;
    };

    // Evaluates every running profile.  `for_suspend` skips the thresholds and uses
    // the suspend policy instead.  Returns empty when the gate could not be taken:
    // "could not ask" is not "nothing to do", but it is also not a reason to fabricate
    // a decision list.
    std::vector<MaintenanceDecision> evaluate(bool for_suspend);

    void record_last_checkpoint(const product::Uuid& profile_id,
                                const std::uint64_t at_ms);
    // Starts the interval clock for a profile the loop has not seen.  Stated as its
    // own step because leaving it out is invisible until a policy is measured: the
    // interval then never elapses and looks like "the policy is not wired".
    void seed_last_checkpoint_if_absent(const product::Uuid& profile_id,
                                        const std::uint64_t at_ms);
    [[nodiscard]] std::uint64_t since_last_checkpoint_ms(
        const product::Uuid& profile_id, const std::uint64_t now) const;

    ServiceCore& core_;
    IServiceClock& clock_;
    IServiceLogger& logger_;
    std::timed_mutex& execution_gate_;
    ServiceMaintenanceOptions options_{};
    mutable std::mutex state_mutex_;
    // Seeded on first sight of a profile with the current time.  In memory on
    // purpose, and stated: the durable record has no checkpoint timestamp, and adding
    // one would change the profile-state schema for a scheduling input.  A service
    // restart therefore starts the interval again -- which delays the first automatic
    // checkpoint after a restart by at most one interval and never brings one earlier.
    std::vector<std::pair<product::Uuid, std::uint64_t>> last_checkpoint_ms_;
    std::vector<MaintenanceDecision> last_decisions_;
};

} // namespace myrd::management
