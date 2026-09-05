#include "tests/support/test_framework.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

namespace {

ServiceCommandRequest stop_request(
    const Fixture& fixture,
    std::string_view request_id) {
    return {
        uuid(request_id),
        control::DiskStopCommand{L"work", false, false},
        caller(fixture.profiles.snapshot().record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
}

ipc::CallerIdentity elevated_admin(const std::wstring& sid) {
    auto identity = caller(sid);
    identity.administrator = true;
    identity.elevated = true;
    return identity;
}

ServiceCommandRequest force_stop_request(
    const Fixture& fixture,
    std::string_view request_id,
    bool force,
    bool accept_loss,
    bool admin) {
    const auto& owner = fixture.profiles.snapshot().record.profile.owner_sid;
    return {
        uuid(request_id),
        control::DiskStopCommand{L"work", force, accept_loss},
        admin ? elevated_admin(owner) : caller(owner),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
}

std::uint64_t result_u64(
    const std::vector<std::byte>& result,
    std::size_t offset) {
    REQUIRE(offset <= result.size());
    REQUIRE(result.size() - offset >= sizeof(std::uint64_t));
    std::uint64_t value{};
    for (unsigned shift = 0; shift != 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(result[offset++]))
            << shift;
    }
    return value;
}

void set_normal_stop_policy(
    Fixture& fixture,
    profile::NormalStopPolicy policy) {
    auto snapshot = fixture.profiles.snapshot();
    snapshot.record.profile.checkpoint.normal_stop = policy;
    fixture.profiles.replace(std::move(snapshot));
}

void set_stopped(Fixture& fixture) {
    auto snapshot = fixture.profiles.snapshot();
    snapshot.state = registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
    snapshot.operation_gate = idle_operation_gate();
    fixture.profiles.replace(std::move(snapshot));
}

ServiceCommandRequest rebind_request(
    const Fixture& fixture,
    std::string_view request_id,
    product::Uuid expected_image_id) {
    return {
        uuid(request_id),
        control::DiskRebindCommand{
            fixture.profiles.snapshot().record.profile.profile_uuid,
            L"E:\\Moved\\work.myrd",
            expected_image_id},
        elevated_admin(
            fixture.profiles.snapshot().record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
}

ServiceCommandRequest repair_request(
    const Fixture& fixture,
    std::string_view request_id) {
    return {
        uuid(request_id),
        control::DiskRepairProfileCommand{
            fixture.profiles.snapshot().record.profile.profile_uuid,
            true},
        elevated_admin(
            fixture.profiles.snapshot().record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
}

ServiceCommandRequest forget_request(
    const Fixture& fixture,
    std::string_view request_id,
    product::Uuid confirmation_id,
    bool administrator = true) {
    const auto& profile = fixture.profiles.snapshot().record.profile;
    return {
        uuid(request_id),
        control::DiskForgetCommand{
            profile.profile_uuid,
            confirmation_id},
        administrator ? elevated_admin(profile.owner_sid)
                      : caller(profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
}

std::uint32_t result_u32(
    const std::vector<std::byte>& result,
    std::size_t offset) {
    REQUIRE(offset <= result.size());
    REQUIRE(result.size() - offset >= sizeof(std::uint32_t));
    std::uint32_t value{};
    for (unsigned shift = 0; shift != 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned char>(result[offset++]))
            << shift;
    }
    return value;
}

std::wstring result_utf16(
    const std::vector<std::byte>& result,
    std::size_t offset,
    std::size_t code_units) {
    REQUIRE(offset <= result.size());
    REQUIRE(code_units <= (result.size() - offset) / 2U);
    std::wstring value;
    value.reserve(code_units);
    for (std::size_t index = 0; index != code_units; ++index) {
        const auto low = std::to_integer<unsigned char>(result[offset++]);
        const auto high = std::to_integer<unsigned char>(result[offset++]);
        value.push_back(static_cast<wchar_t>(low | (high << 8U)));
    }
    return value;
}

void rewind_to_operation_only_admission(Fixture& fixture) {
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    auto& record = fixture.operations.records.front();
    record.revision = 1;
    record.state = OperationState::queued;
    record.phase = "queued";
    record.updated_unix_ms = record.created_unix_ms;
    record.terminal_unix_ms.reset();
    record.cancellable = true;
    record.cancel_requested = false;
    record.terminal_result.clear();
    fixture.operations.history.clear();
}

void require_operation_only_admission_terminalized(Fixture& fixture) {
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    const auto& record = fixture.operations.records.front();
    REQUIRE_EQ(record.state, OperationState::failed);
    REQUIRE_EQ(record.phase, std::string("terminal"));
    REQUIRE(record.terminal_unix_ms.has_value());
    REQUIRE(!record.cancellable);
    REQUIRE(!record.terminal_result.empty());
}

} // namespace

TEST_CASE(normal_stop_dirty_profile_quiesces_checkpoints_then_tears_down) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa1-aaaa-4aaa-8aaa-aaaaaaaaaaa1"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::checkpoint,
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::stable);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(normal_stop_clean_profile_revalidates_and_skips_checkpoint) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa2-aaaa-4aaa-8aaa-aaaaaaaaaaa2"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
}

TEST_CASE(normal_stop_false_clean_epoch_runs_checkpoint_before_removal) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.live_dirty_epoch = 12;
    fixture.worker.checkpoint_cut_dirty_epoch = 12;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa3-aaaa-4aaa-8aaa-aaaaaaaaaaa3"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls[2], ActionKind::checkpoint);
    REQUIRE_EQ(fixture.worker.calls[3], ActionKind::remove_mount);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
}

TEST_CASE(normal_stop_require_clean_refuses_dirty_and_resumes_mutations) {
    Fixture fixture(product::PersistenceState::dirty);
    set_normal_stop_policy(fixture, profile::NormalStopPolicy::require_clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa4-aaaa-4aaa-8aaa-aaaaaaaaaaa4"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::resume_mutations}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::running);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::stable);
}

TEST_CASE(normal_stop_flush_failure_rolls_back_without_removal) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_action_once = ActionKind::flush_exact_volume;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa5-aaaa-4aaa-8aaa-aaaaaaaaaaa5"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::resume_mutations}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::running);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::stable);
}

TEST_CASE(normal_stop_checkpoint_not_committed_resumes_without_removal) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::not_committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa7-aaaa-4aaa-8aaa-aaaaaaaaaaa7"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::checkpoint,
            ActionKind::resume_mutations}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::running);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(normal_stop_checkpoint_preboundary_failure_resumes_without_removal) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_before_boundary = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaa10-aaaa-4aaa-8aaa-aaaaaaaaaa10"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::checkpoint,
            ActionKind::resume_mutations}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::running);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::stable);
}

