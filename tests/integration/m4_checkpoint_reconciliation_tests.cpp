#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "../model/m4_checkpoint_operation_tests.cpp"

namespace {

CheckpointCommitBoundaryEvidence correlation(
    const Fixture& fixture,
    const product::Uuid& operation_id) {
    return {
        operation_id,
        fixture.profiles.snapshot().record.profile.profile_uuid,
        fixture.profiles.snapshot().record.profile.image_uuid,
        4242,
        9001,
        3,
        5,
        6,
        11,
        std::nullopt};
}

void persist_post_boundary(
    Fixture& fixture,
    const product::Uuid& operation_id,
    CheckpointDurablePhase phase,
    std::optional<CheckpointCommitDisposition> disposition = std::nullopt,
    bool final_clean = false,
    bool operation_phase_written = true) {
    auto operation = fixture.operations.load(operation_id);
    REQUIRE(operation.ok());
    auto proposed_operation = operation.value();
    ++proposed_operation.revision;
    proposed_operation.cancellable = false;
    proposed_operation.phase =
        phase == CheckpointDurablePhase::commit_boundary
            || !operation_phase_written
        ? "checkpoint-commit-boundary" : "checkpoint-evidence";
    REQUIRE(fixture.operations.write(
        operation.value().revision, proposed_operation).ok());

    const auto expected = fixture.profiles.snapshot();
    const auto noncancellable = mark_operation_noncancellable(
        expected.operation_gate, operation_id);
    REQUIRE(noncancellable.accepted());
    auto checkpoint = expected.checkpoint;
    checkpoint.phase = phase;
    checkpoint.correlation = correlation(fixture, operation_id);
    checkpoint.disposition = disposition;
    if (final_clean) checkpoint.confirmed_generation = 6;
    auto state = expected.state;
    if (final_clean) {
        const auto plan = plan_command(
            checkpoint.before_state.value(), ManagementCommand::checkpoint);
        const auto complete = complete_command(
            plan, CompletionOutcome::succeeded_clean);
        REQUIRE(complete.applied());
        state = complete.after;
    }
    REQUIRE(fixture.profiles.publish_checkpoint(
        expected, state, noncancellable.after, checkpoint).ok());
}

} // namespace

TEST_CASE(checkpoint_restart_after_p4_reconciles_without_replaying_checkpoint) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888881"));
        REQUIRE(queued.ok());
    }
    persist_post_boundary(
        fixture, operation_id, CheckpointDurablePhase::commit_boundary);
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
}

TEST_CASE(checkpoint_restart_before_p4_reexecutes_only_safe_precommit_work) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888880")).ok());
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 0U);
}

TEST_CASE(checkpoint_restart_after_gate_before_running_resumes_unique_owner) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.operations.failed_write_call = 1U;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto failed = first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888885"));
        REQUIRE(!failed.ok());
    }
    fixture.operations.failed_write_call.reset();

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
}

TEST_CASE(checkpoint_restart_orphan_without_gate_never_executes_worker) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888886")).ok());
    }
    const auto expected = fixture.profiles.snapshot();
    auto checkpoint = expected.checkpoint;
    checkpoint.phase = CheckpointDurablePhase::stable;
    checkpoint.before_state.reset();
    REQUIRE(fixture.profiles.publish_checkpoint(
        expected,
        checkpoint.before_state.value_or(
            running_profile(product::PersistenceState::dirty).state),
        idle_operation_gate(),
        checkpoint).ok());

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    auto orphan = fixture.operations.load(operation_id);
    REQUIRE(orphan.ok());
    REQUIRE_EQ(orphan.value().state, OperationState::failed);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(checkpoint_restart_restores_post_boundary_gate_before_reconciling) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888895")).ok());
    }
    persist_post_boundary(
        fixture, operation_id, CheckpointDurablePhase::commit_boundary);
    auto split_profile = fixture.profiles.snapshot();
    split_profile.operation_gate = idle_operation_gate();
    fixture.profiles.replace(std::move(split_profile));
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    auto durable = fixture.operations.load(operation_id);
    REQUIRE(durable.ok());
    REQUIRE(durable.value().state != OperationState::failed);
    REQUIRE_EQ(
        fixture.profiles.snapshot().operation_gate.active_operation_id,
        std::optional<product::Uuid>(operation_id));
    REQUIRE(!fixture.profiles.snapshot().operation_gate.cancellable);

    auto completed = restarted.execute_operation(operation_id);
    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
}

