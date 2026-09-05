#include "src/management/state_machine.h"

#include <algorithm>
#include <array>
#include <utility>

namespace myrd::management {
namespace {

using C = ManagementCommand;
using A = CommandAccess;
using P = PresenceRequirement;
using L = LifecycleRequirement;
using R = RuntimeRequirement;
using X = ActivityRequirement;
using S = PersistenceRequirement;
using H = HealthRequirement;
using E = AdmissionEffect;

constexpr std::array<TransitionContract, kManagementCommandCount> kContracts{{
    {C::create_and_start, A::exclusive_mutation, P::absent, L::any,
     R::any, X::any, S::any, H::any, E::create_starting_provisioning},
    {C::create_stopped, A::exclusive_mutation, P::absent, L::any,
     R::any, X::any, S::any, H::any, E::create_stopped_provisioning},
    {C::import_profile, A::exclusive_mutation, P::absent, L::any,
     R::any, X::any, S::any, H::any, E::import_stopped_ready},
    {C::start, A::exclusive_mutation, P::present, L::any,
     R::stopped_or_running, X::idle, S::not_commit_indeterminate,
     H::operational, E::start_by_lifecycle},
    {C::stop, A::exclusive_mutation, P::present, L::any,
     R::stopped_or_running, X::idle, S::compactable,
     H::nonfatal, E::begin_stopping},
    {C::force_stop, A::exclusive_mutation, P::present, L::any,
     R::stopped_running_or_orphaned, X::idle,
     S::not_commit_indeterminate, H::nonfatal, E::begin_stopping},
    {C::checkpoint, A::exclusive_mutation, P::present, L::ready,
     R::running, X::idle, S::not_commit_indeterminate,
     H::operational, E::checkpoint_if_needed},
    {C::compact, A::exclusive_mutation, P::present, L::ready,
     R::running, X::idle, S::compactable,
     H::operational, E::checkpoint_then_compact},
    {C::config_set_live, A::exclusive_mutation, P::present, L::any,
     R::stopped_or_running, X::idle, S::not_commit_indeterminate,
     H::nonfatal, E::preserve},
    {C::config_set_stopped, A::exclusive_mutation, P::present, L::any,
     R::stopped, X::idle, S::not_commit_indeterminate,
     H::nonfatal, E::preserve},
    {C::repair_profile, A::exclusive_mutation, P::present, L::any,
     R::stopped, X::idle, S::not_commit_indeterminate,
     H::needs_attention, E::begin_recovery},
    {C::rebind, A::exclusive_mutation, P::present, L::ready,
     R::stopped, X::idle, S::not_commit_indeterminate,
     H::nonfatal, E::begin_recovery},
    {C::forget, A::exclusive_mutation, P::present, L::any,
     R::stopped, X::idle, S::not_commit_indeterminate,
     H::nonfatal, E::remove_profile},
    {C::resolve_commit, A::exclusive_mutation, P::present, L::any,
     R::stopped_running_or_orphaned, X::idle_or_resolving_commit,
     S::commit_indeterminate, H::nonfatal, E::begin_commit_resolution},
    {C::status_read, A::shared_read, P::any, L::any,
     R::any, X::any, S::any, H::any, E::preserve},
    {C::config_read, A::shared_read, P::present, L::any,
     R::any, X::any, S::any, H::any, E::preserve},
    {C::diagnostics_read, A::shared_read, P::any, L::any,
     R::any, X::any, S::any, H::any, E::preserve},
    {C::operation_read, A::shared_read, P::any, L::any,
     R::any, X::any, S::any, H::any, E::preserve},
    {C::operation_cancel, A::active_operation_control, P::any, L::any,
     R::any, X::any, S::not_commit_indeterminate,
     H::nonfatal, E::preserve},
}};

static_assert(kContracts.size() == kManagementCommandCount);

} // namespace

std::span<const TransitionContract> transition_contracts() noexcept {
    return kContracts;
}

ProfileState absent_profile_state() noexcept {
    return ProfileState{
        std::nullopt,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy}};
}

