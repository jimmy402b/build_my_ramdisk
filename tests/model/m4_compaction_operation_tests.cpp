#include "tests/support/test_framework.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

namespace {

std::uint64_t read_result_u64(
    const std::vector<std::byte>& result,
    std::size_t& offset) {
    REQUIRE(offset <= result.size());
    REQUIRE(result.size() - offset >= sizeof(std::uint64_t));
    std::uint64_t value{};
    for (unsigned shift = 0; shift != 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(result[offset++])) << shift;
    }
    return value;
}

} // namespace

TEST_CASE(compaction_clean_profile_persists_boundary_metrics_and_epoch_fence) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888881"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    REQUIRE_EQ(fixture.worker.calls.front(), ActionKind::compact);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_cut_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_epoch_publications, 1U);

    const auto& profile = fixture.profiles.snapshot();
    REQUIRE_EQ(profile.checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(profile.checkpoint.current_dirty_epoch, 11U);
    REQUIRE_EQ(profile.compaction.phase, CompactionDurablePhase::stable);
    REQUIRE(profile.compaction.correlation.has_value());
    REQUIRE(profile.compaction.correlation->cut_dirty_epoch_known);
    REQUIRE_EQ(
        profile.compaction.disposition,
        std::optional<CompactionCommitDisposition>(
            CompactionCommitDisposition::committed));
    REQUIRE(profile.compaction.metrics.has_value());
    REQUIRE_EQ(profile.compaction.metrics->source_generation, 5U);
    REQUIRE_EQ(profile.compaction.metrics->committed_generation, 6U);
    REQUIRE_EQ(profile.compaction.successful_compactions_total, 1U);
    REQUIRE_EQ(profile.compaction.checkpoints_since_compaction, 0U);
    REQUIRE_EQ(
        profile.state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE(!profile.operation_gate.active_operation_id.has_value());
}

TEST_CASE(compaction_dirty_profile_checkpoints_then_compacts_one_parent) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.live_dirty_epoch = 12;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888882"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 3U);
    REQUIRE_EQ(fixture.worker.calls[0], ActionKind::flush_exact_volume);
    REQUIRE_EQ(fixture.worker.calls[1], ActionKind::checkpoint);
    REQUIRE_EQ(fixture.worker.calls[2], ActionKind::compact);
    REQUIRE_EQ(fixture.worker.boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 7U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        0U);
    const auto checkpoint_counted = std::find_if(
        fixture.profiles.publications.begin(),
        fixture.profiles.publications.end(),
        [&](const ManagedProfileSnapshot& snapshot) {
            return snapshot.compaction.phase
                    == CompactionDurablePhase::checkpoint_confirmed
                && snapshot.compaction.checkpoints_since_compaction == 1
                && snapshot.checkpoint.confirmed_generation == 6;
        });
    REQUIRE(checkpoint_counted != fixture.profiles.publications.end());
    const auto& result = completed.value().terminal_result;
    std::size_t result_offset = 10U + 16U + 16U + 4U * 8U;
    REQUIRE_EQ(std::to_integer<unsigned char>(result[result_offset++]), 1U);
    REQUIRE_EQ(
        std::to_integer<unsigned char>(result[result_offset++]),
        static_cast<unsigned char>(
            CheckpointCommitDisposition::committed));
    REQUIRE_EQ(read_result_u64(result, result_offset), 6U);
}

TEST_CASE(compaction_prerequisite_checkpoint_failure_never_enters_compaction) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::not_committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888883"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 0U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        0U);
}

TEST_CASE(compaction_cancellation_before_mutation_boundary_creates_no_candidate) {
    Fixture fixture(product::PersistenceState::clean);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    bool cancellation_accepted = false;
    fixture.worker.before_compaction_boundary = [&] {
        cancellation_accepted = fixture.operations.request_cancel(
            operation_id, 1001).ok();
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888884"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE(cancellation_accepted);
    REQUIRE_EQ(completed.value().state, OperationState::cancelled);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.phase,
        CompactionDurablePhase::stable);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.disposition,
        std::optional<CompactionCommitDisposition>(
            CompactionCommitDisposition::not_committed));
}

TEST_CASE(compaction_write_after_cut_remains_dirty_at_candidate_generation) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.before_compaction_epoch_publish = [&] {
        fixture.worker.live_dirty_epoch = 12;
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888885"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.current_dirty_epoch, 12U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
}