TEST_CASE(normal_stop_checkpoint_failure_after_boundary_stays_reconciling) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa8-aaaa-4aaa-8aaa-aaaaaaaaaaa8"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::reconciling);
    REQUIRE_EQ(completed.value().phase, "stop-reconciling");
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopping);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.activity,
        product::ActivityState::resolving_commit);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
}

TEST_CASE(normal_stop_quiesce_failure_never_removes_or_leaves_gate) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_action_once = ActionKind::quiesce_mutations;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa9-aaaa-4aaa-8aaa-aaaaaaaaaaa9"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.worker.calls,
               std::vector<ActionKind>({ActionKind::quiesce_mutations}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::running);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(normal_stop_failure_after_removal_boundary_stays_reconciling) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.fail_action_once = ActionKind::remove_mount;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(stop_request(
        fixture, "aaaaaaa6-aaaa-4aaa-8aaa-aaaaaaaaaaa6"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::reconciling);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::remove_mount}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopping);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::removal_boundary);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
}

TEST_CASE(normal_stop_post_boundary_failures_never_replay_prior_teardown) {
    const auto run = [](ActionKind failing_action,
                        StopDurablePhase expected_phase,
                        std::string_view request_id) {
        Fixture fixture(product::PersistenceState::clean);
        fixture.worker.fail_action_once = failing_action;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);

        auto queued = core.dispatch(stop_request(fixture, request_id));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(*queued.value().operation_id);

        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::reconciling);
        REQUIRE_EQ(completed.value().phase, "stop-reconciling");
        REQUIRE_EQ(fixture.profiles.snapshot().stop.phase, expected_phase);
        REQUIRE_EQ(
            fixture.profiles.snapshot().state.disk.runtime,
            product::RuntimeState::stopping);
        REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                    .has_value());
        REQUIRE(std::find(
            fixture.worker.calls.begin(), fixture.worker.calls.end(),
            ActionKind::resume_mutations) == fixture.worker.calls.end());
    };

    run(
        ActionKind::remove_exact_device,
        StopDurablePhase::mount_removed,
        "aaaaaa11-aaaa-4aaa-8aaa-aaaaaaaaaa11");
    run(
        ActionKind::stop_runtime_and_release_lock,
        StopDurablePhase::device_removed,
        "aaaaaa12-aaaa-4aaa-8aaa-aaaaaaaaaa12");
}

TEST_CASE(normal_stop_rejects_unknown_persisted_policy_before_worker_io) {
    Fixture fixture(product::PersistenceState::clean);
    set_normal_stop_policy(
        fixture, static_cast<profile::NormalStopPolicy>(255));
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto rejected = core.dispatch(stop_request(
        fixture, "aaaaaa13-aaaa-4aaa-8aaa-aaaaaaaaaa13"));

    REQUIRE(!rejected.ok());
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE(fixture.operations.records.empty());
}

TEST_CASE(force_stop_requires_both_confirmations_before_admission) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto force_only = core.dispatch(force_stop_request(
        fixture, "bbbbbbb1-bbbb-4bbb-8bbb-bbbbbbbbbbb1",
        true, false, true));
    auto acceptance_only = core.dispatch(force_stop_request(
        fixture, "bbbbbbb2-bbbb-4bbb-8bbb-bbbbbbbbbbb2",
        false, true, true));

    REQUIRE(!force_only.ok());
    REQUIRE(!acceptance_only.ok());
    REQUIRE(fixture.operations.records.empty());
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(force_stop_requires_elevated_administrator_not_only_owner) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto rejected = core.dispatch(force_stop_request(
        fixture, "bbbbbbb3-bbbb-4bbb-8bbb-bbbbbbbbbbb3",
        true, true, false));

    REQUIRE(!rejected.ok());
    REQUIRE(fixture.operations.records.empty());
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(force_stop_persists_ack_skips_checkpoint_and_reports_loss_window) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.live_dirty_epoch = 12;
    const auto original_profile = fixture.profiles.snapshot().record;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(force_stop_request(
        fixture, "bbbbbbb4-bbbb-4bbb-8bbb-bbbbbbbbbbb4",
        true, true, true));

    REQUIRE(queued.ok());
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::checkpoint_confirmed);
    REQUIRE(fixture.profiles.snapshot().stop.force);
    REQUIRE(fixture.profiles.snapshot().stop.volatile_data_loss_acknowledged);
    REQUIRE(fixture.profiles.snapshot().stop.volatile_data_loss_possible);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.confirmed_generation_before, 5U);
    REQUIRE_EQ(fixture.profiles.snapshot().stop.dirty_epoch_before, 11U);

    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(fixture.profiles.snapshot().record, original_profile);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.health,
        product::HealthState::degraded);
    const auto& result = completed.value().terminal_result;
    REQUIRE_EQ(result.size(), 44U);
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'F'}, std::byte{'S'}, std::byte{'T'}, std::byte{'P'}};
    REQUIRE(std::equal(magic.begin(), magic.end(), result.begin()));
    REQUIRE_EQ(result[8], std::byte{1});
    REQUIRE_EQ(result[9], std::byte{1});
    REQUIRE_EQ(result[10], std::byte{1});
    REQUIRE_EQ(
        result[11],
        static_cast<std::byte>(product::PersistenceState::dirty));
    REQUIRE(std::equal(
        original_profile.profile.image_uuid.bytes().begin(),
        original_profile.profile.image_uuid.bytes().end(),
        result.begin() + 12));
    REQUIRE_EQ(result_u64(result, 28), 5U);
    REQUIRE_EQ(result_u64(result, 36), 12U);
}

