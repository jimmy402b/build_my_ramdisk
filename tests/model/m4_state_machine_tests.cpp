#include "tests/support/test_framework.h"

#include "src/management/state_machine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using C = myrd::management::ManagementCommand;
using A = myrd::management::CommandAccess;
using P = myrd::management::PresenceRequirement;
using L = myrd::management::LifecycleRequirement;
using R = myrd::management::RuntimeRequirement;
using X = myrd::management::ActivityRequirement;
using S = myrd::management::PersistenceRequirement;
using H = myrd::management::HealthRequirement;
using E = myrd::management::AdmissionEffect;
using Contract = myrd::management::TransitionContract;

constexpr std::array<Contract, myrd::management::kManagementCommandCount>
    kExpectedContracts{{
        {C::create_and_start, A::exclusive_mutation, P::absent, L::any,
         R::any, X::any, S::any, H::any,
         E::create_starting_provisioning},
        {C::create_stopped, A::exclusive_mutation, P::absent, L::any,
         R::any, X::any, S::any, H::any,
         E::create_stopped_provisioning},
        {C::import_profile, A::exclusive_mutation, P::absent, L::any,
         R::any, X::any, S::any, H::any, E::import_stopped_ready},
        {C::start, A::exclusive_mutation, P::present, L::any,
         R::stopped_or_running, X::idle,
         S::not_commit_indeterminate, H::operational,
         E::start_by_lifecycle},
        {C::stop, A::exclusive_mutation, P::present, L::any,
         R::stopped_or_running, X::idle,
         S::compactable, H::nonfatal, E::begin_stopping},
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
         R::stopped_or_running, X::idle,
         S::not_commit_indeterminate, H::nonfatal, E::preserve},
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
         S::commit_indeterminate, H::nonfatal,
         E::begin_commit_resolution},
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

} // namespace

TEST_CASE(state_machine_contract_table_matches_every_frozen_command) {
    const auto actual = myrd::management::transition_contracts();
    REQUIRE_EQ(actual.size(), kExpectedContracts.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        REQUIRE_EQ(static_cast<std::size_t>(actual[index].command), index);
        REQUIRE_EQ(actual[index], kExpectedContracts[index]);
    }
}

namespace {

using Runtime = myrd::product::RuntimeState;
using Activity = myrd::product::ActivityState;
using Persistence = myrd::product::PersistenceState;
using Health = myrd::product::HealthState;
using Lifecycle = myrd::profile::ProfileLifecycle;
using Disposition = myrd::management::TransitionDisposition;
using Action = myrd::management::ActionKind;
using Outcome = myrd::management::CompletionOutcome;

myrd::management::ProfileState state(
    Lifecycle lifecycle,
    Runtime runtime,
    Activity activity,
    Persistence persistence,
    Health health) {
    return myrd::management::registered_profile_state(
        lifecycle, myrd::product::DiskState{
            runtime, activity, persistence, health});
}

Disposition expected_start(
    Runtime runtime,
    Activity activity,
    Persistence persistence,
    Health health) {
    if (persistence == Persistence::commit_indeterminate) {
        return Disposition::commit_indeterminate;
    }
    if (health == Health::fatal) return Disposition::fatal;
    if (health == Health::needs_attention) {
        return Disposition::needs_attention;
    }
    if (activity != Activity::idle) return Disposition::state_conflict;
    if (runtime == Runtime::running) return Disposition::already_running;
    if (runtime != Runtime::stopped) return Disposition::state_conflict;
    return Disposition::allowed;
}

} // namespace

TEST_CASE(state_machine_create_import_start_vertical_slice) {
    const auto absent = myrd::management::absent_profile_state();
    for (const auto command : {
             C::create_and_start, C::create_stopped, C::import_profile}) {
        const auto plan = myrd::management::plan_command(absent, command);
        REQUIRE(plan.accepted());
        REQUIRE_EQ(plan.disposition, Disposition::allowed);
        REQUIRE(plan.admitted.lifecycle.has_value());
    }
    REQUIRE_EQ(
        myrd::management::plan_command(absent, C::start).disposition,
        Disposition::profile_not_found);

    const auto provisioning = state(
        Lifecycle::provisioning, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    auto plan = myrd::management::plan_command(provisioning, C::start);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.runtime, Runtime::starting);
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::provisioning);

    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    plan = myrd::management::plan_command(ready, C::start);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.runtime, Runtime::starting);
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::recovering);
}