TEST_CASE(compaction_worker_failure_after_boundary_parks_for_reconciliation) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.fail_after_compaction_boundary = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888886"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE_EQ(unresolved.value().phase, "compaction-resolving");
    REQUIRE(unresolved.value().terminal_result.empty());
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.phase,
        CompactionDurablePhase::resolving);
    REQUIRE(fixture.profiles.snapshot().compaction.correlation.has_value());
    REQUIRE(!fixture.profiles.snapshot().compaction.correlation
                 ->cut_dirty_epoch_known);
    REQUIRE(fixture.profiles.snapshot().compaction.reconciliation_failure
                .has_value());
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::commit_indeterminate);
}

TEST_CASE(compaction_mismatched_cut_proof_parks_without_commit_evidence) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.corrupt_compaction_cut = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888906"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_cut_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_epoch_publications, 0U);
    REQUIRE(fixture.profiles.snapshot().compaction.correlation.has_value());
    REQUIRE(!fixture.profiles.snapshot().compaction.correlation
                 ->cut_dirty_epoch_known);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
}

TEST_CASE(compaction_not_committed_preserves_source_and_checkpoint_counter) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.profiles.replace([&] {
        auto snapshot = fixture.profiles.snapshot();
        snapshot.compaction.checkpoints_since_compaction = 4;
        return snapshot;
    }());
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::not_committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888887"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        4U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.disposition,
        std::optional<CompactionCommitDisposition>(
            CompactionCommitDisposition::not_committed));
    REQUIRE(fixture.profiles.snapshot().compaction.failure.has_value());
}

TEST_CASE(compaction_not_committed_accepts_no_candidate_artifact_metrics) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::not_committed;
    fixture.worker.zero_not_committed_candidate_metrics = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888903"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_reconciliation_calls, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE(fixture.profiles.snapshot().compaction.metrics.has_value());
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.metrics
            ->committed_generation,
        0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.metrics
            ->candidate_base_bytes,
        0U);
}

TEST_CASE(compaction_cancellation_after_mutation_boundary_is_rejected) {
    Fixture fixture(product::PersistenceState::clean);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    bool cancellation_rejected = false;
    fixture.worker.after_compaction_boundary = [&] {
        cancellation_rejected = !fixture.operations.request_cancel(
            operation_id, 1001).ok();
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888888"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE(cancellation_rejected);
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
}

TEST_CASE(compaction_committed_needs_reconciliation_resolves_same_operation) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::committed_needs_reconciliation;
    fixture.worker.compaction_resolved_disposition =
        CompactionCommitDisposition::committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888889"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_reconciliation_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
}

TEST_CASE(compaction_indeterminate_without_resolution_retains_gate) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::commit_indeterminate;
    fixture.worker.fail_compaction_reconciliation = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888890"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE_EQ(fixture.worker.compaction_reconciliation_calls, 1U);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::commit_indeterminate);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
}

TEST_CASE(compaction_restart_reconciles_same_boundary_without_rebuilding) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::commit_indeterminate;
    fixture.worker.fail_compaction_reconciliation = true;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888891"));
        REQUIRE(queued.ok());
        auto unresolved = first.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    }

    fixture.worker.fail_compaction_reconciliation = false;
    fixture.worker.compaction_resolved_disposition =
        CompactionCommitDisposition::committed;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_reconciliation_calls, 2U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
}

TEST_CASE(compaction_restart_before_cut_proves_source_without_rebuilding) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.fail_after_compaction_boundary = true;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888907"));
        REQUIRE(queued.ok());
        auto unresolved = first.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
        REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
        REQUIRE_EQ(fixture.worker.compaction_cut_calls, 0U);
    }

    fixture.worker.fail_after_compaction_boundary = false;
    fixture.worker.compaction_resolved_disposition =
        CompactionCommitDisposition::not_committed;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_cut_calls, 0U);
    REQUIRE_EQ(fixture.worker.compaction_reconciliation_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(compaction_restart_reconciles_prerequisite_checkpoint_then_continues) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888898"));
        REQUIRE(queued.ok());
        auto unresolved = first.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
        REQUIRE_EQ(unresolved.value().phase, "checkpoint-resolving");
        REQUIRE_EQ(fixture.worker.commit_entries, 1U);
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction.phase,
            CompactionDurablePhase::checkpointing);
        REQUIRE_EQ(
            fixture.profiles.snapshot().checkpoint.phase,
            CheckpointDurablePhase::resolving);
    }

    fixture.worker.fail_after_boundary = false;
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
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 7U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        0U);
}