ProfileState registered_profile_state(
    profile::ProfileLifecycle lifecycle,
    product::DiskState disk) noexcept {
    return ProfileState{lifecycle, disk};
}

bool is_valid(const ProfileState& state) noexcept {
    if (!product::is_valid(state.disk)) return false;
    if (!state.lifecycle.has_value()) {
        return state == absent_profile_state();
    }
    return state.lifecycle.value() == profile::ProfileLifecycle::provisioning
        || state.lifecycle.value() == profile::ProfileLifecycle::ready;
}

namespace {

bool runtime_matches(
    product::RuntimeState actual, RuntimeRequirement required) noexcept {
    switch (required) {
    case RuntimeRequirement::any: return true;
    case RuntimeRequirement::stopped:
        return actual == product::RuntimeState::stopped;
    case RuntimeRequirement::running:
        return actual == product::RuntimeState::running;
    case RuntimeRequirement::stopped_or_running:
        return actual == product::RuntimeState::stopped
            || actual == product::RuntimeState::running;
    case RuntimeRequirement::stopped_running_or_orphaned:
        return actual == product::RuntimeState::stopped
            || actual == product::RuntimeState::running
            || actual == product::RuntimeState::orphaned;
    }
    return false;
}

bool activity_matches(
    product::ActivityState actual, ActivityRequirement required) noexcept {
    switch (required) {
    case ActivityRequirement::any: return true;
    case ActivityRequirement::idle:
        return actual == product::ActivityState::idle;
    case ActivityRequirement::idle_or_resolving_commit:
        return actual == product::ActivityState::idle
            || actual == product::ActivityState::resolving_commit;
    }
    return false;
}

TransitionDisposition health_disposition(
    product::HealthState actual, HealthRequirement required) noexcept {
    if (required == HealthRequirement::any) {
        return TransitionDisposition::allowed;
    }
    if (actual == product::HealthState::fatal) {
        return TransitionDisposition::fatal;
    }
    if (required == HealthRequirement::operational
        && actual == product::HealthState::needs_attention) {
        return TransitionDisposition::needs_attention;
    }
    if (required == HealthRequirement::needs_attention
        && actual != product::HealthState::needs_attention) {
        return TransitionDisposition::state_conflict;
    }
    return TransitionDisposition::allowed;
}

TransitionDisposition persistence_disposition(
    product::PersistenceState actual,
    PersistenceRequirement required) noexcept {
    if (required == PersistenceRequirement::any) {
        return TransitionDisposition::allowed;
    }
    if (actual == product::PersistenceState::commit_indeterminate) {
        return required == PersistenceRequirement::commit_indeterminate
            ? TransitionDisposition::allowed
            : TransitionDisposition::commit_indeterminate;
    }
    if (required == PersistenceRequirement::commit_indeterminate) {
        return TransitionDisposition::state_conflict;
    }
    if (required == PersistenceRequirement::compactable
        && actual == product::PersistenceState::persistence_blocked) {
        return TransitionDisposition::persistence_blocked;
    }
    return TransitionDisposition::allowed;
}

void apply_admission(ActionPlan& plan) noexcept {
    using profile::ProfileLifecycle;
    using product::ActivityState;
    using product::HealthState;
    using product::PersistenceState;
    using product::RuntimeState;
    switch (plan.admission) {
    case AdmissionEffect::preserve:
    case AdmissionEffect::remove_profile:
        return;
    case AdmissionEffect::create_starting_provisioning:
        plan.admitted = registered_profile_state(
            ProfileLifecycle::provisioning,
            product::DiskState{
                RuntimeState::starting, ActivityState::provisioning,
                PersistenceState::clean, HealthState::healthy});
        return;
    case AdmissionEffect::create_stopped_provisioning:
        plan.admitted = registered_profile_state(
            ProfileLifecycle::provisioning,
            product::DiskState{
                RuntimeState::stopped, ActivityState::idle,
                PersistenceState::clean, HealthState::healthy});
        return;
    case AdmissionEffect::import_stopped_ready:
        plan.admitted = registered_profile_state(
            ProfileLifecycle::ready,
            product::DiskState{
                RuntimeState::stopped, ActivityState::idle,
                PersistenceState::clean, HealthState::healthy});
        return;
    case AdmissionEffect::start_by_lifecycle:
        plan.admitted.disk.runtime = RuntimeState::starting;
        plan.admitted.disk.activity =
            plan.admitted.lifecycle == ProfileLifecycle::provisioning
            ? ActivityState::provisioning : ActivityState::recovering;
        return;
    case AdmissionEffect::begin_stopping:
        plan.admitted.disk.runtime = RuntimeState::stopping;
        plan.admitted.disk.activity = ActivityState::idle;
        return;
    case AdmissionEffect::checkpoint_if_needed:
        plan.admitted.disk.activity = ActivityState::checkpointing;
        return;
    case AdmissionEffect::checkpoint_then_compact:
        plan.admitted.disk.activity =
            plan.before.disk.persistence == PersistenceState::dirty
            ? ActivityState::checkpointing : ActivityState::compacting;
        return;
    case AdmissionEffect::begin_recovery:
        plan.admitted.disk.activity = ActivityState::recovering;
        return;
    case AdmissionEffect::begin_commit_resolution:
        plan.admitted.disk.activity = ActivityState::resolving_commit;
        return;
    }
}

void append_action(ActionPlan& plan, ActionKind action) noexcept {
    if (plan.action_count < plan.actions.size()) {
        plan.actions[plan.action_count++] = action;
    }
}

void append_start_actions(ActionPlan& plan, bool provisioning) noexcept {
    append_action(plan, ActionKind::create_runtime_and_worker);
    append_action(plan, ActionKind::recover_bundle);
    append_action(plan, ActionKind::start_proxy);
    append_action(plan, ActionKind::create_exact_device);
    append_action(plan, ActionKind::locate_exact_physical_drive);
    if (provisioning) {
        append_action(plan, ActionKind::prove_new_uninitialized);
        append_action(plan, ActionKind::provision_gpt_ntfs);
        append_action(plan, ActionKind::record_volume_identity);
        append_action(plan, ActionKind::flush_exact_volume);
        append_action(plan, ActionKind::checkpoint);
        append_action(plan, ActionKind::publish_ready_profile);
    } else {
        append_action(plan, ActionKind::verify_existing_volume_identity);
    }
    append_action(plan, ActionKind::assign_exact_mount);
    append_action(plan, ActionKind::readiness_io);
}

void fill_actions(ActionPlan& plan) noexcept {
    switch (plan.command) {
    case ManagementCommand::create_and_start:
        append_action(plan, ActionKind::create_and_verify_bundle);
        append_action(plan, ActionKind::publish_provisioning_profile);
        append_start_actions(plan, true);
        return;
    case ManagementCommand::create_stopped:
        append_action(plan, ActionKind::create_and_verify_bundle);
        append_action(plan, ActionKind::publish_provisioning_profile);
        return;
    case ManagementCommand::import_profile:
        append_action(plan, ActionKind::inspect_bundle_read_only);
        append_action(plan, ActionKind::publish_imported_ready_profile);
        return;
    case ManagementCommand::start:
        append_start_actions(
            plan,
            plan.before.lifecycle == profile::ProfileLifecycle::provisioning);
        return;
    case ManagementCommand::stop:
        append_action(plan, ActionKind::quiesce_mutations);
        append_action(plan, ActionKind::flush_exact_volume);
        append_action(plan, ActionKind::checkpoint_if_required);
        append_action(plan, ActionKind::remove_mount);
        append_action(plan, ActionKind::remove_exact_device);
        append_action(plan, ActionKind::stop_runtime_and_release_lock);
        return;
    case ManagementCommand::force_stop:
        append_action(plan, ActionKind::remove_mount);
        append_action(plan, ActionKind::remove_exact_device);
        append_action(plan, ActionKind::stop_runtime_and_release_lock);
        return;
    case ManagementCommand::checkpoint:
        append_action(plan, ActionKind::flush_exact_volume);
        append_action(plan, ActionKind::checkpoint);
        return;
    case ManagementCommand::compact:
        if (plan.before.disk.persistence == product::PersistenceState::dirty) {
            append_action(plan, ActionKind::flush_exact_volume);
            append_action(plan, ActionKind::checkpoint);
        }
        append_action(plan, ActionKind::compact);
        return;
    case ManagementCommand::config_set_live:
        append_action(plan, ActionKind::publish_live_config);
        return;
    case ManagementCommand::config_set_stopped:
        append_action(plan, ActionKind::publish_stopped_config);
        return;
    case ManagementCommand::repair_profile:
        append_action(plan, ActionKind::repair_profile_from_previous);
        return;
    case ManagementCommand::rebind:
        append_action(plan, ActionKind::verify_rebind_bundle);
        append_action(plan, ActionKind::publish_rebind);
        return;
    case ManagementCommand::forget:
        append_action(plan, ActionKind::forget_metadata);
        return;
    case ManagementCommand::resolve_commit:
        append_action(plan, ActionKind::resolve_commit);
        return;
    case ManagementCommand::status_read:
        append_action(plan, ActionKind::read_status);
        return;
    case ManagementCommand::config_read:
        append_action(plan, ActionKind::read_config);
        return;
    case ManagementCommand::diagnostics_read:
        append_action(plan, ActionKind::collect_diagnostics);
        return;
    case ManagementCommand::operation_read:
        append_action(plan, ActionKind::read_operation);
        return;
    case ManagementCommand::operation_cancel:
        append_action(plan, ActionKind::request_operation_cancel);
        return;
    }
}

} // namespace