TEST_CASE(state_machine_create_import_start_actions_never_cross_format_boundary) {
    const auto absent = myrd::management::absent_profile_state();
    auto plan = myrd::management::plan_command(
        absent, C::create_and_start);
    REQUIRE_EQ(plan.action_count, 15U);
    REQUIRE_EQ(plan.actions.front(), Action::create_and_verify_bundle);
    REQUIRE_EQ(plan.actions[1], Action::publish_provisioning_profile);
    REQUIRE_EQ(plan.actions[7], Action::prove_new_uninitialized);
    REQUIRE_EQ(plan.actions[8], Action::provision_gpt_ntfs);
    REQUIRE_EQ(plan.actions[12], Action::publish_ready_profile);
    REQUIRE_EQ(plan.action_span().back(), Action::readiness_io);

    plan = myrd::management::plan_command(absent, C::create_stopped);
    REQUIRE_EQ(plan.action_count, 2U);
    REQUIRE_EQ(plan.actions[0], Action::create_and_verify_bundle);
    REQUIRE_EQ(plan.actions[1], Action::publish_provisioning_profile);

    plan = myrd::management::plan_command(absent, C::import_profile);
    REQUIRE_EQ(plan.action_count, 2U);
    REQUIRE_EQ(plan.actions[0], Action::inspect_bundle_read_only);
    REQUIRE_EQ(plan.actions[1], Action::publish_imported_ready_profile);

    const auto provisioning = state(
        Lifecycle::provisioning, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    plan = myrd::management::plan_command(provisioning, C::start);
    REQUIRE(std::find(
        plan.action_span().begin(), plan.action_span().end(),
        Action::provision_gpt_ntfs) != plan.action_span().end());

    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    plan = myrd::management::plan_command(ready, C::start);
    REQUIRE_EQ(plan.action_count, 8U);
    REQUIRE(std::find(
        plan.action_span().begin(), plan.action_span().end(),
        Action::provision_gpt_ntfs) == plan.action_span().end());
    REQUIRE(std::find(
        plan.action_span().begin(), plan.action_span().end(),
        Action::prove_new_uninitialized) == plan.action_span().end());
}

TEST_CASE(state_machine_exhausts_create_import_start_without_rejected_mutation) {
    constexpr std::array lifecycles{
        Lifecycle::provisioning, Lifecycle::ready};
    constexpr std::array runtimes{
        Runtime::stopped, Runtime::starting, Runtime::running,
        Runtime::stopping, Runtime::orphaned};
    constexpr std::array activities{
        Activity::idle, Activity::provisioning, Activity::recovering,
        Activity::checkpointing, Activity::compacting,
        Activity::resolving_commit};
    constexpr std::array persistences{
        Persistence::clean, Persistence::dirty,
        Persistence::persistence_blocked,
        Persistence::commit_indeterminate};
    constexpr std::array healths{
        Health::healthy, Health::degraded,
        Health::needs_attention, Health::fatal};

    std::size_t examined = 0;
    for (const auto lifecycle : lifecycles) {
        for (const auto runtime : runtimes) {
            for (const auto activity : activities) {
                for (const auto persistence : persistences) {
                    for (const auto health : healths) {
                        const auto before = state(
                            lifecycle, runtime, activity, persistence, health);
                        for (const auto command : {
                                 C::create_and_start, C::create_stopped,
                                 C::import_profile}) {
                            const auto plan = myrd::management::plan_command(
                                before, command);
                            REQUIRE(!plan.accepted());
                            REQUIRE_EQ(
                                plan.disposition,
                                Disposition::profile_already_exists);
                            REQUIRE_EQ(plan.admitted, before);
                            ++examined;
                        }

                        const auto plan = myrd::management::plan_command(
                            before, C::start);
                        REQUIRE_EQ(
                            plan.disposition,
                            expected_start(
                                runtime, activity, persistence, health));
                        if (!plan.accepted()) {
                            REQUIRE_EQ(plan.admitted, before);
                        }
                        ++examined;
                    }
                }
            }
        }
    }
    REQUIRE_EQ(examined, 3'840U);
}

TEST_CASE(state_machine_rejects_unknown_enum_state_without_mutation) {
    const auto invalid = state(
        Lifecycle::ready, static_cast<Runtime>(255), Activity::idle,
        Persistence::clean, Health::healthy);
    const auto plan = myrd::management::plan_command(invalid, C::start);
    REQUIRE(!plan.accepted());
    REQUIRE_EQ(plan.disposition, Disposition::invalid_state);
    REQUIRE_EQ(plan.admitted, invalid);
}

TEST_CASE(state_machine_stop_checkpoint_and_compact_follow_durability_rules) {
    const auto dirty = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::dirty, Health::healthy);
    auto plan = myrd::management::plan_command(dirty, C::stop);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.runtime, Runtime::stopping);
    const std::vector<Action> expected_stop_actions{
        Action::quiesce_mutations,
        Action::flush_exact_volume,
        Action::checkpoint_if_required,
        Action::remove_mount,
        Action::remove_exact_device,
        Action::stop_runtime_and_release_lock};
    REQUIRE_EQ(
        std::vector<Action>(
            plan.action_span().begin(), plan.action_span().end()),
        expected_stop_actions);

    const auto blocked = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::persistence_blocked, Health::degraded);
    plan = myrd::management::plan_command(blocked, C::stop);
    REQUIRE(!plan.accepted());
    REQUIRE_EQ(plan.disposition, Disposition::persistence_blocked);
    REQUIRE_EQ(plan.admitted, blocked);

    plan = myrd::management::plan_command(blocked, C::force_stop);
    REQUIRE(plan.accepted());
    REQUIRE(std::find(
        plan.action_span().begin(), plan.action_span().end(),
        Action::checkpoint_if_required) == plan.action_span().end());

    const auto clean = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::clean, Health::healthy);
    plan = myrd::management::plan_command(clean, C::checkpoint);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.disposition, Disposition::no_changes);
    REQUIRE_EQ(plan.action_count, 0U);
    REQUIRE_EQ(plan.admitted, clean);

    plan = myrd::management::plan_command(dirty, C::checkpoint);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::checkpointing);
    REQUIRE_EQ(plan.action_count, 2U);

    plan = myrd::management::plan_command(dirty, C::compact);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::checkpointing);
    REQUIRE_EQ(plan.action_span().back(), Action::compact);

    plan = myrd::management::plan_command(blocked, C::compact);
    REQUIRE(!plan.accepted());
    REQUIRE_EQ(plan.disposition, Disposition::persistence_blocked);
}