TEST_CASE(force_stop_teardown_failure_retains_durable_ack_and_gate) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_action_once = ActionKind::remove_exact_device;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(force_stop_request(
        fixture, "bbbbbbb5-bbbb-4bbb-8bbb-bbbbbbbbbbb5",
        true, true, true));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::reconciling);
    REQUIRE(fixture.profiles.snapshot().stop.force);
    REQUIRE(fixture.profiles.snapshot().stop.volatile_data_loss_acknowledged);
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::mount_removed);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
}

TEST_CASE(rebind_stopped_profile_inspects_read_only_then_revision_cas) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot().record;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(rebind_request(
        fixture, "ccccccc1-cccc-4ccc-8ccc-ccccccccccc1",
        before.profile.image_uuid));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls,
               std::vector<ActionKind>({ActionKind::verify_rebind_bundle}));
    REQUIRE_EQ(fixture.profiles.rebind_calls, 1U);
    const auto& after = fixture.profiles.snapshot().record;
    REQUIRE_EQ(after.revision, before.revision + 1);
    REQUIRE_EQ(after.profile.bundle_path, L"E:\\Moved\\work.myrd");
    REQUIRE_EQ(after.profile.image_uuid, before.profile.image_uuid);
    REQUIRE_EQ(after.profile.logical_capacity_bytes,
               before.profile.logical_capacity_bytes);
    REQUIRE_EQ(after.profile.logical_sector_size,
               before.profile.logical_sector_size);
    REQUIRE_EQ(after.profile.physical_sector_size,
               before.profile.physical_sector_size);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(rebind_rejects_running_wrong_image_and_inspection_mismatch) {
    {
        Fixture fixture(product::PersistenceState::clean);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto rejected = core.dispatch(rebind_request(
            fixture, "ccccccc2-cccc-4ccc-8ccc-ccccccccccc2",
            fixture.profiles.snapshot().record.profile.image_uuid));
        REQUIRE(!rejected.ok());
        REQUIRE(fixture.worker.calls.empty());
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto rejected = core.dispatch(rebind_request(
            fixture, "ccccccc3-cccc-4ccc-8ccc-ccccccccccc3",
            uuid("99999999-9999-4999-8999-999999999999")));
        REQUIRE(!rejected.ok());
        REQUIRE(fixture.worker.calls.empty());
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        const auto before = fixture.profiles.snapshot().record;
        fixture.worker.rebind_evidence = BundleRegistrationEvidence{
            uuid("99999999-9999-4999-8999-999999999999"),
            before.profile.manifest_name,
            before.profile.logical_capacity_bytes,
            before.profile.logical_sector_size,
            before.profile.physical_sector_size,
            kProductPayloadBlockBytes,
            BundleFileIdentity{9, {}},
            BundleEvidenceSource::read_only_inspection};
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(rebind_request(
            fixture, "ccccccc4-cccc-4ccc-8ccc-ccccccccccc4",
            before.profile.image_uuid));
        REQUIRE(queued.ok());
        auto failed = core.execute_operation(*queued.value().operation_id);
        REQUIRE(failed.ok());
        REQUIRE_EQ(failed.value().state, OperationState::failed);
        REQUIRE_EQ(fixture.profiles.snapshot().record, before);
        REQUIRE_EQ(fixture.profiles.rebind_calls, 0U);
    }
}

TEST_CASE(rebind_revision_cas_failure_preserves_original_binding) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot().record;
    fixture.profiles.fail_rebind_cas = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(rebind_request(
        fixture, "ccccccc5-cccc-4ccc-8ccc-ccccccccccc5",
        before.profile.image_uuid));
    REQUIRE(queued.ok());
    auto failed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(failed.ok());
    REQUIRE_EQ(failed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.profiles.snapshot().record, before);
}

TEST_CASE(repair_profile_uses_previous_specific_store_path_without_worker_io) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    auto previous = fixture.profiles.snapshot();
    previous.record.revision = 6;
    previous.state.disk.health = product::HealthState::needs_attention;
    const auto original_binding = previous.record.profile;
    fixture.profiles.repair_candidate = previous;
    fixture.profiles.fail_current_load = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(repair_request(
        fixture, "ccccccc6-cccc-4ccc-8ccc-ccccccccccc6"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.repair_candidate_loads, 2U);
    REQUIRE_EQ(fixture.profiles.repair_calls, 1U);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 7U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().record.profile, original_binding);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(forget_stopped_profile_reserves_tombstone_proves_absence_and_removes_only_metadata) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot();
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(forget_request(
        fixture, "ddddddd1-dddd-4ddd-8ddd-ddddddddddd1",
        before.record.profile.profile_uuid));

    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    REQUIRE_EQ(fixture.profiles.forget_reserve_calls, 1U);
    REQUIRE(fixture.profiles.forget_tombstone.has_value());
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->operation_id,
        queued.value().operation_id.value());
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->profile_id,
        before.record.profile.profile_uuid);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->image_id,
        before.record.profile.image_uuid);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->profile_revision,
        before.record.revision);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->confirmed_generation,
        before.checkpoint.confirmed_generation);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->retained_bundle_path,
        before.record.profile.bundle_path);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->authorization_owner_sid,
        before.record.profile.owner_sid);
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->retained_bundle_identity,
        std::optional<BundleFileIdentity>(test_bundle_file_identity()));
    REQUIRE(!fixture.profiles.forgotten);

    auto completed = core.execute_operation(
        queued.value().operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    REQUIRE(fixture.profiles.forgotten);
    REQUIRE(!fixture.profiles.load(before.record.profile.profile_uuid).ok());

    const auto& result = completed.value().terminal_result;
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'F'}, std::byte{'R'}, std::byte{'G'}, std::byte{'T'}};
    REQUIRE(result.size() >= 64U);
    REQUIRE(std::equal(magic.begin(), magic.end(), result.begin()));
    REQUIRE_EQ(result_u32(result, 8), 1U);
    REQUIRE(std::equal(
        before.record.profile.profile_uuid.bytes().begin(),
        before.record.profile.profile_uuid.bytes().end(),
        result.begin() + 12));
    REQUIRE(std::equal(
        before.record.profile.image_uuid.bytes().begin(),
        before.record.profile.image_uuid.bytes().end(),
        result.begin() + 28));
    REQUIRE_EQ(result_u64(result, 44), before.checkpoint.confirmed_generation);
    REQUIRE_EQ(result_u64(result, 52), before.record.revision);
    const auto path_length = result_u32(result, 60);
    REQUIRE_EQ(result.size(), 64U + path_length * 2U);
    REQUIRE_EQ(
        result_utf16(result, 64, path_length),
        before.record.profile.bundle_path);
}