ActionPlan plan_command(
    const ProfileState& state, ManagementCommand command) noexcept {
    const auto command_index = static_cast<std::size_t>(command);
    ActionPlan plan{
        command,
        CommandAccess::shared_read,
        TransitionDisposition::invalid_state,
        state,
        state,
        AdmissionEffect::preserve};
    if (!is_valid(state) || command_index >= kContracts.size()) return plan;

    const auto& contract = kContracts[command_index];
    plan.access = contract.access;
    plan.admission = contract.admission;
    const bool present = state.lifecycle.has_value();
    if (contract.presence == PresenceRequirement::present && !present) {
        plan.disposition = TransitionDisposition::profile_not_found;
        return plan;
    }
    if (contract.presence == PresenceRequirement::absent && present) {
        plan.disposition = TransitionDisposition::profile_already_exists;
        return plan;
    }
    if (contract.access == CommandAccess::shared_read) {
        plan.disposition = TransitionDisposition::allowed;
        fill_actions(plan);
        return plan;
    }

    const auto persistence = persistence_disposition(
        state.disk.persistence, contract.persistence);
    if (persistence != TransitionDisposition::allowed) {
        plan.disposition = persistence;
        return plan;
    }
    const auto health = health_disposition(state.disk.health, contract.health);
    if (health != TransitionDisposition::allowed) {
        plan.disposition = health;
        return plan;
    }
    if (present && contract.lifecycle == LifecycleRequirement::ready
        && state.lifecycle != profile::ProfileLifecycle::ready) {
        plan.disposition = TransitionDisposition::state_conflict;
        return plan;
    }
    if (!activity_matches(state.disk.activity, contract.activity)
        || !runtime_matches(state.disk.runtime, contract.runtime)) {
        plan.disposition = TransitionDisposition::state_conflict;
        return plan;
    }

    if (command == ManagementCommand::start
        && state.disk.runtime == product::RuntimeState::running) {
        plan.disposition = TransitionDisposition::already_running;
        return plan;
    }
    if ((command == ManagementCommand::stop
         || command == ManagementCommand::force_stop)
        && state.disk.runtime == product::RuntimeState::stopped) {
        plan.disposition = TransitionDisposition::already_stopped;
        return plan;
    }
    if (command == ManagementCommand::checkpoint
        && state.disk.persistence == product::PersistenceState::clean) {
        plan.disposition = TransitionDisposition::no_changes;
        return plan;
    }

    plan.disposition = TransitionDisposition::allowed;
    fill_actions(plan);
    apply_admission(plan);
    return plan;
}