TEST_CASE(state_machine_config_repair_rebind_forget_and_resolution_contracts) {
    const auto ready_stopped = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    REQUIRE(myrd::management::plan_command(
        ready_stopped, C::config_set_live).accepted());
    REQUIRE(myrd::management::plan_command(
        ready_stopped, C::config_set_stopped).accepted());
    REQUIRE(myrd::management::plan_command(
        ready_stopped, C::rebind).accepted());
    REQUIRE(myrd::management::plan_command(
        ready_stopped, C::forget).accepted());

    const auto ready_running = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::clean, Health::healthy);
    REQUIRE(myrd::management::plan_command(
        ready_running, C::config_set_live).accepted());
    REQUIRE(!myrd::management::plan_command(
        ready_running, C::config_set_stopped).accepted());
    REQUIRE(!myrd::management::plan_command(
        ready_running, C::rebind).accepted());
    REQUIRE(!myrd::management::plan_command(
        ready_running, C::forget).accepted());

    const auto damaged = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::needs_attention);
    auto plan = myrd::management::plan_command(damaged, C::repair_profile);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::recovering);
    REQUIRE(!myrd::management::plan_command(
        ready_stopped, C::repair_profile).accepted());

    const auto indeterminate = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::commit_indeterminate, Health::needs_attention);
    plan = myrd::management::plan_command(indeterminate, C::resolve_commit);
    REQUIRE(plan.accepted());
    REQUIRE_EQ(plan.admitted.disk.activity, Activity::resolving_commit);
    for (const auto command : {
             C::start, C::stop, C::force_stop, C::checkpoint, C::compact,
             C::config_set_live, C::config_set_stopped, C::repair_profile,
             C::rebind, C::forget, C::operation_cancel}) {
        const auto rejected = myrd::management::plan_command(
            indeterminate, command);
        REQUIRE(!rejected.accepted());
        REQUIRE_EQ(rejected.disposition, Disposition::commit_indeterminate);
        REQUIRE_EQ(rejected.admitted, indeterminate);
    }
    for (const auto command : {
             C::status_read, C::config_read, C::diagnostics_read,
             C::operation_read}) {
        REQUIRE(myrd::management::plan_command(
            indeterminate, command).accepted());
    }
}