TEST_CASE(forget_refuses_every_ambiguous_absence_or_identity_proof_before_removal) {
    const auto run = [](ForgetSafetyEvidence proof,
                        std::string_view request_id) {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        const auto before = fixture.profiles.snapshot();
        fixture.worker.forget_proof = proof;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);

        auto queued = core.dispatch(forget_request(
            fixture, request_id, before.record.profile.profile_uuid));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());

        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::failed);
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
        REQUIRE_EQ(fixture.profiles.forget_remove_calls, 0U);
        REQUIRE(!fixture.profiles.forgotten);
        REQUIRE(fixture.profiles.load(
            before.record.profile.profile_uuid).ok());
        REQUIRE(!fixture.profiles.snapshot().operation_gate
                     .active_operation_id.has_value());
    };

    Fixture reference(product::PersistenceState::clean);
    set_stopped(reference);
    const auto& record = reference.profiles.snapshot().record;
    const ForgetSafetyEvidence exact{
        record.profile.profile_uuid,
        record.profile.image_uuid,
        record.revision,
        true,
        true,
        true,
        true,
        test_bundle_file_identity()};

    auto worker_present = exact;
    worker_present.worker_absent = false;
    run(worker_present, "ddddddd2-dddd-4ddd-8ddd-ddddddddddd2");

    auto lock_present = exact;
    lock_present.writer_lock_absent = false;
    run(lock_present, "ddddddd3-dddd-4ddd-8ddd-ddddddddddd3");

    auto device_present = exact;
    device_present.matching_device_absent = false;
    run(device_present, "ddddddd4-dddd-4ddd-8ddd-ddddddddddd4");

    auto wrong_profile = exact;
    wrong_profile.profile_id =
        uuid("99999991-9999-4999-8999-999999999991");
    run(wrong_profile, "ddddddd5-dddd-4ddd-8ddd-ddddddddddd5");

    auto wrong_image = exact;
    wrong_image.image_id =
        uuid("99999992-9999-4999-8999-999999999992");
    run(wrong_image, "ddddddd6-dddd-4ddd-8ddd-ddddddddddd6");

    auto wrong_revision = exact;
    ++wrong_revision.profile_revision;
    run(wrong_revision, "ddddddd7-dddd-4ddd-8ddd-ddddddddddd7");

    auto bundle_missing = exact;
    bundle_missing.bundle_present = false;
    run(bundle_missing, "dddddd71-dddd-4ddd-8ddd-dddddddddd71");

    auto bundle_replaced = exact;
    bundle_replaced.retained_bundle_identity->file_id[0] = std::byte{0x55};
    run(bundle_replaced, "dddddd72-dddd-4ddd-8ddd-dddddddddd72");
}

TEST_CASE(forget_requires_stopped_profile_exact_confirmation_and_elevated_admin) {
    {
        Fixture fixture(product::PersistenceState::clean);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        const auto profile_id =
            fixture.profiles.snapshot().record.profile.profile_uuid;

        auto rejected = core.dispatch(forget_request(
            fixture, "ddddddd8-dddd-4ddd-8ddd-ddddddddddd8", profile_id));

        REQUIRE(!rejected.ok());
        REQUIRE(fixture.operations.records.empty());
        REQUIRE_EQ(fixture.profiles.forget_reserve_calls, 0U);
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto rejected = core.dispatch(forget_request(
            fixture, "ddddddd9-dddd-4ddd-8ddd-ddddddddddd9",
            uuid("99999993-9999-4999-8999-999999999993")));

        REQUIRE(!rejected.ok());
        REQUIRE(fixture.operations.records.empty());
        REQUIRE_EQ(fixture.profiles.forget_reserve_calls, 0U);
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        const auto profile_id =
            fixture.profiles.snapshot().record.profile.profile_uuid;
        auto rejected = core.dispatch(forget_request(
            fixture, "dddddd10-dddd-4ddd-8ddd-dddddddddd10",
            profile_id, false));

        REQUIRE(!rejected.ok());
        REQUIRE(fixture.operations.records.empty());
        REQUIRE_EQ(fixture.profiles.forget_reserve_calls, 0U);
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    }
}

TEST_CASE(forget_missing_profile_remains_not_found_without_admission) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto missing =
        uuid("99999994-9999-4999-8999-999999999994");
    const ServiceCommandRequest request{
        uuid("dddddd11-dddd-4ddd-8ddd-dddddddddd11"),
        control::DiskForgetCommand{missing, missing},
        elevated_admin(
            fixture.profiles.snapshot().record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};

    auto rejected = core.dispatch(request);

    REQUIRE(!rejected.ok());
    REQUIRE_EQ(
        rejected.error().code,
        static_cast<std::uint32_t>(ServiceCoreError::profile_not_found));
    REQUIRE(fixture.operations.records.empty());
    REQUIRE_EQ(fixture.profiles.forget_reserve_calls, 0U);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
}