bool is_valid(const ActionPlan& plan) noexcept {
    const auto command_index = static_cast<std::size_t>(plan.command);
    if (command_index >= kContracts.size() || !is_valid(plan.before)) {
        return false;
    }

    // ActionPlan is a value type so it can cross deterministic orchestration
    // boundaries. Treat every externally supplied value as untrusted and
    // accept only the unique canonical projection produced by the planner.
    const auto canonical = plan_command(plan.before, plan.command);
    return plan.command == canonical.command
        && plan.access == canonical.access
        && plan.disposition == canonical.disposition
        && plan.before == canonical.before
        && plan.admitted == canonical.admitted
        && plan.admission == canonical.admission
        && plan.actions == canonical.actions
        && plan.action_count == canonical.action_count;
}

namespace {

CompletionDecision invalid_completion(
    const ActionPlan& plan, CompletionDisposition disposition) noexcept {
    return CompletionDecision{disposition, plan.before};
}

product::HealthState degraded_health(product::HealthState current) noexcept {
    return current == product::HealthState::healthy
        ? product::HealthState::degraded : current;
}

void finish_idle(ProfileState& state) noexcept {
    state.disk.activity = product::ActivityState::idle;
}

void apply_commit_indeterminate(ProfileState& state) noexcept {
    state.disk.activity = product::ActivityState::resolving_commit;
    state.disk.persistence = product::PersistenceState::commit_indeterminate;
    state.disk.health = product::HealthState::needs_attention;
}

void apply_reconciliation(
    ProfileState& state, product::PersistenceState persistence) noexcept {
    state.disk.activity = product::ActivityState::resolving_commit;
    state.disk.persistence = persistence;
    state.disk.health = degraded_health(state.disk.health);
}

CompletionDecision complete_checkpoint_like(
    const ActionPlan& plan,
    CompletionOutcome outcome,
    bool compaction) noexcept {
    auto after = plan.before;
    switch (outcome) {
    case CompletionOutcome::succeeded_clean:
        finish_idle(after);
        after.disk.persistence = product::PersistenceState::clean;
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::succeeded_dirty:
        finish_idle(after);
        after.disk.persistence = product::PersistenceState::dirty;
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::transient_failure:
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::compaction_deferred:
        return compaction
            ? CompletionDecision{CompletionDisposition::applied, after}
            : invalid_completion(
                plan, CompletionDisposition::invalid_outcome);
    case CompletionOutcome::persistence_blocked:
        finish_idle(after);
        after.disk.persistence =
            product::PersistenceState::persistence_blocked;
        after.disk.health = degraded_health(after.disk.health);
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::committed_needs_reconciliation_clean:
        apply_reconciliation(after, product::PersistenceState::clean);
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::committed_needs_reconciliation_dirty:
        apply_reconciliation(after, product::PersistenceState::dirty);
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::commit_indeterminate:
        apply_commit_indeterminate(after);
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::needs_attention_failure:
        finish_idle(after);
        after.disk.health = product::HealthState::needs_attention;
        return {CompletionDisposition::applied, after};
    case CompletionOutcome::resolution_unresolved:
        return invalid_completion(plan, CompletionDisposition::invalid_outcome);
    }
    return invalid_completion(plan, CompletionDisposition::invalid_outcome);
}

bool metadata_publication_can_be_indeterminate(
    ManagementCommand command) noexcept {
    switch (command) {
    case ManagementCommand::create_stopped:
    case ManagementCommand::import_profile:
    case ManagementCommand::config_set_live:
    case ManagementCommand::config_set_stopped:
    case ManagementCommand::repair_profile:
    case ManagementCommand::rebind:
    case ManagementCommand::forget:
        return true;
    default:
        return false;
    }
}

} // namespace