TEST_CASE(compaction_restart_restores_parent_gate_from_checkpoint_boundary) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888901"));
        REQUIRE(queued.ok());
        auto unresolved = first.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    }

    auto gate_lost = fixture.profiles.snapshot();
    gate_lost.operation_gate = idle_operation_gate();
    fixture.profiles.replace(std::move(gate_lost));
    fixture.worker.fail_after_boundary = false;
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        fixture.profiles.snapshot().operation_gate.active_operation_id,
        std::optional<product::Uuid>(operation_id));
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(compaction_restart_rejects_cancellable_checkpoint_boundary_record) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888902"));
        REQUIRE(queued.ok());
        auto unresolved = first.execute_operation(operation_id);
        REQUIRE(unresolved.ok());
        REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    }

    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    fixture.operations.records.front().cancellable = true;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(!recovered.ok());
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 0U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
}

TEST_CASE(compaction_restart_after_checkpoint_truth_continues_without_replay) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.profiles.fail_after_compaction_phase_once =
        CompactionDurablePhase::checkpoint_confirmed;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888899"));
        REQUIRE(queued.ok());
        auto interrupted = first.execute_operation(operation_id);
        REQUIRE(!interrupted.ok());
        REQUIRE_EQ(fixture.worker.commit_entries, 1U);
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction.phase,
            CompactionDurablePhase::checkpoint_confirmed);
        REQUIRE_EQ(
            fixture.profiles.snapshot().checkpoint.phase,
            CheckpointDurablePhase::stable);
        REQUIRE_EQ(
            fixture.profiles.snapshot().checkpoint.confirmed_generation,
            6U);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 0U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 7U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
}

TEST_CASE(compaction_restart_preserves_prerequisite_checkpoint_not_committed) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::not_committed;
    fixture.profiles.fail_after_compaction_phase_once =
        CompactionDurablePhase::stable;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888900"));
        REQUIRE(queued.ok());
        auto interrupted = first.execute_operation(operation_id);
        REQUIRE(!interrupted.ok());
        REQUIRE_EQ(fixture.worker.commit_entries, 1U);
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction.phase,
            CompactionDurablePhase::stable);
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction
                .prerequisite_checkpoint_disposition,
            std::optional<CheckpointCommitDisposition>(
                CheckpointCommitDisposition::not_committed));
    }

    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::committed;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 0U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
}

TEST_CASE(compaction_restart_after_profile_truth_only_terminalizes_operation) {
    Fixture fixture(product::PersistenceState::clean);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.worker.before_compaction_epoch_publish = [&] {
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 1;
    };
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888892"));
        REQUIRE(queued.ok());
        auto interrupted = first.execute_operation(operation_id);
        REQUIRE(!interrupted.ok());
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction
                .last_published_compaction_operation_id,
            std::optional<product::Uuid>(operation_id));
    }

    const auto entries_before = fixture.worker.compaction_entries;
    const auto reconciliations_before =
        fixture.worker.compaction_reconciliation_calls;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, entries_before);
    REQUIRE_EQ(
        fixture.worker.compaction_reconciliation_calls,
        reconciliations_before);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        1U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(compaction_cached_clean_revalidates_worker_epoch_and_checkpoints) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.live_dirty_epoch = 12;
    fixture.worker.checkpoint_cut_dirty_epoch = 12;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888893"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 3U);
    REQUIRE_EQ(fixture.worker.calls[1], ActionKind::checkpoint);
    REQUIRE_EQ(fixture.worker.calls[2], ActionKind::compact);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 7U);
}

TEST_CASE(compaction_prerequisite_checkpoint_cancellation_blocks_compaction) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.worker.before_boundary = [&] {
        REQUIRE(fixture.operations.request_cancel(operation_id, 1001).ok());
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888894"));
    REQUIRE(queued.ok());
    auto cancelled = core.execute_operation(operation_id);

    REQUIRE(cancelled.ok());
    REQUIRE_EQ(cancelled.value().state, OperationState::cancelled);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.phase,
        CompactionDurablePhase::stable);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction
            .prerequisite_checkpoint_disposition,
        std::optional<CheckpointCommitDisposition>(
            CheckpointCommitDisposition::not_committed));
}