TEST_CASE(normal_stop_recovers_after_service_restart_before_worker_execution) {
    Fixture fixture(product::PersistenceState::dirty);
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(stop_request(
            fixture, "fffffff1-ffff-4fff-8fff-fffffffffff1"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        REQUIRE(fixture.worker.calls.empty());
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::quiesce_mutations,
            ActionKind::flush_exact_volume,
            ActionKind::checkpoint,
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
}

TEST_CASE(force_stop_recovers_durable_ack_after_service_restart) {
    Fixture fixture(product::PersistenceState::dirty);
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(force_stop_request(
            fixture, "fffffff2-ffff-4fff-8fff-fffffffffff2",
            true, true, true));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        REQUIRE(fixture.profiles.snapshot().stop
                    .volatile_data_loss_acknowledged);
        REQUIRE(fixture.worker.calls.empty());
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
    REQUIRE_EQ(completed.value().terminal_result.size(), 44U);
}

TEST_CASE(normal_stop_restart_resumes_only_after_last_durable_teardown_phase) {
    Fixture fixture(product::PersistenceState::clean);
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(stop_request(
            fixture, "fffffff3-ffff-4fff-8fff-fffffffffff3"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
    }
    auto snapshot = fixture.profiles.snapshot();
    snapshot.stop.phase = StopDurablePhase::mount_removed;
    snapshot.operation_gate.cancellable = false;
    fixture.profiles.replace(std::move(snapshot));
    auto& operation = fixture.operations.records.front();
    operation.cancellable = false;
    operation.phase = "stop-reconciling";

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
}

TEST_CASE(normal_stop_restart_terminalizes_profile_first_operation_split_without_replaying_teardown) {
    Fixture fixture(product::PersistenceState::clean);
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(stop_request(
            fixture, "fffffff4-ffff-4fff-8fff-fffffffffff4"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 3U;
        auto interrupted = first.execute_operation(operation_id.value());
        REQUIRE(!interrupted.ok());
    }
    const auto completed_actions = fixture.worker.calls;
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
    fixture.operations.failed_write_call.reset();

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls, completed_actions);
    REQUIRE(!fixture.profiles.snapshot().operation_gate
                 .active_operation_id.has_value());
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::stable);
}

TEST_CASE(normal_stop_restart_resumes_from_quiesced_phase_without_replaying_quiesce) {
    Fixture fixture(product::PersistenceState::dirty);
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(stop_request(
            fixture, "fffffff5-ffff-4fff-8fff-fffffffffff5"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
    }
    auto snapshot = fixture.profiles.snapshot();
    snapshot.stop.phase = StopDurablePhase::quiesced;
    fixture.profiles.replace(std::move(snapshot));

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({
            ActionKind::flush_exact_volume,
            ActionKind::checkpoint,
            ActionKind::remove_mount,
            ActionKind::remove_exact_device,
            ActionKind::stop_runtime_and_release_lock}));
}

TEST_CASE(normal_stop_restart_reconciles_checkpoint_boundary_before_teardown) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(stop_request(
            fixture, "fffffff6-ffff-4fff-8fff-fffffffffff6"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        auto interrupted = first.execute_operation(operation_id.value());
        REQUIRE(interrupted.ok());
        REQUIRE_EQ(
            interrupted.value().state,
            OperationState::reconciling);
    }
    REQUIRE_EQ(
        fixture.profiles.snapshot().stop.phase,
        StopDurablePhase::resolving);
    const auto calls_before_restart = fixture.worker.calls;
    fixture.worker.fail_after_boundary = false;
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    auto expected_calls = calls_before_restart;
    expected_calls.push_back(ActionKind::remove_mount);
    expected_calls.push_back(ActionKind::remove_exact_device);
    expected_calls.push_back(ActionKind::stop_runtime_and_release_lock);
    REQUIRE_EQ(fixture.worker.calls, expected_calls);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.runtime,
        product::RuntimeState::stopped);
}

TEST_CASE(rebind_recovers_exact_durable_intent_after_service_restart) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot().record;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(rebind_request(
            fixture, "fffffff7-ffff-4fff-8fff-fffffffffff7",
            before.profile.image_uuid));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        REQUIRE_EQ(fixture.profiles.recovery_reserve_calls, 1U);
        REQUIRE(fixture.profiles.recovery_reservation.has_value());
        REQUIRE_EQ(
            fixture.profiles.recovery_reservation->operation_id,
            operation_id.value());
        REQUIRE_EQ(
            fixture.profiles.recovery_reservation->intent.bundle_path,
            L"E:\\Moved\\work.myrd");
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(
        fixture.worker.calls,
        std::vector<ActionKind>({ActionKind::verify_rebind_bundle}));
    REQUIRE_EQ(
        fixture.profiles.snapshot().record.profile.bundle_path,
        L"E:\\Moved\\work.myrd");
}

TEST_CASE(repair_profile_recovers_previous_candidate_after_service_restart) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    auto previous = fixture.profiles.snapshot();
    previous.record.revision = 6;
    previous.state.disk.health = product::HealthState::needs_attention;
    fixture.profiles.repair_candidate = previous;
    fixture.profiles.fail_current_load = true;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(repair_request(
            fixture, "fffffff8-ffff-4fff-8fff-fffffffffff8"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        REQUIRE_EQ(fixture.profiles.recovery_reserve_calls, 1U);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.repair_calls, 1U);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 7U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().record.profile,
        previous.record.profile);
}

TEST_CASE(rebind_restart_terminalizes_profile_first_split_without_reinspection) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot().record;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(rebind_request(
            fixture, "fffffff9-ffff-4fff-8fff-fffffffffff9",
            before.profile.image_uuid));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 3U;
        auto interrupted = first.execute_operation(operation_id.value());
        REQUIRE(!interrupted.ok());
    }
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 8U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().record.profile.bundle_path,
        L"E:\\Moved\\work.myrd");
    fixture.operations.failed_write_call.reset();

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    auto terminal = fixture.operations.load(operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 8U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate
                 .active_operation_id.has_value());
}

TEST_CASE(repair_restart_terminalizes_repaired_profile_without_reapplying_previous) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    auto previous = fixture.profiles.snapshot();
    previous.record.revision = 6;
    previous.state.disk.health = product::HealthState::needs_attention;
    fixture.profiles.repair_candidate = previous;
    fixture.profiles.fail_current_load = true;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(repair_request(
            fixture, "ffffff10-ffff-4fff-8fff-ffffffffff10"));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 3U;
        auto interrupted = first.execute_operation(operation_id.value());
        REQUIRE(!interrupted.ok());
    }
    REQUIRE_EQ(fixture.profiles.repair_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 7U);
    REQUIRE(!fixture.profiles.repair_candidate.has_value());
    fixture.operations.failed_write_call.reset();

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    REQUIRE_EQ(fixture.profiles.repair_calls, 1U);
    auto terminal = fixture.operations.load(operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, 7U);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(forget_recovers_reserved_tombstone_after_service_restart) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(forget_request(
            fixture, "ffffff11-ffff-4fff-8fff-ffffffffff11",
            profile_id));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        REQUIRE(fixture.profiles.forget_tombstone.has_value());
        REQUIRE(!fixture.profiles.forgotten);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    REQUIRE(fixture.profiles.forgotten);
}