TEST_CASE(state_machine_all_rejected_plans_are_side_effect_free) {
    constexpr std::array lifecycles{
        Lifecycle::provisioning, Lifecycle::ready};
    constexpr std::array runtimes{
        Runtime::stopped, Runtime::starting, Runtime::running,
        Runtime::stopping, Runtime::orphaned};
    constexpr std::array activities{
        Activity::idle, Activity::provisioning, Activity::recovering,
        Activity::checkpointing, Activity::compacting,
        Activity::resolving_commit};
    constexpr std::array persistences{
        Persistence::clean, Persistence::dirty,
        Persistence::persistence_blocked,
        Persistence::commit_indeterminate};
    constexpr std::array healths{
        Health::healthy, Health::degraded,
        Health::needs_attention, Health::fatal};

    std::size_t examined = 0;
    for (const auto lifecycle : lifecycles) {
        for (const auto runtime : runtimes) {
            for (const auto activity : activities) {
                for (const auto persistence : persistences) {
                    for (const auto health : healths) {
                        const auto before = state(
                            lifecycle, runtime, activity, persistence, health);
                        for (std::size_t value = 0;
                             value < myrd::management::kManagementCommandCount;
                             ++value) {
                            const auto plan = myrd::management::plan_command(
                                before, static_cast<C>(value));
                            if (!plan.accepted()) {
                                REQUIRE_EQ(plan.admitted, before);
                                REQUIRE_EQ(plan.action_count, 0U);
                            } else if (plan.disposition
                                       == Disposition::allowed) {
                                REQUIRE(plan.action_count != 0U);
                            } else {
                                REQUIRE_EQ(plan.admitted, before);
                                REQUIRE_EQ(plan.action_count, 0U);
                            }
                            ++examined;
                        }
                    }
                }
            }
        }
    }
    REQUIRE_EQ(examined, 18'240U);
}

namespace {

myrd::product::Uuid operation_id(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

} // namespace

TEST_CASE(state_machine_gate_serializes_mutations_but_never_reads) {
    const auto first_id = operation_id(
        "10000000-0000-4000-8000-000000000001");
    const auto second_id = operation_id(
        "20000000-0000-4000-8000-000000000002");
    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    const auto start = myrd::management::plan_command(ready, C::start);
    auto gate = myrd::management::idle_operation_gate();
    auto acquired = myrd::management::plan_gate_access(gate, start, first_id);
    REQUIRE(acquired.accepted());
    REQUIRE_EQ(acquired.after.active_operation_id, first_id);
    REQUIRE(acquired.after.cancellable);

    const auto config = myrd::management::plan_command(
        ready, C::config_set_live);
    auto conflict = myrd::management::plan_gate_access(
        acquired.after, config, second_id);
    REQUIRE(!conflict.accepted());
    REQUIRE_EQ(
        conflict.disposition,
        myrd::management::GateDisposition::operation_conflict);
    REQUIRE_EQ(conflict.after, acquired.after);

    const auto read = myrd::management::plan_command(ready, C::status_read);
    auto concurrent_read = myrd::management::plan_gate_access(
        acquired.after, read, second_id);
    REQUIRE(concurrent_read.accepted());
    REQUIRE_EQ(concurrent_read.after, acquired.after);

    const auto running = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::clean, Health::healthy);
    const auto already = myrd::management::plan_command(running, C::start);
    auto no_op = myrd::management::plan_gate_access(
        acquired.after, already, second_id);
    REQUIRE(!no_op.accepted());
    REQUIRE_EQ(
        no_op.disposition,
        myrd::management::GateDisposition::operation_conflict);
    REQUIRE_EQ(no_op.after, acquired.after);

    no_op = myrd::management::plan_gate_access(
        myrd::management::idle_operation_gate(), already, second_id);
    REQUIRE(no_op.accepted());
    REQUIRE_EQ(no_op.after, myrd::management::idle_operation_gate());
}

TEST_CASE(state_machine_gate_enforces_exact_cancel_boundary_and_release) {
    const auto active_id = operation_id(
        "30000000-0000-4000-8000-000000000003");
    const auto stale_id = operation_id(
        "40000000-0000-4000-8000-000000000004");
    const auto dirty = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::dirty, Health::healthy);
    const auto checkpoint = myrd::management::plan_command(
        dirty, C::checkpoint);
    auto acquired = myrd::management::plan_gate_access(
        myrd::management::idle_operation_gate(), checkpoint, active_id);
    REQUIRE(acquired.accepted());

    const auto cancel = myrd::management::plan_command(
        dirty, C::operation_cancel);
    auto decision = myrd::management::plan_gate_access(
        acquired.after, cancel, stale_id);
    REQUIRE(!decision.accepted());
    REQUIRE_EQ(
        decision.disposition,
        myrd::management::GateDisposition::target_mismatch);
    REQUIRE_EQ(decision.after, acquired.after);

    decision = myrd::management::plan_gate_access(
        acquired.after, cancel, active_id);
    REQUIRE(decision.accepted());
    REQUIRE_EQ(decision.after, acquired.after);

    auto boundary = myrd::management::mark_operation_noncancellable(
        acquired.after, active_id);
    REQUIRE(boundary.accepted());
    REQUIRE(!boundary.after.cancellable);
    decision = myrd::management::plan_gate_access(
        boundary.after, cancel, active_id);
    REQUIRE(!decision.accepted());
    REQUIRE_EQ(
        decision.disposition,
        myrd::management::GateDisposition::cancellation_too_late);
    REQUIRE_EQ(decision.after, boundary.after);

    auto released = myrd::management::release_operation_gate(
        boundary.after, stale_id);
    REQUIRE(!released.accepted());
    REQUIRE_EQ(released.after, boundary.after);
    released = myrd::management::release_operation_gate(
        boundary.after, active_id);
    REQUIRE(released.accepted());
    REQUIRE_EQ(released.after, myrd::management::idle_operation_gate());
}

