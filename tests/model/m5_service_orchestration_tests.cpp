#include "tests/support/test_framework.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

using namespace myrd;
using namespace myrd::management;

namespace {

ProfileState state(product::RuntimeState runtime,
    product::ActivityState activity = product::ActivityState::idle,
    product::PersistenceState persistence = product::PersistenceState::clean,
    product::HealthState health = product::HealthState::healthy) {
    return registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{runtime, activity, persistence, health});
}

product::Uuid id(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

} // namespace

TEST_CASE(m5_aggregate_projection_maps_lifecycle_states) {
    REQUIRE(aggregate_status(state(product::RuntimeState::running))
        == AggregateStatus::running);
    REQUIRE(aggregate_status(state(product::RuntimeState::starting))
        == AggregateStatus::running);
    REQUIRE(aggregate_status(state(product::RuntimeState::stopped))
        == AggregateStatus::stopped_configured);
    REQUIRE(aggregate_status(state(product::RuntimeState::stopped,
              product::ActivityState::idle,
              product::PersistenceState::persistence_blocked))
        == AggregateStatus::blocked_needs_attention);
    REQUIRE(aggregate_status(state(product::RuntimeState::orphaned))
        == AggregateStatus::failed);
    REQUIRE(aggregate_status(state(product::RuntimeState::stopped,
              product::ActivityState::idle,
              product::PersistenceState::commit_indeterminate))
        == AggregateStatus::blocked_needs_attention);
    REQUIRE(to_string(AggregateStatus::blocked_needs_attention)
        == "blocked/needs-attention");
}

// §3.6's last row: `resolving` "恢复 reconciliation，阻止 stop/remove".  That is
// not a guard inside some caller -- it is one statement in the transition
// contract's activity requirement, where every mutating command asks for `idle`
// and a profile in `resolving_commit` is not idle.  Asserted here because
// nothing asserted it: the suite checked that the state *is* `resolving_commit`,
// never that anything is refused for it.
//
// Why it matters: `resolving_commit` means a publish whose outcome is unknown.
// A stop removes the mount and drops every uncommitted write, and a checkpoint
// cuts a new snapshot; either one taken over an unresolved commit destroys the
// evidence the reconciliation is supposed to read.
TEST_CASE(m5_a_resolving_commit_admits_only_the_command_that_resolves_it) {
    const auto resolving = state(product::RuntimeState::running,
                                 product::ActivityState::resolving_commit,
                                 product::PersistenceState::commit_indeterminate);
    const ManagementCommand blocked[] = {
        ManagementCommand::stop,
        ManagementCommand::force_stop,
        ManagementCommand::start,
        ManagementCommand::checkpoint,
        ManagementCommand::compact,
        ManagementCommand::config_set_live,
        ManagementCommand::config_set_stopped,
        ManagementCommand::repair_profile,
        ManagementCommand::rebind,
        ManagementCommand::forget};
    for (const auto command : blocked) {
        const auto plan = plan_command(resolving, command);
        REQUIRE(!plan.accepted());
    }
    // Reads stay readable -- an operator has to be able to see what is stuck --
    // and the one command that can move it forward is admitted.  A gate that
    // refused everything would leave the disk stuck with no way out.
    REQUIRE(plan_command(resolving, ManagementCommand::status_read).accepted());
    REQUIRE(plan_command(resolving, ManagementCommand::diagnostics_read).accepted());
    REQUIRE(plan_command(resolving, ManagementCommand::resolve_commit).accepted());
}

