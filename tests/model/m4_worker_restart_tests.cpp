#include "src/management/service_core.h"
#include "src/management/worker_supervisor.h"
#include "tests/support/m4_fake_clock.h"
#include "tests/support/test_framework.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

WorkerRuntimeIdentity identity() {
    return {
        uuid("11111111-1111-4111-8111-111111111111"),
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        0x123456789ULL,
        7,
        11,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

WorkerDeviceIdentity device() {
    return {
        9,
        8ULL << 30,
        3,
        0,
        2,
        0,
        uuid("33333333-3333-4333-8333-333333333333"),
        uuid("44444444-4444-4444-8444-444444444444"),
        uuid("55555555-5555-4555-8555-555555555555")};
}

Error fake_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 0x7701, operation, L"injected failure"};
}

WorkerFailureObservation observation() {
    return {
        identity(),
        true,
        true,
        true,
        identity().process_creation_time,
        true,
        0};
}

WorkerFailureObservation exited_observation() {
    auto value = observation();
    value.process_running = false;
    value.authenticated_transport_alive = false;
    return value;
}

class HistoryStore final : public IWorkerRestartHistoryStore {
public:
    Result<std::optional<WorkerRestartHistory>> load(
        const product::Uuid&) override {
        return Result<std::optional<WorkerRestartHistory>>::success(value);
    }

    Result<void> write_next(
        const product::Uuid&,
        const std::optional<std::uint64_t> expected_revision,
        const WorkerRestartHistory& next) override {
        const auto current_revision = value.has_value()
            ? std::optional<std::uint64_t>(value->revision)
            : std::nullopt;
        if (current_revision != expected_revision) {
            return Result<void>::failure(fake_error(L"history.cas"));
        }
        value = next;
        if (events != nullptr) events->push_back("admit");
        return Result<void>::success();
    }

    std::optional<WorkerRestartHistory> value;
    std::vector<std::string>* events{};
};

WorkerRestartAdmissionRequest admission(
    const char* operation,
    const std::uint64_t generation = 5,
    const std::uint64_t dirty_epoch = 91) {
    return {
        identity().profile_id,
        identity().image_id,
        uuid(operation),
        generation,
        dirty_epoch};
}

class RecoveryActions final : public IWorkerFailureRecoveryActions {
public:
    Result<WorkerCheckpointRecoveryProof> reconcile_interrupted_checkpoint(
        const WorkerRestartRecoveryRequest& request) override {
        events.push_back("checkpoint");
        if (checkpoint_error) {
            return Result<WorkerCheckpointRecoveryProof>::failure(
                fake_error(L"checkpoint"));
        }
        return Result<WorkerCheckpointRecoveryProof>::success({
            request.expected.profile_id,
            request.expected.image_id,
            request.recovery_operation_id,
            request.expected.profile_revision,
            request.expected.session_generation,
            request.observed_dirty_epoch,
            checkpoint_generation,
            checkpoint_resolved});
    }

    Result<WorkerResidualDeviceRecoveryProof> reconcile_residual_device(
        const WorkerRestartRecoveryRequest& request) override {
        events.push_back("residual");
        return Result<WorkerResidualDeviceRecoveryProof>::success({
            request.expected.profile_id,
            request.expected.image_id,
            tamper_residual_binding
                ? uuid("99999999-9999-4999-8999-999999999999")
                : request.recovery_operation_id,
            request.expected.profile_revision,
            request.expected.session_generation,
            request.expected_device,
            residual_disposition});
    }

    Result<WorkerRestartExecutionProof> ensure_restarted_from_checkpoint(
        const WorkerRestartRecoveryRequest& request,
        const std::uint64_t generation) override {
        events.push_back("restart");
        restarted_generation = generation;
        if (restart_error) {
            return Result<WorkerRestartExecutionProof>::failure(
                fake_error(L"restart"));
        }
        if (!launched_operation.has_value()
            || launched_operation.value() != request.recovery_operation_id) {
            launched_operation = request.recovery_operation_id;
            ++physical_launches;
        }
        auto worker = request.expected;
        if (!reuse_old_process_identity) {
            ++worker.worker_pid;
            ++worker.process_creation_time;
        }
        ++worker.session_generation;
        auto proof = WorkerRestartExecutionProof{
            request.expected.profile_id,
            request.expected.image_id,
            request.recovery_operation_id,
            generation,
            worker};
        if (tamper_restart_proof) ++proof.checkpoint_generation;
        return Result<WorkerRestartExecutionProof>::success(
            std::move(proof));
    }

