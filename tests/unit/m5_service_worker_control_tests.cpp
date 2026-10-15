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

    // What the last launch asked for.  The adapter's bundle actions are judged
    // partly on the request they build, and that request is the whole of what
    // the launch is for.
    std::optional<WorkerLaunchRequest> last_request;

    Result<std::unique_ptr<ServiceWorkerSession>> launch(
        const WorkerLaunchRequest& request,
        const OperationContext&) override {
        ++launches;
        last_request = request;
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            Error{ErrorDomain::internal, 1, L"launch", L"no worker here", 0});
    }
    Result<std::unique_ptr<ServiceWorkerSession>> attach(
        const product::Uuid&, const OperationContext&) override {
        ++attaches;
        return Result<std::unique_ptr<ServiceWorkerSession>>::success(nullptr);
    }
};

// `LifecycleOperationIntent` and `profile::Profile` both carry a `product::Uuid`
// with no public default constructor, so neither can be value-initialized --
// every case builds them through an aggregate, in the struct's own field order.
LifecycleOperationIntent intent(const LifecycleOperationKind kind) {
    return LifecycleOperationIntent{
        kind,
        profile_id(),
        kind == LifecycleOperationKind::create
            ? std::optional<product::Uuid>(
                  uuid("12345678-1234-1234-1234-123456789012"))
            : std::nullopt,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        L"C:\\bundles\\a",
        L"R:",
        kind == LifecycleOperationKind::create ? 64ull * 1024 * 1024 : 0,
        false,
        control::MemoryMode::automatic,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        PhysicalMemorySnapshot{},
        0};
}

WorkerActionRequest bundle_request(const ActionKind action,
                                   const LifecycleOperationKind kind) {
    return WorkerActionRequest{
        uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
        operation_id(),
        profile_id(),
        ManagementCommand::start,
        action,
        intent(kind),
        std::nullopt,
        std::nullopt};
}

profile::ProfileRecord profile_record(const profile::ProfileLifecycle lifecycle) {
    profile::Profile value{
        profile_id(),
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        lifecycle,
        L"C:\\bundles\\a",
        "disk.rdm",
        uuid("12345678-1234-1234-1234-123456789012"),
        64ull * 1024 * 1024,
        512,
        4096,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        profile::MountKind::drive_letter,
        L"R:",
        false,
        profile::MemoryPolicy{},
        profile::CheckpointPolicy{},
        profile::CompactionPolicy{}};
    return profile::ProfileRecord{7, std::move(value)};
}

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
        ActionKind::quiesce_mutations, ActionKind::freeze_writes};
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

TEST_CASE(m5_worker_control_bundle_actions_launch_from_the_intent) {
    // Neither action has a durable profile to launch from -- both run before
    // their profile is published, which is what made the plan's ordering
    // workable -- so the launch is built from the operation's intent.  The two
    // differ in the mode they ask for, and that mode is what decides whether the
    // worker creates a volume or adopts one.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);

    const auto created = control.execute(bundle_request(
        ActionKind::create_and_verify_bundle, LifecycleOperationKind::create));
    REQUIRE(!created.ok()); // the null provider cannot launch
    REQUIRE_EQ(provider.launches, 1);
    REQUIRE(provider.last_request.has_value());
    REQUIRE(provider.last_request->volume_mode
            == m5::WorkerVolumeMode::provision_new);
    REQUIRE(provider.last_request->image_id.has_value());
    REQUIRE_EQ(provider.last_request->logical_capacity_bytes,
               64ull * 1024 * 1024);
    // No durable record exists yet, so there is no revision to name.
    REQUIRE_EQ(provider.last_request->profile_revision, 0ull);

    const auto inspected = control.execute(bundle_request(
        ActionKind::inspect_bundle_read_only, LifecycleOperationKind::import));
    REQUIRE(!inspected.ok());
    REQUIRE_EQ(provider.launches, 2);
    REQUIRE(provider.last_request->volume_mode
            == m5::WorkerVolumeMode::adopt_existing);
    // An import names no image: it records what the bundle holds rather than
    // asserting one in advance.
    REQUIRE(!provider.last_request->image_id.has_value());
}

TEST_CASE(m5_worker_control_bundle_actions_need_their_intent) {
    // Without the intent there is nothing to launch from, and reporting success
    // would tell ServiceCore a bundle had been registered when nothing had.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    for (const auto action : {ActionKind::create_and_verify_bundle,
                              ActionKind::inspect_bundle_read_only}) {
        const auto result = control.execute(request(action));
        REQUIRE(!result.ok());
        REQUIRE_EQ(error_code_of(result.error()),
                   ServiceWorkerControlError::invalid_argument);
    }
    REQUIRE_EQ(provider.launches, 0);
}