TEST_CASE(m5_aggregate_projection_is_fail_safe_for_unknown_or_contradictory_state) {
    auto unknown = state(product::RuntimeState::stopped);
    unknown.disk.runtime = static_cast<product::RuntimeState>(0xff);
    REQUIRE(aggregate_status(unknown) == AggregateStatus::failed);

    auto unknown_activity = state(product::RuntimeState::running);
    unknown_activity.disk.activity =
        static_cast<product::ActivityState>(0xff);
    REQUIRE(aggregate_status(unknown_activity) == AggregateStatus::failed);

    auto unknown_persistence = state(product::RuntimeState::stopped);
    unknown_persistence.disk.persistence =
        static_cast<product::PersistenceState>(0xff);
    REQUIRE(aggregate_status(unknown_persistence) == AggregateStatus::failed);

    auto attention = state(product::RuntimeState::stopped,
        product::ActivityState::resolving_commit,
        product::PersistenceState::commit_indeterminate,
        product::HealthState::needs_attention);
    REQUIRE(aggregate_status(attention)
        == AggregateStatus::blocked_needs_attention);

    auto stopped_busy = state(product::RuntimeState::stopped,
        product::ActivityState::checkpointing,
        product::PersistenceState::clean,
        product::HealthState::healthy);
    REQUIRE(aggregate_status(stopped_busy) == AggregateStatus::failed);

    auto absent_with_live_disk = absent_profile_state();
    absent_with_live_disk.disk.runtime = product::RuntimeState::running;
    REQUIRE(aggregate_status(absent_with_live_disk) == AggregateStatus::failed);
}

TEST_CASE(m5_operation_gate_serializes_operations_within_profile) {
    const auto operation_a = id("11111111-1111-4111-8111-111111111111");
    const auto operation_b = id("22222222-2222-4222-8222-222222222222");
    const auto first = plan_command(
        state(product::RuntimeState::stopped), ManagementCommand::start);
    const auto admitted = plan_gate_access(
        idle_operation_gate(), first, operation_a);
    REQUIRE(admitted.accepted());
    const auto conflicting_operation = plan_gate_access(
        admitted.after, first, operation_a);
    REQUIRE(conflicting_operation.disposition
        == GateDisposition::operation_conflict);
    REQUIRE(release_operation_gate(admitted.after, operation_a).accepted());
}

TEST_CASE(m5_checkpoint_indeterminate_is_not_success) {
    const auto before = state(product::RuntimeState::running,
        product::ActivityState::idle, product::PersistenceState::dirty);
    const auto plan = plan_command(before, ManagementCommand::checkpoint);
    REQUIRE(plan.accepted());
    const auto decision = complete_command(
        plan, CompletionOutcome::commit_indeterminate);
    REQUIRE(decision.applied());
    REQUIRE(decision.after.disk.persistence
        == product::PersistenceState::commit_indeterminate);
    REQUIRE(aggregate_status(decision.after)
        == AggregateStatus::blocked_needs_attention);
}

TEST_CASE(m5_checkpoint_dispositions_preserve_safe_truth) {
    const auto before = state(product::RuntimeState::running,
        product::ActivityState::idle, product::PersistenceState::dirty);
    const auto plan = plan_command(before, ManagementCommand::checkpoint);
    REQUIRE(plan.accepted());

    const auto committed = complete_command(
        plan, CompletionOutcome::succeeded_clean);
    REQUIRE(committed.applied());
    REQUIRE(committed.after.disk.persistence
        == product::PersistenceState::clean);

    const auto not_committed = complete_command(
        plan, CompletionOutcome::transient_failure);
    REQUIRE(not_committed.applied());
    REQUIRE(not_committed.after.disk.persistence
        == product::PersistenceState::dirty);

    const auto indeterminate = complete_command(
        plan, CompletionOutcome::commit_indeterminate);
    REQUIRE(indeterminate.applied());
    REQUIRE(indeterminate.after.disk.persistence
        == product::PersistenceState::commit_indeterminate);

    const auto reconciliation = complete_command(
        plan, CompletionOutcome::committed_needs_reconciliation_dirty);
    REQUIRE(reconciliation.applied());
    REQUIRE(reconciliation.after.disk.persistence
        == product::PersistenceState::dirty);
    REQUIRE(reconciliation.after.disk.activity
        == product::ActivityState::resolving_commit);
    REQUIRE(aggregate_status(reconciliation.after)
        == AggregateStatus::blocked_needs_attention);

    const auto reconciliation_clean = complete_command(
        plan, CompletionOutcome::committed_needs_reconciliation_clean);
    REQUIRE(reconciliation_clean.applied());
    REQUIRE(reconciliation_clean.after.disk.persistence
        == product::PersistenceState::clean);
    REQUIRE(reconciliation_clean.after.disk.activity
        == product::ActivityState::resolving_commit);
    REQUIRE(aggregate_status(reconciliation_clean.after)
        == AggregateStatus::blocked_needs_attention);
}