    std::vector<std::string> events;
    std::uint64_t checkpoint_generation{6};
    std::uint64_t restarted_generation{};
    bool checkpoint_resolved{true};
    bool checkpoint_error{};
    bool restart_error{};
    bool tamper_restart_proof{};
    bool reuse_old_process_identity{};
    bool tamper_residual_binding{};
    std::size_t physical_launches{};
    std::optional<product::Uuid> launched_operation;
    WorkerResidualDeviceRecoveryDisposition residual_disposition{
        WorkerResidualDeviceRecoveryDisposition::absent};
};

WorkerRestartRecoveryRequest recovery_request(const char* operation) {
    return {
        identity(),
        device(),
        uuid(operation),
        5,
        91};
}

TEST_CASE(worker_failure_classifier_distinguishes_all_required_causes) {
    auto value = observation();
    REQUIRE_EQ(
        classify_worker_failure(value).kind,
        WorkerFailureKind::none);

    value = exited_observation();
    auto classified = classify_worker_failure(value);
    REQUIRE_EQ(classified.kind, WorkerFailureKind::process_exited);
    REQUIRE(classified.worker_exit_proven);

    value = observation();
    ++value.observed_process_creation_time;
    value.authenticated_transport_alive = false;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(classified.kind, WorkerFailureKind::pid_reused);
    REQUIRE(classified.worker_exit_proven);

    value = observation();
    value.authenticated_transport_alive = false;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(classified.kind, WorkerFailureKind::transport_lost);
    REQUIRE(!classified.worker_exit_proven);
    REQUIRE(!classified.automatic_kill_allowed);

    value = observation();
    value.elapsed_since_valid_heartbeat_ms = kWorkerNeedsAttentionAfterMs;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(classified.kind, WorkerFailureKind::live_stalled);
    REQUIRE(!classified.worker_exit_proven);
    REQUIRE_EQ(classified.health, product::HealthState::needs_attention);

    value = observation();
    value.process_query_succeeded = false;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(classified.kind, WorkerFailureKind::observation_indeterminate);
    REQUIRE(!classified.worker_exit_proven);

    value = observation();
    value.process_present = false;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(
        classified.kind,
        WorkerFailureKind::observation_indeterminate);
    REQUIRE(!classified.worker_exit_proven);

    value = observation();
    ++value.observed_process_creation_time;
    classified = classify_worker_failure(value);
    REQUIRE_EQ(
        classified.kind,
        WorkerFailureKind::observation_indeterminate);
    REQUIRE(!classified.worker_exit_proven);
}