TEST_CASE(m5_worker_control_bundle_actions_refuse_the_wrong_operation_kind) {
    // The source these actions record is what ServiceCore validates against the
    // operation's kind, so an action answering for the other kind must not even
    // launch: a create intent answered by an inspection would record a bundle it
    // never created as one it merely read.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const auto result = control.execute(bundle_request(
        ActionKind::create_and_verify_bundle, LifecycleOperationKind::import));
    REQUIRE(!result.ok());
    REQUIRE_EQ(error_code_of(result.error()),
               ServiceWorkerControlError::invalid_argument);
    REQUIRE_EQ(provider.launches, 0);
}

TEST_CASE(m5_launch_request_takes_its_mode_from_the_profile_lifecycle) {
    // The criterion is the one `append_start_actions` already branches on: a
    // profile whose lifecycle is still `provisioning` has a bundle and no volume,
    // so starting it must create one, and every other profile adopts the volume
    // it already has.  A second criterion invented here is how the two would come
    // to disagree.
    auto provisional =
        make_launch_request(profile_record(profile::ProfileLifecycle::provisioning));
    REQUIRE(provisional.ok());
    REQUIRE(provisional.value().volume_mode
            == m5::WorkerVolumeMode::provision_new);
    REQUIRE(provisional.value().image_id.has_value());
    REQUIRE_EQ(provisional.value().profile_revision, 7ull);

    auto ready = make_launch_request(profile_record(profile::ProfileLifecycle::ready));
    REQUIRE(ready.ok());
    REQUIRE(ready.value().volume_mode == m5::WorkerVolumeMode::adopt_existing);
}

TEST_CASE(m5_launch_request_refuses_a_mount_target_that_is_not_a_drive_letter) {
    // The worker takes a drive letter, so a target that is not one is refused
    // before a request exists -- and therefore before any process starts.  A
    // worker launched for the wrong mount is a worker holding the wrong disk.
    auto operation = intent(LifecycleOperationKind::create);
    operation.mount_target = L"not-a-drive-letter";
    REQUIRE(!make_launch_request(operation).ok());

    // The same rule for a durable record, which also has to say it is a
    // drive-letter mount at all.
    auto profile = profile_record(profile::ProfileLifecycle::ready);
    profile.profile.mount_target = L"not-a-drive-letter";
    REQUIRE(!make_launch_request(profile).ok());

    profile = profile_record(profile::ProfileLifecycle::ready);
    profile.profile.mount_kind = profile::MountKind::directory;
    REQUIRE(!make_launch_request(profile).ok());
}

TEST_CASE(m5_worker_control_refuses_a_correlation_that_cannot_prove_ownership) {
    // A correlation recorded before the schema minor that added the manifest
    // slot and the digest carries neither, and the adapter must say so rather
    // than answer.  The distinction that matters: a zero digest is the *absence*
    // of the fact the question is made of, so comparing it would answer a
    // different question -- and the answer it would give is "matches", which is
    // the one answer that must never be given here.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    const CheckpointCommitBoundaryEvidence legacy{
        operation_id(), profile_id(), uuid("12345678-1234-1234-1234-123456789012"),
        1, 2, 3, 4, 5, 6, 3903};
    REQUIRE(!legacy.recovery_recoverable());

    const auto result =
        control.reconcile_checkpoint(request(ActionKind::checkpoint), legacy);
    REQUIRE(!result.ok());
    REQUIRE_EQ(error_code_of(result.error()),
               ServiceWorkerControlError::recovery_not_provable);
    // Refused before a worker was asked: the question was unanswerable, not
    // unanswered.
    REQUIRE_EQ(provider.attaches, 0);
}

TEST_CASE(m5_worker_control_refuses_a_correlation_without_a_native_id) {
    // The slot and the digest are present but there is no native operation id,
    // so the bundle has nothing to match its journal against.  A proof taken
    // without it would be answering a different question with a confident
    // answer, which is worse than no answer.
    NullSessionProvider provider;
    ServiceWorkerControl control(provider);
    CheckpointCommitBoundaryEvidence correlation{
        operation_id(), profile_id(), uuid("12345678-1234-1234-1234-123456789012"),
        1, 2, 3, 4, 5, 6, std::nullopt, 2, {}};
    correlation.candidate_bytes_digest.fill(std::byte{0xAB});
    REQUIRE(correlation.recovery_recoverable());

    const auto result =
        control.reconcile_checkpoint(request(ActionKind::checkpoint), correlation);
    REQUIRE(!result.ok());
    REQUIRE_EQ(error_code_of(result.error()),
               ServiceWorkerControlError::recovery_not_provable);
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