TEST_CASE(m5_independent_profile_gates_can_progress_in_parallel) {
    const auto operation_a = id("33333333-3333-4333-8333-333333333333");
    const auto operation_b = id("44444444-4444-4444-8444-444444444444");
    const auto plan = plan_command(
        state(product::RuntimeState::stopped), ManagementCommand::start);
    REQUIRE(plan.accepted());
    const auto gate_a = plan_gate_access(idle_operation_gate(), plan, operation_a);
    const auto gate_b = plan_gate_access(idle_operation_gate(), plan, operation_b);
    REQUIRE(gate_a.accepted());
    REQUIRE(gate_b.accepted());
    REQUIRE(gate_a.after.active_operation_id == operation_a);
    REQUIRE(gate_b.after.active_operation_id == operation_b);
}

TEST_CASE(m5_gate_release_requires_matching_operation_identity) {
    const auto operation_a = id("55555555-5555-4555-8555-555555555555");
    const auto operation_b = id("66666666-6666-4666-8666-666666666666");
    const auto plan = plan_command(
        state(product::RuntimeState::stopped), ManagementCommand::start);
    const auto admitted = plan_gate_access(
        idle_operation_gate(), plan, operation_a);
    REQUIRE(admitted.accepted());
    const auto wrong_release = release_operation_gate(
        admitted.after, operation_b);
    REQUIRE(wrong_release.disposition == GateDisposition::target_mismatch);
    REQUIRE(wrong_release.after == admitted.after);
    const auto released = release_operation_gate(admitted.after, operation_a);
    REQUIRE(released.accepted());
    const auto successor = plan_gate_access(
        released.after, plan, operation_b);
    REQUIRE(successor.accepted());
}