TEST_CASE(state_machine_completion_covers_create_import_and_start) {
    const auto absent = myrd::management::absent_profile_state();
    auto plan = myrd::management::plan_command(
        absent, C::create_and_start);
    auto completed = myrd::management::complete_command(
        plan, Outcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.lifecycle, Lifecycle::ready);
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::running);
    REQUIRE_EQ(completed.after.disk.activity, Activity::idle);
    REQUIRE_EQ(completed.after.disk.persistence, Persistence::clean);

    completed = myrd::management::complete_command(
        plan, Outcome::needs_attention_failure);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.lifecycle, Lifecycle::provisioning);
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::stopped);
    REQUIRE_EQ(completed.after.disk.health, Health::needs_attention);

    plan = myrd::management::plan_command(absent, C::import_profile);
    completed = myrd::management::complete_command(
        plan, Outcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.lifecycle, Lifecycle::ready);
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::stopped);

    const auto provisioning = state(
        Lifecycle::provisioning, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    plan = myrd::management::plan_command(provisioning, C::start);
    completed = myrd::management::complete_command(
        plan, Outcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.lifecycle, Lifecycle::ready);
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::running);
}

TEST_CASE(state_machine_ready_start_never_invents_a_clean_checkpoint) {
    for (const auto [persistence, health] : {
             std::pair{Persistence::clean, Health::healthy},
             std::pair{Persistence::dirty, Health::healthy},
             std::pair{
                 Persistence::persistence_blocked, Health::degraded}}) {
        const auto before = state(
            Lifecycle::ready, Runtime::stopped, Activity::idle,
            persistence, health);
        const auto plan = myrd::management::plan_command(before, C::start);
        REQUIRE(plan.accepted());
        const auto completed = myrd::management::complete_command(
            plan, Outcome::succeeded_clean);
        REQUIRE(completed.applied());
        REQUIRE_EQ(completed.after.disk.runtime, Runtime::running);
        REQUIRE_EQ(completed.after.disk.activity, Activity::idle);
        REQUIRE_EQ(completed.after.disk.persistence, persistence);
        REQUIRE_EQ(completed.after.disk.health, health);
    }
}