TEST_CASE(checkpoint_restart_after_p7_uses_durable_evidence_not_process_exit) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888882")).ok());
    }
    persist_post_boundary(
        fixture,
        operation_id,
        CheckpointDurablePhase::evidence_recorded,
        CheckpointCommitDisposition::committed,
        false,
        false);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
}

TEST_CASE(checkpoint_restart_pending_epoch_confirmation_never_exposes_clean) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888889")).ok());
    }
    persist_post_boundary(
        fixture,
        operation_id,
        CheckpointDurablePhase::committed_pending_epoch_confirmation,
        CheckpointCommitDisposition::committed);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
}

TEST_CASE(checkpoint_restart_after_final_truth_terminalizes_without_worker) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888883")).ok());
    }
    persist_post_boundary(
        fixture,
        operation_id,
        CheckpointDurablePhase::stable,
        CheckpointCommitDisposition::committed,
        true);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 0U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(checkpoint_missing_m3_proof_remains_reconciling) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888884")).ok());
    }
    persist_post_boundary(
        fixture, operation_id, CheckpointDurablePhase::commit_boundary);
    fixture.worker.fail_reconciliation = true;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::reconciling);
    REQUIRE_EQ(completed.value().phase, "checkpoint-resolving");
    REQUIRE(completed.value().terminal_result.empty());
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE(fixture.profiles.snapshot().checkpoint.reconciliation_failure
                .has_value());
    const auto& reconciliation_failure = fixture.profiles.snapshot()
        .checkpoint.reconciliation_failure.value();
    REQUIRE_EQ(reconciliation_failure.domain, ErrorDomain::internal);
    REQUIRE_EQ(
        reconciliation_failure.operation, std::wstring(L"worker.reconcile"));
    REQUIRE_EQ(
        reconciliation_failure.unresolved_disposition,
        std::optional<CheckpointCommitDisposition>{});
    REQUIRE(!fixture.profiles.snapshot().checkpoint.disposition.has_value());
    REQUIRE_EQ(
        reconciliation_failure.safe_context,
        std::string("worker checkpoint failed after durable P4 boundary"));
}

TEST_CASE(checkpoint_indeterminate_retries_reconciliation_after_next_restart) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888894")).ok());
    }
    persist_post_boundary(
        fixture, operation_id, CheckpointDurablePhase::commit_boundary);
    fixture.worker.fail_reconciliation = true;

    {
        ServiceCore first_restart(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first_restart.recover_after_restart().ok());
        auto unresolved = first_restart.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    }
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);

    fixture.worker.fail_reconciliation = false;
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;
    ServiceCore second_restart(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(second_restart.recover_after_restart().ok());
    auto completed = second_restart.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 2U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(checkpoint_restart_boundary_proven_not_committed_keeps_old_generation) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(fixture.request(
            "88888888-8888-4888-8888-888888888887")).ok());
    }
    persist_post_boundary(
        fixture, operation_id, CheckpointDurablePhase::commit_boundary);
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::not_committed;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
}

TEST_CASE(checkpoint_final_epoch_cas_conflict_cannot_report_success) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.worker.before_epoch_publish = [&] {
        fixture.profiles.before_checkpoint_publish = [](Profiles& profiles) {
            auto newer = profiles.snapshot();
            ++newer.checkpoint.current_dirty_epoch;
            newer.state.disk.persistence = product::PersistenceState::dirty;
            profiles.replace(std::move(newer));
        };
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(core.dispatch(fixture.request(
        "88888888-8888-4888-8888-888888888888")).ok());

    auto interrupted = core.execute_operation(operation_id);

    REQUIRE(!interrupted.ok());
    auto durable = fixture.operations.load(operation_id);
    REQUIRE(durable.ok());
    REQUIRE(durable.value().state != OperationState::succeeded);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());

    fixture.worker.before_epoch_publish = {};
    fixture.worker.live_dirty_epoch =
        fixture.profiles.snapshot().checkpoint.current_dirty_epoch;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);
    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
}