TEST_CASE(m5_service_core_admits_and_executes_two_profiles_independently) {
    Fixture fixture(product::PersistenceState::dirty);
    auto secondary = running_profile(product::PersistenceState::dirty);
    secondary.record.profile.profile_uuid =
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    secondary.record.profile.display_name = L"other";
    secondary.record.profile.owner_sid =
        L"S-1-5-21-100-200-300-1002";
    fixture.profiles.add_secondary(secondary);
    const auto operation_b =
        uuid("55555555-5555-4555-8555-555555555555");
    fixture.ids.values.push_back(operation_b);

    auto a = fixture.request("66666666-6666-4666-8666-666666666661");
    auto b = a;
    b.request_id = uuid("66666666-6666-4666-8666-666666666662");
    b.command = control::DiskCheckpointCommand{L"other"};
    b.caller = caller(secondary.record.profile.owner_sid);

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted_a = core.dispatch(a);
    auto admitted_b = core.dispatch(b);
    REQUIRE(admitted_a.ok());
    REQUIRE(admitted_b.ok());
    REQUIRE(admitted_a.value().operation_id.has_value());
    REQUIRE(admitted_b.value().operation_id.has_value());
    REQUIRE(admitted_a.value().operation_id
        != admitted_b.value().operation_id);
    REQUIRE_EQ(fixture.operations.records.size(), 2U);

    auto terminal_a = core.execute_operation(
        admitted_a.value().operation_id.value());
    auto terminal_b = core.execute_operation(
        admitted_b.value().operation_id.value());
    REQUIRE(terminal_a.ok());
    REQUIRE(terminal_b.ok());
    REQUIRE_EQ(terminal_a.value().state, OperationState::succeeded);
    REQUIRE_EQ(terminal_b.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE_EQ(fixture.profiles.secondary_snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id);
    REQUIRE(!fixture.profiles.secondary_snapshot().operation_gate
        .active_operation_id);
}

TEST_CASE(m5_service_core_rejects_same_profile_conflict_and_releases_gate) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto first = fixture.request("77777777-7777-4777-8777-777777777771");
    auto second = fixture.request("77777777-7777-4777-8777-777777777772");
    second.command = control::DiskStartCommand{L"work"};
    auto admitted = core.dispatch(first);
    REQUIRE(admitted.ok());
    auto conflict = core.dispatch(second);
    REQUIRE(!conflict.ok());
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    auto terminal = core.execute_operation(
        admitted.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    fixture.ids.values.push_back(
        uuid("77777777-7777-4777-8777-777777777773"));
    auto successor = core.dispatch(second);
    REQUIRE(successor.ok());
    REQUIRE(successor.value().operation_id.has_value());
}

TEST_CASE(m5_service_core_isolates_failed_profile_from_successful_profile) {
    Fixture fixture(product::PersistenceState::dirty);
    auto secondary = running_profile(product::PersistenceState::dirty);
    secondary.record.profile.profile_uuid =
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    secondary.record.profile.display_name = L"other";
    secondary.record.profile.owner_sid =
        L"S-1-5-21-100-200-300-1002";
    fixture.profiles.add_secondary(secondary);

    const auto operation_a =
        uuid("99999999-9999-4999-8999-999999999991");
    const auto operation_b =
        uuid("99999999-9999-4999-8999-999999999992");
    fixture.ids.values = {operation_a, operation_b};
    // The first profile's worker action fails.  The same worker fixture then
    // remains healthy for the independent second profile.
    fixture.worker.fail_action_once = ActionKind::flush_exact_volume;

    auto request_a = fixture.request(
        "99999999-9999-4999-8999-999999999981");
    auto request_b = request_a;
    request_b.request_id = uuid(
        "99999999-9999-4999-8999-999999999982");
    request_b.command = control::DiskCheckpointCommand{L"other"};
    request_b.caller = caller(secondary.record.profile.owner_sid);

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto admitted_a = core.dispatch(request_a);
    auto admitted_b = core.dispatch(request_b);
    REQUIRE(admitted_a.ok());
    REQUIRE(admitted_b.ok());
    REQUIRE_EQ(admitted_a.value().operation_id, operation_a);
    REQUIRE_EQ(admitted_b.value().operation_id, operation_b);

    const auto secondary_before = fixture.profiles.secondary_snapshot();
    const auto publications_before = fixture.profiles.publications.size();
    auto terminal_a = core.execute_operation(operation_a);
    REQUIRE(terminal_a.ok());
    REQUIRE_EQ(terminal_a.value().state, OperationState::failed);

    // A's failure must release only A's gate and must not publish or mutate B.
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
    REQUIRE(fixture.profiles.secondary_snapshot() == secondary_before);
    REQUIRE_EQ(fixture.profiles.publications.size(), publications_before + 2U);

    auto terminal_b = core.execute_operation(operation_b);
    REQUIRE(terminal_b.ok());
    REQUIRE_EQ(terminal_b.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 3U);
    REQUIRE_EQ(fixture.worker.calls[0], ActionKind::flush_exact_volume);
    REQUIRE_EQ(fixture.worker.calls[1], ActionKind::flush_exact_volume);
    REQUIRE_EQ(fixture.worker.calls[2], ActionKind::checkpoint);

    const auto& secondary_after = fixture.profiles.secondary_snapshot();
    REQUIRE_EQ(secondary_after.checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(secondary_after.state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE(!secondary_after.operation_gate.active_operation_id.has_value());

    // Every publication belongs to one of the two admitted profiles; no
    // cleanup/publication for A may be applied to B's profile record.
    for (const auto& publication : fixture.profiles.publications) {
        const auto id = publication.record.profile.profile_uuid;
        REQUIRE(id == fixture.profiles.snapshot().record.profile.profile_uuid
            || id == secondary.record.profile.profile_uuid);
    }
    REQUIRE_EQ(fixture.operations.records.size(), 2U);
    const auto record_a = std::find_if(
        fixture.operations.records.begin(), fixture.operations.records.end(),
        [&](const auto& record) { return record.operation_id == operation_a; });
    const auto record_b = std::find_if(
        fixture.operations.records.begin(), fixture.operations.records.end(),
        [&](const auto& record) { return record.operation_id == operation_b; });
    REQUIRE(record_a != fixture.operations.records.end());
    REQUIRE(record_b != fixture.operations.records.end());
    REQUIRE_EQ(record_a->state, OperationState::failed);
    REQUIRE_EQ(record_b->state, OperationState::succeeded);
}

TEST_CASE(m5_service_core_checkpoint_dispositions_never_report_success_on_uncertainty) {
    for (const auto disposition : {
             CheckpointCommitDisposition::not_committed,
             CheckpointCommitDisposition::commit_indeterminate,
             CheckpointCommitDisposition::committed,
             CheckpointCommitDisposition::committed_needs_reconciliation}) {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.worker.initial_disposition = disposition;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
            fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888881"));
        REQUIRE(queued.ok());
        auto terminal = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(terminal.ok());
        if (disposition == CheckpointCommitDisposition::committed) {
            REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
        } else {
            REQUIRE(terminal.value().state != OperationState::succeeded);
            REQUIRE(fixture.profiles.snapshot().state.disk.persistence
                != product::PersistenceState::clean);
        }
    }
}

// §3.6's second table, as the durable *phase* rather than the terminal state.
// The case above asserts the coarsest consequence -- an uncertain disposition
// never reports success -- which four different wrong answers would all satisfy.
// These are the durable facts the restart table is built on, and one of them
// wrong means a restart either rolls back bytes that landed or promotes bytes
// that never did.
//
// `evidence_recorded` is the P5 point: the commit evidence is durable and the
// epoch confirmation is not.  What it becomes afterwards is what the table
// then decides on.
TEST_CASE(m5_checkpoint_evidence_records_the_durable_phase_section_3_6_names) {
    struct Expectation final {
        CheckpointCommitDisposition disposition;
        CheckpointDurablePhase phase;
    };
    // committed            -> the epoch was confirmed in this same session, so
    //                         the profile is stable again with the new
    //                         generation confirmed.  `committed_pending_epoch_
    //                         confirmation` is the *transient* on the way there,
    //                         and it only survives a restart when the
    //                         confirmation did not land -- which is a different
    //                         case, because it needs an epoch publication that
    //                         fails.
    // not_committed        -> the prior generation is the durable truth again
    // the two uncertain    -> resolving, because neither answer may be assumed
    const Expectation expectations[] = {
        {CheckpointCommitDisposition::committed,
         CheckpointDurablePhase::stable},
        {CheckpointCommitDisposition::not_committed,
         CheckpointDurablePhase::stable},
        {CheckpointCommitDisposition::commit_indeterminate,
         CheckpointDurablePhase::resolving},
        {CheckpointCommitDisposition::committed_needs_reconciliation,
         CheckpointDurablePhase::resolving},
    };
    for (const auto& expected : expectations) {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.worker.initial_disposition = expected.disposition;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
            fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888881"));
        REQUIRE(queued.ok());
        auto terminal = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(terminal.ok());
        // The disposition is recorded whichever phase it left behind: a record
        // that forgot what the publish did could not be reconciled later.
        REQUIRE(fixture.profiles.snapshot().checkpoint.disposition
                == expected.disposition);
        REQUIRE(fixture.profiles.snapshot().checkpoint.phase == expected.phase);
    }
}

// §3.6's first table names `committed_pending_epoch_confirmation` as a row a
// restart resolves.  The case above never produces it, and that is the point:
// with the epoch confirmed in the same session it is only ever a transient.  It
// survives a restart exactly when the confirmation did not land -- so this case
// makes the epoch publication fail and pins what is left on disk.
//
// The distinction is the whole reason the phase exists.  Read as "committed", it
// would promote an epoch nobody confirmed; read as "nothing happened", it would
// roll back bytes that are on disk.  It has to be its own answer, and it has to
// be recoverable by asking the bundle.
TEST_CASE(m5_an_unconfirmed_epoch_leaves_the_pending_phase_on_disk) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition = CheckpointCommitDisposition::committed;
    fixture.worker.refuse_epoch_publication = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(fixture.request(
        "88888888-8888-4888-8888-888888888882"));
    REQUIRE(queued.ok());
    // The operation did not complete: the confirmation it was waiting on never
    // came, and reporting success would claim a clean checkpoint the service
    // cannot point at.
    const auto terminal = core.execute_operation(
        queued.value().operation_id.value());
    REQUIRE(!terminal.ok());

    const auto& checkpoint = fixture.profiles.snapshot().checkpoint;
    // What *is* durable is the commit itself, recorded with its disposition --
    // the bytes landed -- and the phase a restart branches on.
    REQUIRE(checkpoint.phase
            == CheckpointDurablePhase::committed_pending_epoch_confirmation);
    REQUIRE(checkpoint.disposition.has_value());
    REQUIRE(checkpoint.disposition.value()
            == CheckpointCommitDisposition::committed);
    REQUIRE(checkpoint.correlation.has_value());
}

// The read path must not report a durable claim the service can disprove.  A
// record that says `running` asserts a live worker; when the authority that
// holds the worker facts says there is none, neither the record's "running" nor
// a fabricated "stopped" is honest, so it reports blocked/needs-attention.
TEST_CASE(m5_recorded_running_without_a_live_worker_reports_blocked) {
    auto snapshot = running_profile(product::PersistenceState::clean);
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::running);

    // An authoritative observation that no worker is alive.
    snapshot.runtime_observation = WorkerRuntimeObservation{};
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::blocked_needs_attention);
    // The snapshot's own state is untouched: the store and the worker
    // scheduler still see the record they depend on.
    REQUIRE_EQ(snapshot.state.disk.runtime, product::RuntimeState::running);
    REQUIRE_EQ(snapshot.state.disk.health, product::HealthState::healthy);
}