TEST_CASE(state_machine_fatal_profiles_permit_only_safe_reads) {
    const auto fatal = state(
        Lifecycle::ready, Runtime::orphaned, Activity::resolving_commit,
        Persistence::commit_indeterminate, Health::fatal);

    for (const auto command : {
             C::status_read, C::config_read, C::diagnostics_read,
             C::operation_read}) {
        const auto plan = myrd::management::plan_command(fatal, command);
        REQUIRE(plan.accepted());
        REQUIRE_EQ(plan.admitted, fatal);
    }

    for (const auto command : {
             C::start, C::stop, C::force_stop, C::checkpoint, C::compact,
             C::config_set_live, C::config_set_stopped, C::repair_profile,
             C::rebind, C::forget, C::resolve_commit,
             C::operation_cancel}) {
        const auto plan = myrd::management::plan_command(fatal, command);
        REQUIRE(!plan.accepted());
        REQUIRE_EQ(plan.admitted, fatal);
        REQUIRE_EQ(plan.action_count, 0U);
    }
}

TEST_CASE(state_machine_completion_preserves_normal_stop_and_force_semantics) {
    const auto dirty = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::dirty, Health::healthy);
    auto normal = myrd::management::plan_command(dirty, C::stop);
    auto completed = myrd::management::complete_command(
        normal, Outcome::persistence_blocked);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::running);
    REQUIRE_EQ(
        completed.after.disk.persistence,
        Persistence::persistence_blocked);
    REQUIRE_EQ(completed.after.disk.health, Health::degraded);

    completed = myrd::management::complete_command(
        normal, Outcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::stopped);
    REQUIRE_EQ(completed.after.disk.persistence, Persistence::clean);

    auto force = myrd::management::plan_command(dirty, C::force_stop);
    completed = myrd::management::complete_command(
        force, Outcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.runtime, Runtime::stopped);
    REQUIRE_EQ(completed.after.disk.persistence, Persistence::clean);
    REQUIRE_EQ(completed.after.disk.health, Health::degraded);
}

TEST_CASE(state_machine_completion_classifies_checkpoint_and_compaction) {
    const auto dirty = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::dirty, Health::healthy);
    const auto checkpoint = myrd::management::plan_command(
        dirty, C::checkpoint);
    for (const auto [outcome, persistence] : {
             std::pair{Outcome::succeeded_clean, Persistence::clean},
             std::pair{Outcome::succeeded_dirty, Persistence::dirty}}) {
        const auto completed = myrd::management::complete_command(
            checkpoint, outcome);
        REQUIRE(completed.applied());
        REQUIRE_EQ(completed.after.disk.runtime, Runtime::running);
        REQUIRE_EQ(completed.after.disk.activity, Activity::idle);
        REQUIRE_EQ(completed.after.disk.persistence, persistence);
    }

    auto completed = myrd::management::complete_command(
        checkpoint, Outcome::committed_needs_reconciliation_dirty);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.activity, Activity::resolving_commit);
    REQUIRE_EQ(completed.after.disk.persistence, Persistence::dirty);
    REQUIRE_EQ(completed.after.disk.health, Health::degraded);

    completed = myrd::management::complete_command(
        checkpoint, Outcome::commit_indeterminate);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.activity, Activity::resolving_commit);
    REQUIRE_EQ(
        completed.after.disk.persistence,
        Persistence::commit_indeterminate);
    REQUIRE_EQ(completed.after.disk.health, Health::needs_attention);

    completed = myrd::management::complete_command(
        checkpoint, Outcome::compaction_deferred);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(completed.after, dirty);

    const auto compact = myrd::management::plan_command(dirty, C::compact);
    completed = myrd::management::complete_command(
        compact, Outcome::compaction_deferred);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after, dirty);
}