TEST_CASE(compaction_prerequisite_checkpoint_indeterminate_blocks_compaction) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::commit_indeterminate;
    fixture.worker.fail_reconciliation = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888895"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
}

TEST_CASE(compaction_checkpoint_boundary_makes_parent_permanently_noncancellable) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    bool cancellation_rejected = false;
    fixture.worker.after_boundary = [&] {
        cancellation_rejected = !fixture.operations.request_cancel(
            operation_id, 1001).ok();
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888896"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE(cancellation_rejected);
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
}

TEST_CASE(compaction_low_space_defers_when_future_checkpoint_is_proven_possible) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_low_space = true;
    fixture.worker.future_checkpoint_possible = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888897"));
    REQUIRE(queued.ok());
    auto deferred = core.execute_operation(*queued.value().operation_id);

    REQUIRE(deferred.ok());
    REQUIRE_EQ(deferred.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.phase,
        CompactionDurablePhase::stable);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        0U);
}

TEST_CASE(compaction_low_space_blocks_only_with_future_checkpoint_impossible_proof) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_low_space = true;
    fixture.worker.future_checkpoint_possible = false;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888898"));
    REQUIRE(queued.ok());
    auto blocked = core.execute_operation(*queued.value().operation_id);

    REQUIRE(blocked.ok());
    REQUIRE_EQ(blocked.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::persistence_blocked);
}

TEST_CASE(compaction_dirty_checkpoint_then_deferred_keeps_checkpoint_count) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.compaction_low_space = true;
    fixture.worker.future_checkpoint_possible = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888904"));
    REQUIRE(queued.ok());
    auto deferred = core.execute_operation(*queued.value().operation_id);

    REQUIRE(deferred.ok());
    REQUIRE_EQ(deferred.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
}

TEST_CASE(compaction_restart_after_dirty_deferred_truth_does_not_repeat_checkpoint) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.compaction_low_space = true;
    fixture.worker.future_checkpoint_possible = true;
    fixture.profiles.fail_after_compaction_phase_once =
        CompactionDurablePhase::stable;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888906"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
        REQUIRE_EQ(fixture.worker.commit_entries, 1U);
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE_EQ(
            fixture.profiles.snapshot().compaction.phase,
            CompactionDurablePhase::stable);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        1U);
}

TEST_CASE(compaction_restart_after_clean_blocked_truth_does_not_repeat_preflight) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_low_space = true;
    fixture.worker.future_checkpoint_possible = false;
    fixture.profiles.fail_after_compaction_phase_once =
        CompactionDurablePhase::stable;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888911"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
        REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE(fixture.operations.load(operation_id).value().cancellable);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::persistence_blocked);
}

TEST_CASE(compaction_restart_repairs_operation_first_checkpoint_boundary_split) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.profiles.fail_before_checkpoint_phase_once =
        CheckpointDurablePhase::commit_boundary;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888907"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
        REQUIRE_EQ(fixture.worker.commit_entries, 0U);
        REQUIRE(!fixture.operations.load(operation_id).value().cancellable);
        REQUIRE(!fixture.profiles.snapshot().checkpoint.correlation.has_value());
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.boundary_calls, 2U);
}

TEST_CASE(compaction_restart_repairs_operation_first_compaction_boundary_split) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.profiles.fail_before_compaction_phase_once =
        CompactionDurablePhase::mutation_boundary;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "88888888-8888-4888-8888-888888888908"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
        REQUIRE(!fixture.operations.load(operation_id).value().cancellable);
        REQUIRE(!fixture.profiles.snapshot().compaction.correlation.has_value());
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_boundary_calls, 2U);
}