TEST_CASE(forget_restart_terminalizes_metadata_removed_split_without_redeletion) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(forget_request(
            fixture, "ffffff12-ffff-4fff-8fff-ffffffffff12",
            profile_id));
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 3U;
        auto interrupted = first.execute_operation(operation_id.value());
        REQUIRE(!interrupted.ok());
    }
    REQUIRE(fixture.profiles.forgotten);
    REQUIRE(fixture.profiles.forget_tombstone.has_value());
    REQUIRE_EQ(
        fixture.profiles.forget_tombstone->phase,
        ForgetTombstonePhase::metadata_removed);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    fixture.operations.failed_write_call.reset();

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    auto terminal = fixture.operations.load(operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(result_u32(terminal.value().terminal_result, 8), 1U);
    REQUIRE(std::equal(
        profile_id.bytes().begin(), profile_id.bytes().end(),
        terminal.value().terminal_result.begin() + 12));
}

TEST_CASE(terminal_forget_with_absent_profile_is_stable_across_service_restart) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(forget_request(
            fixture, "ffffff13-ffff-4fff-8fff-ffffffffff13",
            profile_id));
        REQUIRE(queued.ok());
        auto completed = first.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    }
    const auto proof_calls = fixture.worker.forget_proof_calls;
    const auto remove_calls = fixture.profiles.forget_remove_calls;

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    REQUIRE_EQ(fixture.worker.forget_proof_calls, proof_calls);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, remove_calls);
    REQUIRE(fixture.profiles.forgotten);
}

TEST_CASE(forget_same_request_replay_returns_terminal_operation_after_profile_removal) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    const auto request = forget_request(
        fixture, "ffffff14-ffff-4fff-8fff-ffffffffff14", profile_id);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    auto completed = core.execute_operation(operation_id);
    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    const auto proof_calls = fixture.worker.forget_proof_calls;
    const auto remove_calls = fixture.profiles.forget_remove_calls;

    auto replay = core.dispatch(request);

    REQUIRE(replay.ok());
    REQUIRE_EQ(
        replay.value().disposition,
        ServiceResponseDisposition::completed);
    REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
    REQUIRE_EQ(
        replay.value().operation->state,
        OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, proof_calls);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, remove_calls);
}

TEST_CASE(forget_same_request_replay_requires_its_exact_retained_tombstone) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    const auto request = forget_request(
        fixture, "ac00001b-0000-4000-8000-00000000001b", profile_id);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    REQUIRE(core.execute_operation(
        queued.value().operation_id.value()).ok());

    const auto exact = fixture.profiles.forget_tombstone;
    fixture.profiles.forget_tombstone.reset();
    REQUIRE(!core.dispatch(request).ok());

    fixture.profiles.forget_tombstone = exact;
    fixture.profiles.forget_tombstone->request_id =
        uuid("ac00001f-0000-4000-8000-00000000001f");
    REQUIRE(!core.dispatch(request).ok());
}

TEST_CASE(normal_and_force_stop_same_request_replay_never_repeats_worker_actions) {
    const auto run = [](bool force, std::string_view request_id) {
        Fixture fixture(product::PersistenceState::dirty);
        const auto request = force
            ? force_stop_request(fixture, request_id, true, true, true)
            : stop_request(fixture, request_id);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(request);
        REQUIRE(queued.ok());
        const auto operation_id = queued.value().operation_id.value();
        auto completed = core.execute_operation(operation_id);
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        const auto worker_calls = fixture.worker.calls;

        auto replay = core.dispatch(request);

        REQUIRE(replay.ok());
        REQUIRE_EQ(
            replay.value().disposition,
            ServiceResponseDisposition::completed);
        REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
        REQUIRE_EQ(replay.value().operation->state, OperationState::succeeded);
        REQUIRE_EQ(fixture.worker.calls, worker_calls);
        REQUIRE_EQ(fixture.operations.records.size(), 1U);
    };

    run(false, "ac000001-0000-4000-8000-000000000001");
    run(true, "ac000002-0000-4000-8000-000000000002");
}

TEST_CASE(rebind_same_request_replay_never_reinspects_or_republishes) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto request = rebind_request(
        fixture, "ac000003-0000-4000-8000-000000000003",
        fixture.profiles.snapshot().record.profile.image_uuid);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());
    const auto worker_calls = fixture.worker.calls;
    const auto rebind_calls = fixture.profiles.rebind_calls;

    auto replay = core.dispatch(request);

    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().disposition, ServiceResponseDisposition::completed);
    REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
    REQUIRE_EQ(fixture.worker.calls, worker_calls);
    REQUIRE_EQ(fixture.profiles.rebind_calls, rebind_calls);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
}

TEST_CASE(repair_same_request_replay_never_reapplies_previous_profile) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    auto previous = fixture.profiles.snapshot();
    previous.record.revision = 6;
    previous.state.disk.health = product::HealthState::needs_attention;
    fixture.profiles.repair_candidate = previous;
    fixture.profiles.fail_current_load = true;
    const auto request = repair_request(
        fixture, "ac000004-0000-4000-8000-000000000004");
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());
    const auto repair_calls = fixture.profiles.repair_calls;
    const auto revision = fixture.profiles.snapshot().record.revision;

    auto replay = core.dispatch(request);

    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().disposition, ServiceResponseDisposition::completed);
    REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
    REQUIRE_EQ(fixture.profiles.repair_calls, repair_calls);
    REQUIRE_EQ(fixture.profiles.snapshot().record.revision, revision);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
}