TEST_CASE(state_machine_completion_resolves_or_preserves_indeterminacy) {
    const auto indeterminate = state(
        Lifecycle::ready, Runtime::running, Activity::idle,
        Persistence::commit_indeterminate, Health::needs_attention);
    const auto resolution = myrd::management::plan_command(
        indeterminate, C::resolve_commit);
    auto completed = myrd::management::complete_command(
        resolution, Outcome::succeeded_dirty);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.activity, Activity::idle);
    REQUIRE_EQ(completed.after.disk.persistence, Persistence::dirty);
    REQUIRE_EQ(completed.after.disk.health, Health::healthy);

    completed = myrd::management::complete_command(
        resolution, Outcome::resolution_unresolved);
    REQUIRE(completed.applied());
    REQUIRE_EQ(completed.after.disk.activity, Activity::idle);
    REQUIRE_EQ(
        completed.after.disk.persistence,
        Persistence::commit_indeterminate);
    REQUIRE_EQ(completed.after.disk.health, Health::needs_attention);
}

TEST_CASE(state_machine_metadata_publication_indeterminacy_fails_closed) {
    const auto absent = myrd::management::absent_profile_state();
    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    const auto damaged = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::needs_attention);
    const std::array plans{
        myrd::management::plan_command(absent, C::create_stopped),
        myrd::management::plan_command(absent, C::import_profile),
        myrd::management::plan_command(ready, C::config_set_live),
        myrd::management::plan_command(ready, C::config_set_stopped),
        myrd::management::plan_command(damaged, C::repair_profile),
        myrd::management::plan_command(ready, C::rebind),
        myrd::management::plan_command(ready, C::forget)};

    for (const auto& plan : plans) {
        REQUIRE(plan.accepted());
        const auto completed = myrd::management::complete_command(
            plan, Outcome::commit_indeterminate);
        REQUIRE(completed.applied());
        REQUIRE(completed.after.lifecycle.has_value());
        REQUIRE_EQ(
            completed.after.disk.activity, Activity::resolving_commit);
        REQUIRE_EQ(
            completed.after.disk.persistence,
            Persistence::commit_indeterminate);
        REQUIRE_EQ(completed.after.disk.health, Health::needs_attention);
        REQUIRE(myrd::management::plan_command(
            completed.after, C::resolve_commit).accepted());
        REQUIRE(!myrd::management::plan_command(
            completed.after, C::config_set_live).accepted());
    }
}

TEST_CASE(state_machine_invalid_completion_is_side_effect_free) {
    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    const auto start = myrd::management::plan_command(ready, C::start);
    auto completed = myrd::management::complete_command(
        start, Outcome::compaction_deferred);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(
        completed.disposition,
        myrd::management::CompletionDisposition::invalid_outcome);
    REQUIRE_EQ(completed.after, ready);

    const auto rejected = myrd::management::plan_command(ready, C::checkpoint);
    completed = myrd::management::complete_command(
        rejected, Outcome::succeeded_clean);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(
        completed.disposition,
        myrd::management::CompletionDisposition::invalid_plan);
    REQUIRE_EQ(completed.after, ready);
}

TEST_CASE(state_machine_rejects_forged_action_plan_bounds) {
    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    auto forged = myrd::management::plan_command(ready, C::start);
    REQUIRE(myrd::management::is_valid(forged));
    forged.action_count = myrd::management::kMaximumPlanActions + 1U;
    REQUIRE(!myrd::management::is_valid(forged));
    REQUIRE(forged.action_span().empty());

    const auto id = operation_id(
        "50000000-0000-4000-8000-000000000005");
    const auto gate = myrd::management::plan_gate_access(
        myrd::management::idle_operation_gate(), forged, id);
    REQUIRE(!gate.accepted());
    REQUIRE_EQ(
        gate.disposition,
        myrd::management::GateDisposition::invalid_request);
    const auto completed = myrd::management::complete_command(
        forged, Outcome::succeeded_clean);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(
        completed.disposition,
        myrd::management::CompletionDisposition::invalid_plan);
    REQUIRE_EQ(completed.after, ready);
}