// The mirror contradiction: a live worker for a profile the record calls
// cleanly stopped.  Reporting "stopped/configured" here would tell the caller a
// disk is safely down while a process is still holding it.
TEST_CASE(m5_live_worker_for_a_recorded_stopped_profile_reports_blocked) {
    auto snapshot = running_profile(product::PersistenceState::clean);
    snapshot.state.disk.runtime = product::RuntimeState::stopped;
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::stopped_configured);

    WorkerRuntimeObservation live{};
    live.worker_alive = true;
    live.committed_generation = 5;
    live.dirty_epoch = 11;
    snapshot.runtime_observation = live;
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::blocked_needs_attention);
}

// A confirmed live worker is not itself a conflict, and an unobserved profile
// keeps M4's durable-only reporting.  This is the guard that the new rule
// cannot turn every healthy running disk into a needs-attention one.
TEST_CASE(m5_confirmed_live_worker_and_unobserved_profiles_keep_durable_reporting) {
    auto snapshot = running_profile(product::PersistenceState::clean);

    WorkerRuntimeObservation live{};
    live.worker_alive = true;
    live.committed_generation = 5;
    snapshot.runtime_observation = live;
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::running);

    // No observation taken at all -- an adapter that does not participate, or a
    // store snapshot that never reached the read path.
    snapshot.runtime_observation.reset();
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::running);
}