TEST_CASE(worker_restart_limiter_enforces_exact_rolling_window) {
    tests::FakeServiceClock clock(1'000'000, 1'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    const std::array operations{
        "10000000-0000-4000-8000-000000000001",
        "10000000-0000-4000-8000-000000000002",
        "10000000-0000-4000-8000-000000000003"};
    for (const auto* operation : operations) {
        auto admitted = limiter.admit(admission(operation));
        REQUIRE(admitted.ok());
        REQUIRE_EQ(
            admitted.value().disposition,
            WorkerRestartAdmissionDisposition::admitted);
        clock.advance(1);
    }
    clock.set(1'000'000 + kWorkerRestartWindowMs - 1, 1'000);
    auto denied = limiter.admit(admission(
        "10000000-0000-4000-8000-000000000004"));
    REQUIRE(denied.ok());
    REQUIRE_EQ(
        denied.value().disposition,
        WorkerRestartAdmissionDisposition::limit_exhausted);

    clock.set(1'000'000 + kWorkerRestartWindowMs, 1'000);
    auto boundary = limiter.admit(admission(
        "10000000-0000-4000-8000-000000000004"));
    REQUIRE(boundary.ok());
    REQUIRE_EQ(
        boundary.value().disposition,
        WorkerRestartAdmissionDisposition::admitted);
    REQUIRE_EQ(store.value->attempts.size(), 3U);
}

TEST_CASE(worker_restart_history_survives_service_object_restart) {
    tests::FakeServiceClock clock(2'000'000, 2'000);
    HistoryStore store;
    {
        WorkerRestartLimiter first(clock, store);
        REQUIRE(first.admit(admission(
            "20000000-0000-4000-8000-000000000001")).ok());
        REQUIRE(first.admit(admission(
            "20000000-0000-4000-8000-000000000002")).ok());
        REQUIRE(first.admit(admission(
            "20000000-0000-4000-8000-000000000003")).ok());
    }
    WorkerRestartLimiter replacement(clock, store);
    auto fourth = replacement.admit(admission(
        "20000000-0000-4000-8000-000000000004"));
    REQUIRE(fourth.ok());
    REQUIRE_EQ(
        fourth.value().disposition,
        WorkerRestartAdmissionDisposition::limit_exhausted);
}

TEST_CASE(worker_restart_admission_is_idempotent_and_clock_rollback_safe) {
    tests::FakeServiceClock clock(3'000'000, 3'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    const auto request = admission(
        "30000000-0000-4000-8000-000000000001");
    REQUIRE(limiter.admit(request).ok());
    const auto revision = store.value->revision;
    auto replayed = limiter.admit(request);
    REQUIRE(replayed.ok());
    REQUIRE_EQ(
        replayed.value().disposition,
        WorkerRestartAdmissionDisposition::replayed);
    REQUIRE_EQ(store.value->revision, revision);
    REQUIRE_EQ(store.value->attempts.size(), 1U);

    clock.set(2'999'999, 3'000);
    auto rollback = limiter.admit(admission(
        "30000000-0000-4000-8000-000000000002"));
    REQUIRE(rollback.ok());
    REQUIRE_EQ(
        rollback.value().disposition,
        WorkerRestartAdmissionDisposition::clock_rollback);
    REQUIRE_EQ(store.value->attempts.size(), 1U);
}

TEST_CASE(worker_restart_replay_rejects_changed_generation_or_dirty_truth) {
    tests::FakeServiceClock clock(3'500'000, 3'500);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    auto request = admission(
        "35000000-0000-4000-8000-000000000001");
    REQUIRE(limiter.admit(request).ok());
    ++request.checkpoint_generation;
    REQUIRE(!limiter.admit(request).ok());
    --request.checkpoint_generation;
    ++request.observed_dirty_epoch;
    REQUIRE(!limiter.admit(request).ok());
    REQUIRE_EQ(store.value->attempts.size(), 1U);
}

TEST_CASE(worker_recovery_orders_checkpoint_residual_admission_and_restart) {
    tests::FakeServiceClock clock(4'000'000, 4'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    store.events = &actions.events;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    auto failed = exited_observation();

    auto recovered = coordinator.recover(
        recovery_request("40000000-0000-4000-8000-000000000001"),
        failed);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        recovered.value().disposition,
        WorkerRestartRecoveryDisposition::restarted);
    const std::vector<std::string> expected{
        "checkpoint", "residual", "admit", "restart"};
    REQUIRE_EQ(actions.events, expected);
    REQUIRE_EQ(actions.restarted_generation, 6U);
    REQUIRE(store.value->attempts.front().volatile_data_loss_possible);
    REQUIRE_EQ(store.value->attempts.front().checkpoint_generation, 6U);
}

TEST_CASE(worker_recovery_replay_ensures_only_one_physical_launch) {
    tests::FakeServiceClock clock(4'500'000, 4'500);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    const auto request = recovery_request(
        "45000000-0000-4000-8000-000000000001");
    const auto exited = exited_observation();

    REQUIRE(coordinator.recover(request, exited).ok());
    REQUIRE(coordinator.recover(request, exited).ok());
    REQUIRE_EQ(actions.physical_launches, 1U);
    REQUIRE_EQ(store.value->attempts.size(), 1U);
    const std::vector<std::string> expected{
        "checkpoint", "residual", "restart", "restart"};
    REQUIRE_EQ(actions.events, expected);
}

TEST_CASE(worker_recovery_never_mutates_for_live_or_unresolved_truth) {
    tests::FakeServiceClock clock(5'000'000, 5'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);

    auto live = observation();
    live.elapsed_since_valid_heartbeat_ms = kWorkerNeedsAttentionAfterMs;
    auto waiting = coordinator.recover(
        recovery_request("50000000-0000-4000-8000-000000000001"), live);
    REQUIRE(waiting.ok());
    REQUIRE_EQ(
        waiting.value().disposition,
        WorkerRestartRecoveryDisposition::needs_attention);
    REQUIRE(actions.events.empty());
    REQUIRE(!store.value.has_value());

    auto exited = exited_observation();
    actions.checkpoint_resolved = false;
    auto unresolved = coordinator.recover(
        recovery_request("50000000-0000-4000-8000-000000000002"),
        exited);
    REQUIRE(unresolved.ok());
    REQUIRE_EQ(
        unresolved.value().disposition,
        WorkerRestartRecoveryDisposition::needs_attention);
    REQUIRE_EQ(actions.events, std::vector<std::string>{"checkpoint"});
    REQUIRE(!store.value.has_value());
}

TEST_CASE(worker_recovery_stops_before_admission_on_residual_conflict) {
    tests::FakeServiceClock clock(6'000'000, 6'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    actions.residual_disposition =
        WorkerResidualDeviceRecoveryDisposition::needs_attention;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    auto exited = exited_observation();

    auto result = coordinator.recover(
        recovery_request("60000000-0000-4000-8000-000000000001"),
        exited);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        WorkerRestartRecoveryDisposition::needs_attention);
    const std::vector<std::string> expected{"checkpoint", "residual"};
    REQUIRE_EQ(actions.events, expected);
    REQUIRE(!store.value.has_value());
}

TEST_CASE(worker_recovery_rejects_stale_or_forged_action_proof) {
    tests::FakeServiceClock clock(6'500'000, 6'500);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    auto exited = exited_observation();

    actions.checkpoint_generation = 4;
    auto stale = coordinator.recover(
        recovery_request("65000000-0000-4000-8000-000000000001"),
        exited);
    REQUIRE(!stale.ok());
    REQUIRE_EQ(actions.events, std::vector<std::string>{"checkpoint"});
    REQUIRE(!store.value.has_value());

    actions.events.clear();
    actions.checkpoint_generation = 6;
    actions.residual_disposition =
        static_cast<WorkerResidualDeviceRecoveryDisposition>(0xff);
    auto forged = coordinator.recover(
        recovery_request("65000000-0000-4000-8000-000000000002"),
        exited);
    REQUIRE(!forged.ok());
    const std::vector<std::string> expected{"checkpoint", "residual"};
    REQUIRE_EQ(actions.events, expected);
    REQUIRE(!store.value.has_value());

    actions.events.clear();
    actions.residual_disposition =
        WorkerResidualDeviceRecoveryDisposition::absent;
    actions.tamper_residual_binding = true;
    auto stale_residual = coordinator.recover(
        recovery_request("65000000-0000-4000-8000-000000000004"),
        exited);
    REQUIRE(!stale_residual.ok());
    REQUIRE_EQ(actions.events, expected);
    REQUIRE(!store.value.has_value());
    actions.tamper_residual_binding = false;

    actions.events.clear();
    actions.residual_disposition =
        WorkerResidualDeviceRecoveryDisposition::absent;
    actions.tamper_restart_proof = true;
    auto bad_restart = coordinator.recover(
        recovery_request("65000000-0000-4000-8000-000000000003"),
        exited);
    REQUIRE(!bad_restart.ok());
    const std::vector<std::string> restart_expected{
        "checkpoint", "residual", "restart"};
    REQUIRE_EQ(actions.events, restart_expected);
    REQUIRE(store.value.has_value());
    REQUIRE_EQ(store.value->attempts.size(), 1U);

    HistoryStore second_store;
    WorkerRestartLimiter second_limiter(clock, second_store);
    RecoveryActions second_actions;
    second_actions.reuse_old_process_identity = true;
    WorkerFailureRecoveryCoordinator second_coordinator(
        second_limiter, second_actions);
    auto old_process = second_coordinator.recover(
        recovery_request("65000000-0000-4000-8000-000000000005"),
        exited);
    REQUIRE(!old_process.ok());
    REQUIRE(second_store.value.has_value());
}

TEST_CASE(worker_recovery_fourth_attempt_never_invokes_restart) {
    tests::FakeServiceClock clock(6'750'000, 6'750);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    REQUIRE(limiter.admit(admission(
        "67500000-0000-4000-8000-000000000001", 6)).ok());
    REQUIRE(limiter.admit(admission(
        "67500000-0000-4000-8000-000000000002", 6)).ok());
    REQUIRE(limiter.admit(admission(
        "67500000-0000-4000-8000-000000000003", 6)).ok());
    RecoveryActions actions;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    auto exited = exited_observation();

    auto result = coordinator.recover(
        recovery_request("67500000-0000-4000-8000-000000000004"),
        exited);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        WorkerRestartRecoveryDisposition::needs_attention);
    REQUIRE(actions.events.empty());
    REQUIRE_EQ(store.value->attempts.size(), 3U);
}

TEST_CASE(worker_restart_failure_consumes_one_durable_attempt) {
    tests::FakeServiceClock clock(7'000'000, 7'000);
    HistoryStore store;
    WorkerRestartLimiter limiter(clock, store);
    RecoveryActions actions;
    actions.restart_error = true;
    WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
    auto exited = exited_observation();

    auto result = coordinator.recover(
        recovery_request("70000000-0000-4000-8000-000000000001"),
        exited);
    REQUIRE(!result.ok());
    REQUIRE(store.value.has_value());
    REQUIRE_EQ(store.value->attempts.size(), 1U);
}

} // namespace