TEST_CASE(state_machine_rejects_semantically_forged_action_plans) {
    const auto ready = state(
        Lifecycle::ready, Runtime::stopped, Activity::idle,
        Persistence::clean, Health::healthy);
    const auto canonical = myrd::management::plan_command(ready, C::start);
    REQUIRE(myrd::management::is_valid(canonical));
    const auto id = operation_id(
        "60000000-0000-4000-8000-000000000006");

    auto forged = canonical;
    forged.actions[0] = Action::read_status;
    REQUIRE(!myrd::management::is_valid(forged));
    auto gate = myrd::management::plan_gate_access(
        myrd::management::idle_operation_gate(), forged, id);
    REQUIRE(!gate.accepted());
    auto completed = myrd::management::complete_command(
        forged, Outcome::succeeded_clean);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(completed.after, ready);

    forged = canonical;
    forged.admitted.disk.persistence = Persistence::dirty;
    REQUIRE(!myrd::management::is_valid(forged));

    forged = canonical;
    forged.disposition = static_cast<Disposition>(255);
    forged.admitted = forged.before;
    forged.actions.fill(Action::none);
    forged.action_count = 0;
    REQUIRE(!myrd::management::is_valid(forged));

    gate = myrd::management::plan_gate_access(
        myrd::management::idle_operation_gate(), forged, id);
    REQUIRE(!gate.accepted());
    REQUIRE_EQ(
        gate.disposition,
        myrd::management::GateDisposition::invalid_request);
    completed = myrd::management::complete_command(
        forged, Outcome::succeeded_clean);
    REQUIRE(!completed.applied());
    REQUIRE_EQ(
        completed.disposition,
        myrd::management::CompletionDisposition::invalid_plan);
    REQUIRE_EQ(completed.after, ready);
}

TEST_CASE(state_machine_exhausts_every_completion_without_invalid_state) {
    constexpr std::array lifecycles{
        Lifecycle::provisioning, Lifecycle::ready};
    constexpr std::array runtimes{
        Runtime::stopped, Runtime::starting, Runtime::running,
        Runtime::stopping, Runtime::orphaned};
    constexpr std::array activities{
        Activity::idle, Activity::provisioning, Activity::recovering,
        Activity::checkpointing, Activity::compacting,
        Activity::resolving_commit};
    constexpr std::array persistences{
        Persistence::clean, Persistence::dirty,
        Persistence::persistence_blocked,
        Persistence::commit_indeterminate};
    constexpr std::array healths{
        Health::healthy, Health::degraded,
        Health::needs_attention, Health::fatal};
    constexpr std::array outcomes{
        Outcome::succeeded_clean,
        Outcome::succeeded_dirty,
        Outcome::transient_failure,
        Outcome::needs_attention_failure,
        Outcome::persistence_blocked,
        Outcome::committed_needs_reconciliation_clean,
        Outcome::committed_needs_reconciliation_dirty,
        Outcome::commit_indeterminate,
        Outcome::compaction_deferred,
        Outcome::resolution_unresolved};

    std::size_t examined = 0;
    for (const auto lifecycle : lifecycles) {
        for (const auto runtime : runtimes) {
            for (const auto activity : activities) {
                for (const auto persistence : persistences) {
                    for (const auto health : healths) {
                        const auto before = state(
                            lifecycle, runtime, activity, persistence, health);
                        for (std::size_t command_value = 0;
                             command_value
                                 < myrd::management::kManagementCommandCount;
                             ++command_value) {
                            const auto plan = myrd::management::plan_command(
                                before, static_cast<C>(command_value));
                            for (const auto outcome : outcomes) {
                                const auto completed =
                                    myrd::management::complete_command(
                                        plan, outcome);
                                if (completed.applied()) {
                                    REQUIRE(myrd::management::is_valid(
                                        completed.after));
                                } else {
                                    REQUIRE_EQ(completed.after, before);
                                }
                                ++examined;
                            }
                        }
                    }
                }
            }
        }
    }
    REQUIRE_EQ(examined, 182'400U);
}