// A generation the record has not caught up to yet is a normal in-flight
// checkpoint, not a conflict.  Failing closed on it would report a healthy disk
// as needing attention, so the rule deliberately ignores it.
TEST_CASE(m5_observed_generation_lead_is_not_a_liveness_conflict) {
    auto snapshot = running_profile(product::PersistenceState::clean);
    WorkerRuntimeObservation ahead{};
    ahead.worker_alive = true;
    ahead.committed_generation = snapshot.checkpoint.confirmed_generation + 1;
    ahead.dirty = true;
    ahead.dirty_epoch = snapshot.checkpoint.current_dirty_epoch + 1;
    snapshot.runtime_observation = ahead;
    REQUIRE(aggregate_status(reported_profile_state(snapshot))
        == AggregateStatus::running);
}

// A transitional runtime state is not a liveness claim: the worker is
// legitimately absent while starting and legitimately gone while stopping.
TEST_CASE(m5_transitional_runtime_states_are_not_liveness_claims) {
    for (const auto runtime : {product::RuntimeState::starting,
             product::RuntimeState::stopping}) {
        auto snapshot = running_profile(product::PersistenceState::clean);
        snapshot.state.disk.runtime = runtime;
        snapshot.runtime_observation = WorkerRuntimeObservation{};
        REQUIRE(aggregate_status(reported_profile_state(snapshot))
            != AggregateStatus::blocked_needs_attention);
    }
}

