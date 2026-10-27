#include "src/management/service_maintenance.h"

#include <algorithm>
#include <chrono>

namespace myrd::management {
namespace {

// The service's own authority is defined once, in the management layer
// (`service_caller_identity`), because two copies of "who the service is" is one copy
// too many: the first that drifts becomes a policy that silently stops being
// authorized.

// **A request identity for events that have no client request behind them.**
//
// `ServiceLogEvent` carries a non-optional request id because until T1-08 every
// event was caused by one.  A policy tick is not: the service decided on its own.
// Rather than widen the log type -- and every construction site with it -- the
// maintenance loop logs under one fixed, documented id, so "the service's own
// policy did this" is recognizable in the log without being mistaken for a client.
const product::Uuid& maintenance_request_id() {
    static const auto id = product::Uuid::parse(
        "00000000-0000-0000-0000-0000000000fe");
    return id.value();
}

// The service log carries narrow text; an error message is wide because the service
// speaks wide strings, and a maintenance line is not worth a locale conversion.
std::string narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const auto unit : text) {
        result.push_back(unit > 0 && unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return result;
}

// The handle a command addresses a profile by.
//
// It is the profile's **uuid**, not its mount target, and that is a measured
// correction: the first guest run dispatched against the mount target ("Z:") and every
// tick was refused with "The requested profile is not registered" -- the store resolves
// a target as a display name or a uuid, and a drive letter is neither.  The uuid is
// also the stable choice: a mount target can move, a profile identity cannot.
std::wstring profile_handle(const product::Uuid& profile_id) {
    const auto text = profile_id.to_string();
    return std::wstring(text.begin(), text.end());
}

} // namespace

std::string checkpoint_due_reason(const MaintenanceInput& input) {
    // A profile that is not running has nothing to checkpoint.  `starting` and
    // `stopping` are windows, not states to act in.
    if (input.state.disk.runtime != product::RuntimeState::running) return {};
    if (input.checkpoint.on_memory_pressure
        && input.memory.soft_limit_bytes != 0
        && input.resident_bytes >= input.memory.soft_limit_bytes) {
        return "memory-pressure";
    }
    if (input.checkpoint.dirty_threshold_enabled
        && input.checkpoint.dirty_threshold_bytes != 0
        && input.dirty_bytes >= input.checkpoint.dirty_threshold_bytes) {
        return "dirty-threshold";
    }
    if (input.checkpoint.interval_enabled
        && input.checkpoint.interval_seconds != 0
        && input.since_last_checkpoint_ms
            >= input.checkpoint.interval_seconds * 1000ULL) {
        return "interval";
    }
    return {};
}

std::string compaction_due_reason(const MaintenanceInput& input) {
    // Compaction moves data the disk already has, so a disk that is not running has nothing to compact from and
    // nothing to compact into.  `starting` and `stopping` are windows, not states to act in.
    if (input.state.disk.runtime != product::RuntimeState::running) return {};
    if (!input.compaction.automatic) return {};
    if (input.compaction.journal_occupancy_percent != 0
        && input.journal_occupancy_percent >= input.compaction.journal_occupancy_percent) {
        return "journal-occupancy";
    }
    if (input.compaction.reclaimable_percent != 0
        && input.reclaimable_percent >= input.compaction.reclaimable_percent) {
        return "reclaimable";
    }
    // The count threshold is a count of checkpoints, and the policy says zero means "do not use this trigger" --
    // which is why it is compared only when it is set.
    if (input.compaction.checkpoint_count_threshold != 0
        && input.checkpoints_since_compaction >= input.compaction.checkpoint_count_threshold) {
        return "checkpoint-count";
    }
    return {};
}
ServiceMaintenance::ServiceMaintenance(ServiceCore& core, IServiceClock& clock,
                                       IServiceLogger& logger,
                                       std::timed_mutex& execution_gate,
                                       ServiceMaintenanceOptions options) noexcept
    : core_(core),
      clock_(clock),
      logger_(logger),
      execution_gate_(execution_gate),
      options_(options) {}

void ServiceMaintenance::tick() { (void)evaluate(false); }

std::size_t ServiceMaintenance::checkpoint_for_suspend() {
    const auto decisions = evaluate(true);
    return static_cast<std::size_t>(
        std::count_if(decisions.begin(), decisions.end(),
                      [](const MaintenanceDecision& decision) {
                          return decision.dispatched;
                      }));
}

std::vector<MaintenanceDecision> ServiceMaintenance::last_decisions() const {
    std::scoped_lock lock(state_mutex_);
    return last_decisions_;
}

void ServiceMaintenance::record_last_checkpoint(const product::Uuid& profile_id,
                                                const std::uint64_t at_ms) {
    std::scoped_lock lock(state_mutex_);
    const auto found = std::find_if(
        last_checkpoint_ms_.begin(), last_checkpoint_ms_.end(),
        [&profile_id](const auto& entry) { return entry.first == profile_id; });
    if (found == last_checkpoint_ms_.end()) {
        last_checkpoint_ms_.emplace_back(profile_id, at_ms);
    } else {
        found->second = at_ms;
    }
}

std::uint64_t ServiceMaintenance::since_last_checkpoint_ms(
    const product::Uuid& profile_id, const std::uint64_t now) const {
    std::scoped_lock lock(state_mutex_);
    const auto found = std::find_if(
        last_checkpoint_ms_.begin(), last_checkpoint_ms_.end(),
        [&profile_id](const auto& entry) { return entry.first == profile_id; });
    if (found == last_checkpoint_ms_.end()) return 0;
    return now >= found->second ? now - found->second : 0;
}

void ServiceMaintenance::seed_last_checkpoint_if_absent(
    const product::Uuid& profile_id, const std::uint64_t at_ms) {
    std::scoped_lock lock(state_mutex_);
    const auto found = std::find_if(
        last_checkpoint_ms_.begin(), last_checkpoint_ms_.end(),
        [&profile_id](const auto& entry) { return entry.first == profile_id; });
    if (found == last_checkpoint_ms_.end()) {
        last_checkpoint_ms_.emplace_back(profile_id, at_ms);
    }
}

std::vector<MaintenanceDecision> ServiceMaintenance::evaluate(
    const bool for_suspend) {
    // The gate is *tried*, because a maintenance tick is never more important than
    // the command or operation already inside the core.  A skipped tick is the
    // honest answer; a queued one would act on state that has moved on.
    std::unique_lock gate(execution_gate_, std::defer_lock);
    if (!gate.try_lock_for(
            std::chrono::milliseconds(options_.gate_timeout_milliseconds))) {
        return {};
    }

    ServiceCommandRequest list_request{};
    list_request.command = control::DiskListCommand{};
    list_request.caller = service_caller_identity();
    auto listed = core_.dispatch(list_request);
    if (!listed.ok()) {
        logger_.write(ServiceLogEvent{
            ServiceLogKind::operation_failed,
            maintenance_request_id(),
            std::nullopt,
            std::nullopt,
            "maintenance",
            "list",
            "maintenance: listing profiles failed: "
                + narrow(listed.error().message)});
        return {};
    }

    const auto now = clock_.monotonic_time_ms();
    std::vector<Candidate> candidates;
    std::vector<Candidate> compactions;
    for (const auto& snapshot : listed.value().profiles) {
        const auto& profile = snapshot.record.profile;
        if (!snapshot.runtime_observation.has_value()
            || !snapshot.runtime_observation->worker_alive) {
            // Seeding here rather than in the dispatch branch: the interval must start
            // when the service begins watching a running profile, not when it first
            // decides to checkpoint it.
        }
        const auto observation = snapshot.runtime_observation;

        MaintenanceInput input{};
        input.state = snapshot.state;
        input.checkpoint = profile.checkpoint;
        input.memory = profile.memory;
        input.resident_bytes = observation.has_value() ? observation->resident_bytes : 0;
        input.dirty_bytes = observation.has_value() ? observation->dirty_bytes : 0;
        // **Seed the interval clock the first time a running profile is seen.**
        //
        // Without this the profile is never due: `since` starts at zero, and only a
        // checkpoint can move it -- which is a deadlock.  Measured on the guest: a
        // 30-second interval ran for 90 seconds and the generation never moved
        // (`generation stayed 2`).  Seeding here means the interval counts from when
        // the service began watching the profile, never from a timestamp it never had.
        if (input.state.disk.runtime == product::RuntimeState::running) {
            seed_last_checkpoint_if_absent(profile.profile_uuid, now);
        }
        input.since_last_checkpoint_ms =
            since_last_checkpoint_ms(profile.profile_uuid, now);

        if (for_suspend) {
            // Suspend is not a threshold: the machine is going away, so the policy
            // that decides is the one that says whether we try at all.
            if (profile.checkpoint.on_suspend != profile::BestEffortPolicy::best_effort) {
                continue;
            }
            if (input.state.disk.runtime != product::RuntimeState::running) continue;
            candidates.push_back({profile.profile_uuid, profile_handle(profile.profile_uuid),
                                  "suspend"});
            continue;
        }
        const auto reason = checkpoint_due_reason(input);
        if (!reason.empty()) {
            candidates.push_back({profile.profile_uuid,
                                  profile_handle(profile.profile_uuid), reason});
            continue;
        }
        // **Compaction's half, from the same snapshot.**  The policy is the profile's and the count is the
        // durable one, so the checkpoint-count trigger needs nothing new.  The two percentage triggers stay at
        // zero because the worker does not report occupancy yet, and the policy reads zero as "trigger off" --
        // nothing is guessed in the meantime.
        if (for_suspend) continue;
        MaintenanceInput compaction_input = input;
        compaction_input.compaction = profile.compaction;
        compaction_input.checkpoints_since_compaction = static_cast<std::uint32_t>(
            (std::min)(snapshot.compaction.checkpoints_since_compaction,
                       static_cast<std::uint64_t>(0xFFFFFFFFULL)));
        // The durable phase is the authority on "a compaction is already in flight", not the policy.
        if (snapshot.compaction.phase == CompactionDurablePhase::stable) {
            const auto compaction_reason = compaction_due_reason(compaction_input);
            if (!compaction_reason.empty()) {
                compactions.push_back({profile.profile_uuid,
                                       profile_handle(profile.profile_uuid),
                                       compaction_reason});
            }
        }
    }


    std::vector<MaintenanceDecision> decisions;
    decisions.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        ServiceCommandRequest checkpoint_request{};
        checkpoint_request.command =
            control::DiskCheckpointCommand{candidate.target};
        checkpoint_request.caller = service_caller_identity();
        // Admitted, not awaited: the scheduler is the one executor, and a policy that
        // waited for its own work would hold the gate it needs to release.
        checkpoint_request.no_wait = true;
        auto accepted = core_.dispatch(checkpoint_request);
        MaintenanceDecision decision{candidate.profile_id, candidate.reason,
                                     accepted.ok()};
        logger_.write(ServiceLogEvent{
            accepted.ok() ? ServiceLogKind::operation_queued
                          : ServiceLogKind::operation_failed,
            accepted.ok() ? accepted.value().request_id : maintenance_request_id(),
            accepted.ok() ? accepted.value().operation_id : std::nullopt,
            candidate.profile_id,
            "disk.checkpoint",
            "maintenance",
            accepted.ok()
                ? "maintenance: checkpoint dispatched (" + candidate.reason + ")"
                : "maintenance: checkpoint refused (" + candidate.reason + "): "
                      + narrow(accepted.error().message)});
        if (accepted.ok()) {
            record_last_checkpoint(candidate.profile_id, now);
        }
        decisions.push_back(std::move(decision));
    }
    // **The compaction half of the same tick.**  Same shape as the checkpoint dispatch above, and for the same
    // reason: admission is durable and execution belongs to the scheduler, so a policy that awaited its own work
    // would hold the gate it needs to release.
    for (const auto& candidate : compactions) {
        ServiceCommandRequest compact_request{};
        compact_request.command = control::DiskCompactCommand{candidate.target};
        compact_request.caller = service_caller_identity();
        compact_request.no_wait = true;
        auto accepted = core_.dispatch(compact_request);
        logger_.write(ServiceLogEvent{
            accepted.ok() ? ServiceLogKind::operation_queued
                          : ServiceLogKind::operation_failed,
            accepted.ok() ? accepted.value().request_id : maintenance_request_id(),
            accepted.ok() ? accepted.value().operation_id : std::nullopt,
            candidate.profile_id,
            "disk.compact",
            "maintenance",
            accepted.ok()
                ? "maintenance: compaction dispatched (" + candidate.reason + ")"
                : "maintenance: compaction refused (" + candidate.reason + "): "
                      + narrow(accepted.error().message)});
        decisions.push_back(MaintenanceDecision{candidate.profile_id,
                                               candidate.reason, accepted.ok()});
    }
    std::scoped_lock lock(state_mutex_);
    last_decisions_ = decisions;
    return decisions;
}

} // namespace myrd::management