TEST_CASE(admission_side_write_failure_is_terminal_and_same_request_is_stable) {
    {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.profiles.fail_before_stop_phase_once =
            StopDurablePhase::admitted;
        const auto request = stop_request(
            fixture, "ac00000d-0000-4000-8000-00000000000d");
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(!core.dispatch(request).ok());
        require_operation_only_admission_terminalized(fixture);
        const auto operation_id = fixture.operations.records.front().operation_id;
        auto replay = core.dispatch(request);
        REQUIRE(replay.ok());
        REQUIRE_EQ(replay.value().disposition,
                   ServiceResponseDisposition::completed);
        REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
        REQUIRE_EQ(replay.value().operation->state, OperationState::failed);
        REQUIRE(fixture.worker.calls.empty());
    }
    {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.profiles.fail_before_stop_phase_once =
            StopDurablePhase::checkpoint_confirmed;
        const auto request = force_stop_request(
            fixture, "ac00000e-0000-4000-8000-00000000000e",
            true, true, true);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(!core.dispatch(request).ok());
        require_operation_only_admission_terminalized(fixture);
        REQUIRE(core.dispatch(request).ok());
        REQUIRE(fixture.worker.calls.empty());
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_recovery_reserve_once = true;
        const auto request = rebind_request(
            fixture, "ac00000f-0000-4000-8000-00000000000f",
            fixture.profiles.snapshot().record.profile.image_uuid);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(!core.dispatch(request).ok());
        require_operation_only_admission_terminalized(fixture);
        REQUIRE(core.dispatch(request).ok());
        REQUIRE(fixture.worker.calls.empty());
        REQUIRE_EQ(fixture.profiles.rebind_calls, 0U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        auto previous = fixture.profiles.snapshot();
        previous.record.revision = 6;
        previous.state.disk.health = product::HealthState::needs_attention;
        fixture.profiles.repair_candidate = previous;
        fixture.profiles.fail_recovery_reserve_once = true;
        const auto request = repair_request(
            fixture, "ac000010-0000-4000-8000-000000000010");
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(!core.dispatch(request).ok());
        require_operation_only_admission_terminalized(fixture);
        REQUIRE(core.dispatch(request).ok());
        REQUIRE_EQ(fixture.profiles.repair_calls, 0U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_forget_reserve_once = true;
        const auto profile_id =
            fixture.profiles.snapshot().record.profile.profile_uuid;
        const auto request = forget_request(
            fixture, "ac000011-0000-4000-8000-000000000011", profile_id);
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(!core.dispatch(request).ok());
        require_operation_only_admission_terminalized(fixture);
        REQUIRE(core.dispatch(request).ok());
        REQUIRE_EQ(fixture.profiles.forget_remove_calls, 0U);
        REQUIRE(!fixture.profiles.forgotten);
    }
}

TEST_CASE(admission_side_write_indeterminate_exact_commit_continues_once) {
    {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.profiles.fail_after_stop_phase_once =
            StopDurablePhase::admitted;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(stop_request(
            fixture, "ac000014-0000-4000-8000-000000000014"));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        REQUIRE_EQ(
            static_cast<std::size_t>(std::count(
                fixture.worker.calls.begin(), fixture.worker.calls.end(),
                ActionKind::remove_exact_device)),
            1U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_recovery_reserve_after_commit_once = true;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(rebind_request(
            fixture, "ac000015-0000-4000-8000-000000000015",
            fixture.profiles.snapshot().record.profile.image_uuid));
        REQUIRE(queued.ok());
        REQUIRE(core.execute_operation(
            queued.value().operation_id.value()).ok());
        REQUIRE_EQ(fixture.profiles.rebind_calls, 1U);
        REQUIRE_EQ(fixture.worker.calls,
                   std::vector<ActionKind>({ActionKind::verify_rebind_bundle}));
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_forget_reserve_after_commit_once = true;
        const auto profile_id =
            fixture.profiles.snapshot().record.profile.profile_uuid;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(forget_request(
            fixture, "ac000016-0000-4000-8000-000000000016", profile_id));
        REQUIRE(queued.ok());
        REQUIRE(core.execute_operation(
            queued.value().operation_id.value()).ok());
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
        REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    }
}

TEST_CASE(operation_admitting_write_is_resolved_before_worker_execution) {
    {
        Fixture fixture(product::PersistenceState::dirty);
        fixture.operations.failed_write_call = 1U;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(stop_request(
            fixture, "ac000017-0000-4000-8000-000000000017"));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        REQUIRE_EQ(fixture.operations.records.size(), 1U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.operations.failed_after_write_call = 1U;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(rebind_request(
            fixture, "ac000018-0000-4000-8000-000000000018",
            fixture.profiles.snapshot().record.profile.image_uuid));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        REQUIRE_EQ(fixture.profiles.rebind_calls, 1U);
    }
}

TEST_CASE(recovery_gate_publication_is_resolved_before_execution) {
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_runtime_publish_once_before = true;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(rebind_request(
            fixture, "ac000019-0000-4000-8000-000000000019",
            fixture.profiles.snapshot().record.profile.image_uuid));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        REQUIRE_EQ(fixture.profiles.rebind_calls, 1U);
    }
    {
        Fixture fixture(product::PersistenceState::clean);
        set_stopped(fixture);
        fixture.profiles.fail_runtime_publish_once_after = true;
        const auto profile_id =
            fixture.profiles.snapshot().record.profile.profile_uuid;
        ServiceCore core(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = core.dispatch(forget_request(
            fixture, "ac00001a-0000-4000-8000-00000000001a", profile_id));
        REQUIRE(queued.ok());
        auto completed = core.execute_operation(
            queued.value().operation_id.value());
        REQUIRE(completed.ok());
        REQUIRE_EQ(completed.value().state, OperationState::succeeded);
        REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
        REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
    }
}

TEST_CASE(normal_stop_operation_first_crash_terminalizes_without_worker_io) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto before = fixture.profiles.snapshot();
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(stop_request(
            fixture, "ac000005-0000-4000-8000-000000000005")).ok());
    }
    fixture.profiles.replace(before);
    rewind_to_operation_only_admission(fixture);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(recovered.ok());
    require_operation_only_admission_terminalized(fixture);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.snapshot(), before);
}

TEST_CASE(force_stop_operation_first_crash_terminalizes_without_teardown) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto before = fixture.profiles.snapshot();
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(force_stop_request(
            fixture, "ac000006-0000-4000-8000-000000000006",
            true, true, true)).ok());
    }
    fixture.profiles.replace(before);
    rewind_to_operation_only_admission(fixture);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());

    require_operation_only_admission_terminalized(fixture);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.snapshot(), before);
}

TEST_CASE(rebind_operation_first_crash_terminalizes_without_inspection_or_adoption) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot();
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(rebind_request(
            fixture, "ac000007-0000-4000-8000-000000000007",
            before.record.profile.image_uuid)).ok());
    }
    fixture.profiles.replace(before);
    fixture.profiles.recovery_reservation.reset();
    rewind_to_operation_only_admission(fixture);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());

    require_operation_only_admission_terminalized(fixture);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.rebind_calls, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot(), before);
}