CompletionDecision complete_command(
    const ActionPlan& plan, CompletionOutcome outcome) noexcept {
    if (!is_valid(plan)
        || plan.disposition != TransitionDisposition::allowed
        || plan.action_count == 0) {
        return invalid_completion(plan, CompletionDisposition::invalid_plan);
    }

    auto after = plan.admitted;
    if (outcome == CompletionOutcome::commit_indeterminate
        && metadata_publication_can_be_indeterminate(plan.command)) {
        apply_commit_indeterminate(after);
        return {CompletionDisposition::applied, after};
    }
    switch (plan.command) {
    case ManagementCommand::create_and_start:
        if (outcome == CompletionOutcome::succeeded_clean) {
            after.lifecycle = profile::ProfileLifecycle::ready;
            after.disk = product::DiskState{
                product::RuntimeState::running,
                product::ActivityState::idle,
                product::PersistenceState::clean,
                product::HealthState::healthy};
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after.lifecycle = profile::ProfileLifecycle::provisioning;
            after.disk = product::DiskState{
                product::RuntimeState::stopped,
                product::ActivityState::idle,
                product::PersistenceState::clean,
                product::HealthState::needs_attention};
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::commit_indeterminate) {
            apply_commit_indeterminate(after);
            return {CompletionDisposition::applied, after};
        }
        if (outcome
                == CompletionOutcome::committed_needs_reconciliation_clean
            || outcome
                == CompletionOutcome::committed_needs_reconciliation_dirty) {
            apply_reconciliation(
                after,
                outcome
                        == CompletionOutcome::committed_needs_reconciliation_clean
                    ? product::PersistenceState::clean
                    : product::PersistenceState::dirty);
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::create_stopped:
        if (outcome == CompletionOutcome::succeeded_clean) {
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after.disk.health = product::HealthState::needs_attention;
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::import_profile:
        if (outcome == CompletionOutcome::succeeded_clean) {
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            return {CompletionDisposition::applied, plan.before};
        }
        break;
    case ManagementCommand::start:
        if (outcome == CompletionOutcome::succeeded_clean) {
            const bool completed_provisioning =
                plan.before.lifecycle
                == profile::ProfileLifecycle::provisioning;
            after.lifecycle = profile::ProfileLifecycle::ready;
            after.disk.runtime = product::RuntimeState::running;
            after.disk.activity = product::ActivityState::idle;
            if (completed_provisioning) {
                after.disk.persistence = product::PersistenceState::clean;
                after.disk.health = product::HealthState::healthy;
            }
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure) {
            after = plan.before;
            after.disk.runtime = product::RuntimeState::stopped;
            after.disk.activity = product::ActivityState::idle;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::needs_attention_failure) {
            after = plan.before;
            after.disk.runtime = product::RuntimeState::stopped;
            after.disk.activity = product::ActivityState::idle;
            after.disk.health = product::HealthState::needs_attention;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::commit_indeterminate) {
            apply_commit_indeterminate(after);
            return {CompletionDisposition::applied, after};
        }
        if (outcome
                == CompletionOutcome::committed_needs_reconciliation_clean
            || outcome
                == CompletionOutcome::committed_needs_reconciliation_dirty) {
            apply_reconciliation(
                after,
                outcome
                        == CompletionOutcome::committed_needs_reconciliation_clean
                    ? product::PersistenceState::clean
                    : product::PersistenceState::dirty);
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::stop:
        if (outcome == CompletionOutcome::succeeded_clean) {
            after.disk.runtime = product::RuntimeState::stopped;
            after.disk.activity = product::ActivityState::idle;
            after.disk.persistence = product::PersistenceState::clean;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure) {
            return {CompletionDisposition::applied, plan.before};
        }
        if (outcome == CompletionOutcome::persistence_blocked) {
            after = plan.before;
            after.disk.persistence =
                product::PersistenceState::persistence_blocked;
            after.disk.health = degraded_health(after.disk.health);
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::commit_indeterminate) {
            after = plan.before;
            apply_commit_indeterminate(after);
            return {CompletionDisposition::applied, after};
        }
        if (outcome
                == CompletionOutcome::committed_needs_reconciliation_clean
            || outcome
                == CompletionOutcome::committed_needs_reconciliation_dirty) {
            after = plan.before;
            apply_reconciliation(
                after,
                outcome
                        == CompletionOutcome::committed_needs_reconciliation_clean
                    ? product::PersistenceState::clean
                    : product::PersistenceState::dirty);
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::force_stop:
        if (outcome == CompletionOutcome::succeeded_clean) {
            after.disk.runtime = product::RuntimeState::stopped;
            after.disk.activity = product::ActivityState::idle;
            after.disk.persistence = product::PersistenceState::clean;
            if (plan.before.disk.persistence
                != product::PersistenceState::clean) {
                after.disk.health = degraded_health(after.disk.health);
            }
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after = plan.before;
            if (outcome == CompletionOutcome::needs_attention_failure) {
                after.disk.health = product::HealthState::needs_attention;
            }
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::checkpoint:
        return complete_checkpoint_like(plan, outcome, false);
    case ManagementCommand::compact:
        return complete_checkpoint_like(plan, outcome, true);
    case ManagementCommand::config_set_live:
    case ManagementCommand::config_set_stopped:
        if (outcome == CompletionOutcome::succeeded_clean
            || outcome == CompletionOutcome::transient_failure) {
            return {CompletionDisposition::applied, plan.before};
        }
        if (outcome == CompletionOutcome::needs_attention_failure) {
            after = plan.before;
            after.disk.health = product::HealthState::needs_attention;
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::repair_profile:
        if (outcome == CompletionOutcome::succeeded_clean) {
            after.disk.activity = product::ActivityState::idle;
            after.disk.persistence = product::PersistenceState::clean;
            after.disk.health = product::HealthState::healthy;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after = plan.before;
            after.disk.health = product::HealthState::needs_attention;
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::rebind:
        if (outcome == CompletionOutcome::succeeded_clean) {
            after.disk.activity = product::ActivityState::idle;
            after.disk.health = product::HealthState::healthy;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after = plan.before;
            if (outcome == CompletionOutcome::needs_attention_failure) {
                after.disk.health = product::HealthState::needs_attention;
            }
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::forget:
        if (outcome == CompletionOutcome::succeeded_clean) {
            return {
                CompletionDisposition::applied, absent_profile_state()};
        }
        if (outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            return {CompletionDisposition::applied, plan.before};
        }
        break;
    case ManagementCommand::resolve_commit:
        if (outcome == CompletionOutcome::succeeded_clean
            || outcome == CompletionOutcome::succeeded_dirty) {
            after.disk.activity = product::ActivityState::idle;
            after.disk.persistence =
                outcome == CompletionOutcome::succeeded_clean
                ? product::PersistenceState::clean
                : product::PersistenceState::dirty;
            after.disk.health = product::HealthState::healthy;
            return {CompletionDisposition::applied, after};
        }
        if (outcome == CompletionOutcome::resolution_unresolved
            || outcome == CompletionOutcome::transient_failure
            || outcome == CompletionOutcome::needs_attention_failure) {
            after.disk.activity = product::ActivityState::idle;
            after.disk.persistence =
                product::PersistenceState::commit_indeterminate;
            after.disk.health = product::HealthState::needs_attention;
            return {CompletionDisposition::applied, after};
        }
        break;
    case ManagementCommand::status_read:
    case ManagementCommand::config_read:
    case ManagementCommand::diagnostics_read:
    case ManagementCommand::operation_read:
    case ManagementCommand::operation_cancel:
        break;
    }
    return invalid_completion(plan, CompletionDisposition::invalid_outcome);
}

OperationGateState idle_operation_gate() noexcept {
    return OperationGateState{
        std::nullopt, ManagementCommand::status_read, false};
}

namespace {

bool zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

bool valid_gate(const OperationGateState& gate) noexcept {
    if (!gate.active_operation_id.has_value()) {
        return gate == idle_operation_gate();
    }
    const auto command_index = static_cast<std::size_t>(gate.active_command);
    return !zero_uuid(gate.active_operation_id.value())
        && command_index < kContracts.size()
        && kContracts[command_index].access
            == CommandAccess::exclusive_mutation;
}

GateDecision rejected_gate(
    const OperationGateState& gate, GateDisposition disposition) noexcept {
    return GateDecision{disposition, gate, gate};
}

} // namespace

GateDecision plan_gate_access(
    const OperationGateState& gate,
    const ActionPlan& plan,
    const product::Uuid& operation_id) noexcept {
    if (!valid_gate(gate) || !is_valid(plan) || !plan.accepted()) {
        return rejected_gate(gate, GateDisposition::invalid_request);
    }
    if (plan.access == CommandAccess::shared_read) {
        return GateDecision{GateDisposition::allowed, gate, gate};
    }
    if (zero_uuid(operation_id)) {
        return rejected_gate(gate, GateDisposition::invalid_request);
    }
    if (plan.access == CommandAccess::active_operation_control) {
        if (!gate.active_operation_id.has_value()) {
            return rejected_gate(gate, GateDisposition::no_active_operation);
        }
        if (gate.active_operation_id.value() != operation_id) {
            return rejected_gate(gate, GateDisposition::target_mismatch);
        }
        if (!gate.cancellable) {
            return rejected_gate(gate, GateDisposition::cancellation_too_late);
        }
        return GateDecision{GateDisposition::allowed, gate, gate};
    }
    if (gate.active_operation_id.has_value()) {
        return rejected_gate(gate, GateDisposition::operation_conflict);
    }
    if (plan.disposition != TransitionDisposition::allowed) {
        return GateDecision{GateDisposition::allowed, gate, gate};
    }
    auto after = gate;
    after.active_operation_id = operation_id;
    after.active_command = plan.command;
    after.cancellable = true;
    return GateDecision{GateDisposition::allowed, gate, std::move(after)};
}

GateDecision mark_operation_noncancellable(
    const OperationGateState& gate,
    const product::Uuid& operation_id) noexcept {
    if (!valid_gate(gate) || zero_uuid(operation_id)) {
        return rejected_gate(gate, GateDisposition::invalid_request);
    }
    if (!gate.active_operation_id.has_value()) {
        return rejected_gate(gate, GateDisposition::no_active_operation);
    }
    if (gate.active_operation_id.value() != operation_id) {
        return rejected_gate(gate, GateDisposition::target_mismatch);
    }
    auto after = gate;
    after.cancellable = false;
    return GateDecision{GateDisposition::allowed, gate, std::move(after)};
}

GateDecision release_operation_gate(
    const OperationGateState& gate,
    const product::Uuid& operation_id) noexcept {
    if (!valid_gate(gate) || zero_uuid(operation_id)) {
        return rejected_gate(gate, GateDisposition::invalid_request);
    }
    if (!gate.active_operation_id.has_value()) {
        return rejected_gate(gate, GateDisposition::no_active_operation);
    }
    if (gate.active_operation_id.value() != operation_id) {
        return rejected_gate(gate, GateDisposition::target_mismatch);
    }
    return GateDecision{
        GateDisposition::allowed, gate, idle_operation_gate()};
}

} // namespace myrd::management