TEST_CASE(compaction_restart_rejects_unmarked_noncancellable_preboundary_record) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore first(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = first.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888912"));
    REQUIRE(queued.ok());
    fixture.operations.records.front().cancellable = false;
    fixture.operations.records.front().phase = "admitting";

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(!recovered.ok());
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(compaction_restart_rejects_malformed_operation_first_boundary_shape) {
    const auto rejected = [](
        const std::function<void(Fixture&)>& corrupt,
        std::string_view request_id) {
        Fixture fixture(product::PersistenceState::clean);
        fixture.profiles.fail_before_compaction_phase_once =
            CompactionDurablePhase::mutation_boundary;
        const auto operation_id =
            uuid("44444444-4444-4444-8444-444444444444");
        {
            ServiceCore first(
                fixture.clock, fixture.profiles, fixture.operations,
                fixture.worker, fixture.logger, fixture.ids,
                fixture.memory);
            auto queued = first.dispatch(
                fixture.compact_request(request_id));
            REQUIRE(queued.ok());
            REQUIRE(!first.execute_operation(operation_id).ok());
            REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
            REQUIRE_EQ(
                fixture.operations.load(operation_id).value().phase,
                std::string("compaction-mutation-boundary"));
        }
        corrupt(fixture);

        ServiceCore restarted(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto recovered = restarted.recover_after_restart();

        REQUIRE(!recovered.ok());
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    };

    rejected([](Fixture& fixture) {
        auto snapshot = fixture.profiles.snapshot();
        snapshot.operation_gate.cancellable = false;
        fixture.profiles.replace(std::move(snapshot));
    }, "88888888-8888-4888-8888-888888888913");
    rejected([](Fixture& fixture) {
        fixture.operations.records.front().cancel_requested = true;
    }, "88888888-8888-4888-8888-888888888914");
    rejected([](Fixture& fixture) {
        auto snapshot = fixture.profiles.snapshot();
        snapshot.state.disk.activity = product::ActivityState::idle;
        fixture.profiles.replace(std::move(snapshot));
    }, "88888888-8888-4888-8888-888888888915");
}

TEST_CASE(compaction_dirty_checkpoint_then_not_committed_keeps_checkpoint_count) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.compaction_initial_disposition =
        CompactionCommitDisposition::not_committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888905"));
    REQUIRE(queued.ok());
    auto failed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(failed.ok());
    REQUIRE_EQ(failed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.successful_compactions_total,
        0U);
}

TEST_CASE(compaction_same_request_replay_executes_worker_exactly_once) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto request = fixture.compact_request(
        "88888888-8888-4888-8888-888888888899");

    auto first = core.dispatch(request);
    auto replay = core.dispatch(request);

    REQUIRE(first.ok());
    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().operation_id, first.value().operation_id);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    REQUIRE(core.execute_operation(*first.value().operation_id).ok());
    REQUIRE_EQ(fixture.worker.compaction_entries, 1U);
}