TEST_CASE(repair_operation_first_crash_terminalizes_without_repair) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    auto previous = fixture.profiles.snapshot();
    previous.record.revision = 6;
    previous.state.disk.health = product::HealthState::needs_attention;
    fixture.profiles.repair_candidate = previous;
    fixture.profiles.fail_current_load = true;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(repair_request(
            fixture, "ac000008-0000-4000-8000-000000000008")).ok());
    }
    fixture.profiles.recovery_reservation.reset();
    rewind_to_operation_only_admission(fixture);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());

    require_operation_only_admission_terminalized(fixture);
    REQUIRE_EQ(fixture.profiles.repair_calls, 0U);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(forget_operation_first_crash_terminalizes_without_metadata_removal) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot();
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        REQUIRE(first.dispatch(forget_request(
            fixture, "ac000009-0000-4000-8000-000000000009",
            before.record.profile.profile_uuid)).ok());
    }
    fixture.profiles.replace(before);
    fixture.profiles.forget_tombstone.reset();
    fixture.profiles.forgotten = false;
    rewind_to_operation_only_admission(fixture);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());

    require_operation_only_admission_terminalized(fixture);
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 0U);
    REQUIRE(!fixture.profiles.forgotten);
    REQUIRE_EQ(fixture.profiles.snapshot(), before);
}

TEST_CASE(operation_show_returns_retained_forget_result_after_profile_removal) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    const auto forget = forget_request(
        fixture, "ffffff15-ffff-4fff-8fff-ffffffffff15", profile_id);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(forget);
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());
    const ServiceCommandRequest show{
        uuid("ffffff16-ffff-4fff-8fff-ffffffffff16"),
        control::OperationShowCommand{operation_id},
        forget.caller,
        false,
        false,
        std::nullopt,
        ClientWaitState::active};

    auto response = core.dispatch(show);

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        ServiceResponseDisposition::completed);
    REQUIRE(response.value().operation.has_value());
    REQUIRE_EQ(
        response.value().operation->state,
        OperationState::succeeded);
    REQUIRE_EQ(
        result_u32(response.value().operation->terminal_result, 8),
        1U);
}

TEST_CASE(forget_tombstone_preserves_original_owner_authorization_after_admin_removal) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto owner_sid =
        fixture.profiles.snapshot().record.profile.owner_sid;
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    auto forget = forget_request(
        fixture, "ac00000a-0000-4000-8000-00000000000a", profile_id);
    forget.caller = elevated_admin(L"S-1-5-21-900-901-902-1002");
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(forget);
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());

    const ServiceCommandRequest owner_show{
        uuid("ac00000b-0000-4000-8000-00000000000b"),
        control::OperationShowCommand{operation_id},
        caller(owner_sid),
        false,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto owner_response = core.dispatch(owner_show);
    REQUIRE(owner_response.ok());
    REQUIRE_EQ(
        owner_response.value().disposition,
        ServiceResponseDisposition::completed);

    auto requester_show = owner_show;
    requester_show.request_id =
        uuid("ac00000c-0000-4000-8000-00000000000c");
    requester_show.caller = caller(forget.caller.primary_sid);
    REQUIRE(!core.dispatch(requester_show).ok());
}

TEST_CASE(forget_absence_authority_rejects_profile_store_io_failure) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(forget_request(
        fixture, "ac000012-0000-4000-8000-000000000012", profile_id));
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());
    fixture.profiles.fail_forgotten_load = true;

    const ServiceCommandRequest show{
        uuid("ac000013-0000-4000-8000-000000000013"),
        control::OperationShowCommand{operation_id},
        caller(fixture.profiles.snapshot().record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        ClientWaitState::active};
    REQUIRE(!core.dispatch(show).ok());

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(!restarted.recover_after_restart().ok());
}

TEST_CASE(forget_tombstone_io_failure_remains_nonterminal_and_recoverable) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    ServiceCore first(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = first.dispatch(forget_request(
        fixture, "ac00001c-0000-4000-8000-00000000001c", profile_id));
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    fixture.profiles.fail_forget_lookup_once = true;

    auto interrupted = first.execute_operation(operation_id);

    REQUIRE(!interrupted.ok());
    auto durable = fixture.operations.load(operation_id);
    REQUIRE(durable.ok());
    REQUIRE(durable.value().state != OperationState::succeeded);
    REQUIRE(durable.value().state != OperationState::failed);
    REQUIRE(durable.value().state != OperationState::cancelled);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
        == std::optional<product::Uuid>(operation_id));
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 0U);
    REQUIRE(!fixture.profiles.forgotten);

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);
    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.forget_remove_calls, 1U);
}

TEST_CASE(operation_show_propagates_forget_tombstone_store_failure) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto owner_sid =
        fixture.profiles.snapshot().record.profile.owner_sid;
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(forget_request(
        fixture, "ac00001d-0000-4000-8000-00000000001d", profile_id));
    REQUIRE(queued.ok());
    const auto operation_id = queued.value().operation_id.value();
    REQUIRE(core.execute_operation(operation_id).ok());
    fixture.profiles.fail_forget_lookup_once = true;
    const ServiceCommandRequest show{
        uuid("ac00001e-0000-4000-8000-00000000001e"),
        control::OperationShowCommand{operation_id},
        caller(owner_sid),
        false,
        false,
        std::nullopt,
        ClientWaitState::active};

    auto response = core.dispatch(show);

    REQUIRE(!response.ok());
    REQUIRE_EQ(
        response.error().code,
        static_cast<std::uint32_t>(ServiceCoreError::dependency_failure));
}

TEST_CASE(restart_rejects_recovery_reservation_that_no_longer_matches_operation_payload) {
    Fixture fixture(product::PersistenceState::clean);
    set_stopped(fixture);
    const auto before = fixture.profiles.snapshot().record;
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(rebind_request(
            fixture, "ffffff17-ffff-4fff-8fff-ffffffffff17",
            before.profile.image_uuid));
        REQUIRE(queued.ok());
    }
    REQUIRE(fixture.profiles.recovery_reservation.has_value());
    fixture.profiles.recovery_reservation->intent.bundle_path =
        L"E:\\Tampered\\other.myrd";

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();

    REQUIRE(!recovered.ok());
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.profiles.rebind_calls, 0U);
}