// The read path must actually consult the worker, not infer a value.  This
// proves disk list asks the injected control for every profile it reports and
// carries the answer into the response the wire projection will encode.
TEST_CASE(m5_disk_list_consults_the_worker_for_each_reported_profile) {
    Fixture fixture(product::PersistenceState::clean);

    WorkerRuntimeObservation live{};
    live.worker_alive = true;
    live.committed_generation = 7;
    fixture.worker.observation = live;

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto owner = fixture.profiles.snapshot().record.profile.owner_sid;
    auto response = core.dispatch(ServiceCommandRequest{
        uuid("51000000-0000-4000-8000-000000000001"),
        control::DiskListCommand{},
        caller(owner),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().profiles.size(), 1ULL);
    REQUIRE_EQ(fixture.worker.observed_profiles.size(), 1ULL);
    REQUIRE_EQ(fixture.worker.observed_profiles.front(),
        fixture.profiles.snapshot().record.profile.profile_uuid);
    const auto& reported = response.value().profiles.front();
    REQUIRE(reported.runtime_observation.has_value());
    REQUIRE(reported.runtime_observation.value().worker_alive);
    REQUIRE_EQ(reported.runtime_observation.value().committed_generation, 7ULL);
}

// End to end through the reporting rule: a record that says running, with the
// worker observed gone, reaches the caller as blocked/needs-attention rather
// than as a healthy disk.
TEST_CASE(m5_disk_list_reports_blocked_when_the_recorded_worker_is_gone) {
    Fixture fixture(product::PersistenceState::clean);
    // No observation set: the control authoritatively reports no live worker.
    fixture.worker.observation = WorkerRuntimeObservation{};

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto owner = fixture.profiles.snapshot().record.profile.owner_sid;
    auto response = core.dispatch(ServiceCommandRequest{
        uuid("51000000-0000-4000-8000-000000000002"),
        control::DiskListCommand{},
        caller(owner),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});

    REQUIRE(response.ok());
    const auto& reported = response.value().profiles.front();
    REQUIRE_EQ(reported.state.disk.runtime, product::RuntimeState::running);
    REQUIRE(aggregate_status(reported_profile_state(reported))
        == AggregateStatus::blocked_needs_attention);
}

// A failure to observe is not a licence to guess.  The read fails rather than
// reporting a profile whose worker state the service could not establish.
TEST_CASE(m5_disk_list_fails_closed_when_the_worker_cannot_be_observed) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.observation_failure = error(L"worker.observe");

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto owner = fixture.profiles.snapshot().record.profile.owner_sid;
    auto response = core.dispatch(ServiceCommandRequest{
        uuid("51000000-0000-4000-8000-000000000003"),
        control::DiskListCommand{},
        caller(owner),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});

    REQUIRE(!response.ok());
}