TEST_CASE(compaction_manual_admission_is_per_profile_not_machine_global) {
    Fixture fixture(product::PersistenceState::clean);
    auto second_profile = running_profile(product::PersistenceState::clean);
    second_profile.record.profile.display_name = L"work2";
    second_profile.record.profile.profile_uuid =
        uuid("22222222-2222-4222-8222-222222222223");
    second_profile.record.profile.image_uuid =
        uuid("33333333-3333-4333-8333-333333333334");
    fixture.profiles.add_secondary(second_profile);
    fixture.ids.values.push_back(
        uuid("44444444-4444-4444-8444-444444444445"));
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto first = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888909"));
    const ServiceCommandRequest second_request{
        uuid("88888888-8888-4888-8888-888888888910"),
        control::DiskCompactCommand{L"work2"},
        caller(second_profile.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto second = core.dispatch(second_request);

    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.value().operation_id.has_value());
    REQUIRE(second.value().operation_id.has_value());
    REQUIRE(first.value().operation_id != second.value().operation_id);
    REQUIRE_EQ(fixture.operations.records.size(), 2U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().operation_gate.active_operation_id,
        first.value().operation_id);
    REQUIRE_EQ(
        fixture.profiles.secondary_snapshot().operation_gate
            .active_operation_id,
        second.value().operation_id);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(compaction_safe_result_contains_no_profile_or_bundle_path) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(fixture.compact_request(
        "88888888-8888-4888-8888-888888888900"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);
    REQUIRE(completed.ok());

    std::string result;
    result.reserve(completed.value().terminal_result.size());
    for (const auto value : completed.value().terminal_result) {
        result.push_back(static_cast<char>(
            std::to_integer<unsigned char>(value)));
    }
    REQUIRE(result.find("D:\\Images") == std::string::npos);
    REQUIRE(result.find("work.myrd") == std::string::npos);
    REQUIRE(result.find("base-") == std::string::npos);
    REQUIRE(result.find("journal-") == std::string::npos);

    const auto& bytes = completed.value().terminal_result;
    REQUIRE(bytes.size() >= 10U);
    REQUIRE_EQ(std::to_integer<unsigned char>(bytes[8]), 2U);
    REQUIRE_EQ(
        std::to_integer<unsigned char>(bytes[9]),
        static_cast<unsigned char>(CompactionCommitDisposition::committed));
    std::size_t offset = 10U + 16U + 16U;
    REQUIRE_EQ(read_result_u64(bytes, offset), 5U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 6U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 11U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 11U);
    REQUIRE_EQ(std::to_integer<unsigned char>(bytes[offset++]), 0U);
    REQUIRE_EQ(std::to_integer<unsigned char>(bytes[offset++]), 0xffU);
    REQUIRE_EQ(read_result_u64(bytes, offset), 5U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 4096U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 8192U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 12288U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 4096U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 32768U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 1ULL << 30);
    REQUIRE_EQ(read_result_u64(bytes, offset), 3U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 2U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 4U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 3U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 1U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 0U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 1U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 1U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 0U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 0U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 1U);
    REQUIRE_EQ(read_result_u64(bytes, offset), 0U);
    REQUIRE_EQ(offset, bytes.size());
}

TEST_CASE(compaction_invalid_public_states_never_reach_worker) {
    const auto rejected = [](
        const std::function<void(ManagedProfileSnapshot&)>& mutate,
        std::string_view request_id) {
        Fixture fixture(product::PersistenceState::clean);
        auto snapshot = fixture.profiles.snapshot();
        mutate(snapshot);
        fixture.profiles.replace(std::move(snapshot));
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto response = core.dispatch(fixture.compact_request(request_id));
        REQUIRE(!response.ok());
        REQUIRE(fixture.worker.calls.empty());
        REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    };

    rejected([](ManagedProfileSnapshot& snapshot) {
        snapshot.state.disk.runtime = product::RuntimeState::stopped;
    }, "99999999-9999-4999-8999-999999999901");
    rejected([](ManagedProfileSnapshot& snapshot) {
        snapshot.state.disk.activity = product::ActivityState::checkpointing;
    }, "99999999-9999-4999-8999-999999999902");
    rejected([](ManagedProfileSnapshot& snapshot) {
        snapshot.state.disk.persistence =
            product::PersistenceState::commit_indeterminate;
    }, "99999999-9999-4999-8999-999999999903");
    rejected([](ManagedProfileSnapshot& snapshot) {
        snapshot.state.disk.health = product::HealthState::needs_attention;
    }, "99999999-9999-4999-8999-999999999904");
}

TEST_CASE(compaction_source_generation_mismatch_fails_before_mutation) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.corrupt_compaction_boundary = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "99999999-9999-4999-8999-999999999905"));
    REQUIRE(queued.ok());
    auto failed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(failed.ok());
    REQUIRE_EQ(failed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.compaction_entries, 0U);
    REQUIRE(!fixture.profiles.snapshot().compaction.correlation.has_value());
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
}

TEST_CASE(compaction_unknown_disposition_is_rejected_before_durable_publication) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.compaction_initial_disposition =
        static_cast<CompactionCommitDisposition>(0xff);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.compact_request(
        "99999999-9999-4999-8999-999999999906"));
    REQUIRE(queued.ok());
    auto failed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(!failed.ok());
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.phase,
        CompactionDurablePhase::mutation_boundary);
    REQUIRE(!fixture.profiles.snapshot().compaction.disposition.has_value());
}

TEST_CASE(compaction_restart_rejects_unknown_stable_disposition) {
    Fixture fixture(product::PersistenceState::clean);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.worker.before_compaction_epoch_publish = [&] {
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 1;
    };
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.compact_request(
            "99999999-9999-4999-8999-999999999907"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
    }
    auto corrupted = fixture.profiles.snapshot();
    REQUIRE_EQ(
        corrupted.compaction.phase,
        CompactionDurablePhase::stable);
    corrupted.compaction.disposition =
        static_cast<CompactionCommitDisposition>(0xff);
    fixture.profiles.replace(std::move(corrupted));

    const auto worker_entries = fixture.worker.compaction_entries;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto rejected = restarted.execute_operation(operation_id);

    REQUIRE(!rejected.ok());
    REQUIRE_EQ(fixture.worker.compaction_entries, worker_entries);
}
