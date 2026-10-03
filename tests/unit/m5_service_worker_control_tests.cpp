#include "tests/support/test_framework.h"

#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_observation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace {
using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

product::Uuid profile_id() {
    return uuid("11111111-2222-3333-4444-555555555555");
}

product::Uuid operation_id() {
    return uuid("99999999-8888-7777-6666-555555555555");
}

// `WorkerActionRequest` holds three `product::Uuid`s, which have no public
// default constructor, so it cannot be value-initialized -- every case builds it
// through here.
WorkerActionRequest request(const ActionKind action) {
    return WorkerActionRequest{
        uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
        operation_id(),
        profile_id(),
        ManagementCommand::status_read,
        action,
        std::nullopt,
        std::nullopt,
        std::nullopt};
}

// Nothing here hands out a session: `ServiceWorkerSession` is a concrete type
// over a real authenticated channel and cannot be faked, so these cases cover
// the paths that must not need one.  What *does* need a worker -- the action
// mapping, the checkpoint boundary, the removal proof -- is covered against a
// real process by the production backend suite, and this file deliberately does
// not pretend otherwise.
struct NullSessionProvider final : IWorkerSessionProvider {
    int launches{};
    int attaches{};

    Result<std::unique_ptr<ServiceWorkerSession>> launch(
        const profile::ProfileRecord&, const OperationContext&) override {
        ++launches;
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            Error{ErrorDomain::internal, 1, L"launch", L"no worker here", 0});
    }
    Result<std::unique_ptr<ServiceWorkerSession>> attach(
        const product::Uuid&, const OperationContext&) override {
        ++attaches;
        return Result<std::unique_ptr<ServiceWorkerSession>>::success(nullptr);
    }
};

ServiceWorkerControlError error_code_of(const Error& error) {
    return static_cast<ServiceWorkerControlError>(error.code);
}

} // namespace

TEST_CASE(m5_worker_control_declares_no_cancellation_boundary) {
    // Declaring one here would narrow M4's cancellation semantics, which M5 may
    // not change: a checkpoint cancel is resolved at the pre-commit boundary
    // into a proven abort, and the stop removal is already forced
    // noncancellable by ServiceCore's own rule.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const ActionKind actions[] = {
        ActionKind::checkpoint,      ActionKind::flush_exact_volume,
        ActionKind::remove_mount,    ActionKind::stop_runtime_and_release_lock,
        ActionKind::quiesce_mutations};
    for (const auto action : actions) {
        REQUIRE(!control.begins_noncancellable(action));
    }
}

TEST_CASE(m5_worker_control_fails_closed_on_actions_no_worker_realizes) {
    // Reporting success for any of these would tell ServiceCore a mutation had
    // happened when nothing had -- which is the failure this adapter exists to
    // prevent.  Refused, and the worker is never contacted.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const ActionKind unrealized[] = {
        ActionKind::compact,
        ActionKind::publish_live_config,
        ActionKind::publish_stopped_config,
        ActionKind::verify_rebind_bundle,
        ActionKind::publish_rebind,
        ActionKind::forget_metadata,
        ActionKind::create_and_verify_bundle,
        ActionKind::inspect_bundle_read_only,
    };
    for (const auto action : unrealized) {
        const auto result = control.execute(request(action));
        REQUIRE(!result.ok());
        REQUIRE_EQ(error_code_of(result.error()),
                   ServiceWorkerControlError::unrealized_action);
    }
    REQUIRE_EQ(provider.launches, 0);
    REQUIRE_EQ(provider.attaches, 0);
}

TEST_CASE(m5_worker_control_reconcile_checkpoint_stays_fail_closed) {
    // Deliberately not overridden.  `recovery_proof` would realize it, but the
    // correlation needs the prepared candidate's manifest slot and bytes digest
    // and `CheckpointCommitBoundaryEvidence` carries neither -- so the adapter
    // could not build one from what it is handed.  The inherited answer is the
    // honest one.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    CheckpointCommitBoundaryEvidence correlation{
        operation_id(), profile_id(), uuid("12345678-1234-1234-1234-123456789012"),
        1, 2, 3, 4, 5, 6, std::nullopt};
    const auto result = control.reconcile_checkpoint(request(ActionKind::checkpoint),
                                                     correlation);
    REQUIRE(!result.ok());
}

TEST_CASE(m5_worker_control_prove_new_uninitialized_needs_the_durable_profile) {
    // A service-side preflight with no worker in it.  It still refuses without a
    // profile, because there is then no durable mount target to check and
    // reporting success would claim a check that never ran.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const auto refused =
        control.execute(request(ActionKind::prove_new_uninitialized));
    REQUIRE(!refused.ok());
    REQUIRE_EQ(error_code_of(refused.error()),
               ServiceWorkerControlError::invalid_argument);
    REQUIRE_EQ(provider.launches, 0);
}

TEST_CASE(m5_worker_control_observes_an_unbound_profile_as_gone) {
    // "No worker this authority can see" is an *observation* -- observed to be
    // absent -- and it is a stronger claim than "not observed".  Returning
    // nullopt would say the second, which the read path treats differently.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const auto observed = control.observe_runtime(profile_id());
    REQUIRE(observed.ok());
    REQUIRE(observed.value().has_value());
    REQUIRE(!observed.value()->worker_alive);
    // No session is held, so nothing is adopted to answer the question.
    REQUIRE_EQ(provider.attaches, 0);
}

TEST_CASE(m5_worker_control_strict_observation_refuses_when_unbound) {
    // Strict, unlike observe_runtime: a caller deciding whether a mutation is
    // safe must not read "could not ask" as "the runtime is clean".
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const auto strict = control.require_runtime_observation(profile_id());
    REQUIRE(!strict.ok());
    REQUIRE_EQ(error_code_of(strict.error()),
               ServiceWorkerControlError::execution_not_bound);
}

TEST_CASE(m5_observe_worker_runtime_combines_session_and_runtime_liveness) {
    // Both halves are needed and they are different facts: the session proves a
    // worker exists, the status says its runtime is up.  A worker that has
    // cleaned up keeps answering commands while reporting its runtime down, and
    // reporting that as a running disk is how a stopped profile reads as live.
    m5::WorkerRuntimeStatus status{};
    status.worker_alive = true;
    status.committed_generation = 7;
    status.dirty = true;
    status.dirty_epoch = 42;
    status.active_operation_id = operation_id();
    status.active_operation_phase = 3;

    const auto live = observe_worker_runtime(status, true);
    REQUIRE(live.worker_alive);
    REQUIRE_EQ(live.committed_generation, 7ull);
    REQUIRE(live.dirty);
    REQUIRE_EQ(live.dirty_epoch, 42ull);
    REQUIRE(live.active_operation_id.has_value());

    // The session answered, but the runtime behind it is gone.  The generation
    // and dirty truth are still the last real observations, so they are carried;
    // the active operation is not, because it is history rather than a fact
    // about what is running.
    m5::WorkerRuntimeStatus torn_down = status;
    torn_down.worker_alive = false;
    const auto dead = observe_worker_runtime(torn_down, true);
    REQUIRE(!dead.worker_alive);
    REQUIRE(!dead.active_operation_id.has_value());
    REQUIRE_EQ(dead.committed_generation, 7ull);
    REQUIRE(dead.dirty);

    // A status that never arrived proves nothing about a session either.
    const auto no_session = observe_worker_runtime(status, false);
    REQUIRE(!no_session.worker_alive);
    REQUIRE(!no_session.active_operation_id.has_value());
}
