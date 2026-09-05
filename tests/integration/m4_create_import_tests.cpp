#include "tests/support/test_framework.h"

#include "src/management/service_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

Error test_error(const wchar_t* operation) {
    return Error{
        ErrorDomain::internal,
        static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
        operation,
        L"Injected create/import integration failure"};
}

ipc::CallerIdentity administrator() {
    return ipc::CallerIdentity{
        L"S-1-5-21-100-200-300-1001",
        true, false, false, true, true, false, false};
}

class FakeClock final : public IServiceClock {
public:
    std::uint64_t unix_time_ms() const noexcept override { return unix_ms; }
    std::uint64_t monotonic_time_ms() const noexcept override {
        return monotonic_ms;
    }
    std::uint64_t unix_ms{1'700'000'000'000ULL};
    std::uint64_t monotonic_ms{10'000};
};

class FakeProfiles final : public IServiceProfileStore {
public:
    Result<ManagedProfileSnapshot> resolve(
        std::wstring_view target) override {
        for (const auto& item : values) {
            if (item.record.profile.display_name == target) {
                return Result<ManagedProfileSnapshot>::success(item);
            }
        }
        return Result<ManagedProfileSnapshot>::failure(
            test_error(L"fake_profiles.resolve"));
    }

    Result<ManagedProfileSnapshot> load(
        const product::Uuid& profile_id) override {
        for (const auto& item : values) {
            if (item.record.profile.profile_uuid == profile_id) {
                return Result<ManagedProfileSnapshot>::success(item);
            }
        }
        return Result<ManagedProfileSnapshot>::failure(
            test_error(L"fake_profiles.load"));
    }

    Result<ManagedProfileSnapshot> publish_runtime(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate) override {
        for (auto& item : values) {
            if (item.record.profile.profile_uuid
                != expected.record.profile.profile_uuid) {
                continue;
            }
            REQUIRE_EQ(item, expected);
            item.state = proposed_state;
            item.operation_gate = proposed_gate;
            return Result<ManagedProfileSnapshot>::success(item);
        }
        return Result<ManagedProfileSnapshot>::failure(
            test_error(L"fake_profiles.publish_runtime"));
    }

    void add(ManagedProfileSnapshot snapshot) {
        values.push_back(std::move(snapshot));
    }

    Result<ManagedProfileSnapshot> replace_record(
        const ManagedProfileSnapshot& expected,
        profile::ProfileRecord proposed,
        const ProfileState& state,
        const OperationGateState& gate) {
        for (auto& item : values) {
            if (item.record.profile.profile_uuid
                != expected.record.profile.profile_uuid) {
                continue;
            }
            REQUIRE_EQ(item, expected);
            item.record = std::move(proposed);
            item.state = state;
            item.operation_gate = gate;
            return Result<ManagedProfileSnapshot>::success(item);
        }
        return Result<ManagedProfileSnapshot>::failure(
            test_error(L"fake_profiles.replace_record"));
    }

    std::vector<ManagedProfileSnapshot> values;
};

class FakeRegistrations final : public IServiceRegistrationStore {
public:
    explicit FakeRegistrations(FakeProfiles& profiles) : profiles_(&profiles) {}

    Result<LifecycleRegistrationReservation> reserve(
        const LifecycleRegistrationReservation& proposed) override {
        if (reserve_error.has_value()) {
            return Result<LifecycleRegistrationReservation>::failure(
                reserve_error.value());
        }
        const auto existing = std::find_if(
            reservations.begin(), reservations.end(),
            [&](const auto& candidate) {
                return candidate.operation_id == proposed.operation_id;
            });
        if (existing != reservations.end()) {
            REQUIRE_EQ(existing->request_id, proposed.request_id);
            REQUIRE_EQ(existing->argument_digest, proposed.argument_digest);
            REQUIRE_EQ(existing->intent.profile_id,
                       proposed.intent.profile_id);
            REQUIRE_EQ(existing->intent.kind, proposed.intent.kind);
            return Result<LifecycleRegistrationReservation>::success(
                *existing);
        }
        reservations.push_back(proposed);
        return Result<LifecycleRegistrationReservation>::success(proposed);
    }

    Result<LifecycleRegistrationReservation> load_reservation(
        const product::Uuid& operation_id) override {
        const auto existing = std::find_if(
            reservations.begin(), reservations.end(),
            [&](const auto& candidate) {
                return candidate.operation_id == operation_id;
            });
        if (existing == reservations.end()) {
            return Result<LifecycleRegistrationReservation>::failure(
                test_error(L"fake_registrations.load_reservation"));
        }
        return Result<LifecycleRegistrationReservation>::success(*existing);
    }

    Result<LifecycleRegistrationReservation> update_reservation(
        const LifecycleRegistrationReservation& expected,
        const LifecycleRegistrationReservation& proposed) override {
        const auto existing = std::find_if(
            reservations.begin(), reservations.end(),
            [&](const auto& candidate) {
                return candidate.operation_id == expected.operation_id;
            });
        if (existing == reservations.end()) {
            return Result<LifecycleRegistrationReservation>::failure(
                test_error(L"fake_registrations.update_reservation"));
        }
        REQUIRE_EQ(existing->revision, expected.revision);
        REQUIRE_EQ(proposed.revision, expected.revision + 1);
        REQUIRE_EQ(proposed.operation_id, expected.operation_id);
        REQUIRE_EQ(proposed.request_id, expected.request_id);
        REQUIRE_EQ(proposed.argument_digest, expected.argument_digest);
        REQUIRE_EQ(proposed.intent.profile_id, expected.intent.profile_id);
        *existing = proposed;
        ++reservation_update_calls;
        return Result<LifecycleRegistrationReservation>::success(proposed);
    }

    Result<RegistrationMutationResult> publish_registration(
        const LifecycleOperationIntent& intent,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& bundle,
        const ProfileState& state,
        const OperationGateState& gate) override {
        ++publish_calls;
        REQUIRE_EQ(intent.profile_id, proposed.profile.profile_uuid);
        REQUIRE_EQ(bundle.image_id, proposed.profile.image_uuid);
        REQUIRE_EQ(bundle.logical_capacity_bytes,
                   proposed.profile.logical_capacity_bytes);
        for (const auto& existing : profiles_->values) {
            if (existing.record.profile.display_name
                    == proposed.profile.display_name
                || existing.record.profile.image_uuid == bundle.image_id) {
                return Result<RegistrationMutationResult>::success({
                    RegistrationCommitDisposition::not_committed,
                    std::nullopt});
            }
        }
        if ((duplicate_image_id.has_value()
             && duplicate_image_id.value() == bundle.image_id)
            || (duplicate_file_identity.has_value()
                && duplicate_file_identity.value() == bundle.file_identity)) {
            return Result<RegistrationMutationResult>::success({
                RegistrationCommitDisposition::not_committed,
                std::nullopt});
        }
        if (publish_disposition
                == RegistrationCommitDisposition::not_committed) {
            return Result<RegistrationMutationResult>::success({
                publish_disposition, std::nullopt});
        }
        ManagedProfileSnapshot snapshot{proposed, state, gate};
        profiles_->add(snapshot);
        publications.push_back(snapshot);
        return Result<RegistrationMutationResult>::success({
            publish_disposition,
            publish_disposition == RegistrationCommitDisposition::committed
                ? std::optional<ManagedProfileSnapshot>(snapshot)
                : std::nullopt});
    }

    Result<RegistrationMutationResult> update_registration(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const ProfileState& state,
        const OperationGateState& gate) override {
        ++update_calls;
        const auto disposition = affected_update_call.has_value()
                && affected_update_call.value() == update_calls
            ? affected_update_disposition
            : RegistrationCommitDisposition::committed;
        if (disposition == RegistrationCommitDisposition::not_committed) {
            return Result<RegistrationMutationResult>::success({
                disposition, std::nullopt});
        }
        updates.push_back(proposed);
        auto replaced = profiles_->replace_record(
            expected, proposed, state, gate);
        if (!replaced.ok()) {
            return Result<RegistrationMutationResult>::failure(
                replaced.error());
        }
        return Result<RegistrationMutationResult>::success({
            disposition,
            disposition == RegistrationCommitDisposition::committed
                ? std::optional<ManagedProfileSnapshot>(replaced.value())
                : std::nullopt});
    }

    Result<RegistrationResolution> resolve_registration(
        const product::Uuid& operation_id,
        const product::Uuid& profile_id) override {
        ++resolve_calls;
        if (resolution_ambiguous) {
            return Result<RegistrationResolution>::success({
                RegistrationResolutionDisposition::ambiguous,
                std::nullopt});
        }
        const auto reservation = std::find_if(
            reservations.begin(), reservations.end(),
            [&](const auto& candidate) {
                return candidate.operation_id == operation_id
                    && candidate.intent.profile_id == profile_id;
            });
        if (reservation == reservations.end()) {
            return Result<RegistrationResolution>::success({
                RegistrationResolutionDisposition::ambiguous,
                std::nullopt});
        }
        const auto profile = std::find_if(
            profiles_->values.begin(), profiles_->values.end(),
            [&](const auto& candidate) {
                return candidate.record.profile.profile_uuid == profile_id;
            });
        if (profile == profiles_->values.end()) {
            return Result<RegistrationResolution>::success({
                RegistrationResolutionDisposition::absent,
                std::nullopt});
        }
        return Result<RegistrationResolution>::success({
            RegistrationResolutionDisposition::exact,
            *profile});
    }

    FakeProfiles* profiles_;
    std::vector<LifecycleRegistrationReservation> reservations;
    std::vector<ManagedProfileSnapshot> publications;
    std::vector<profile::ProfileRecord> updates;
    std::optional<product::Uuid> duplicate_image_id;
    std::optional<BundleFileIdentity> duplicate_file_identity;
    std::optional<Error> reserve_error;
    RegistrationCommitDisposition publish_disposition{
        RegistrationCommitDisposition::committed};
    std::optional<std::size_t> affected_update_call;
    RegistrationCommitDisposition affected_update_disposition{
        RegistrationCommitDisposition::committed};
    bool resolution_ambiguous{};
    std::size_t publish_calls{};
    std::size_t update_calls{};
    std::size_t resolve_calls{};
    std::size_t reservation_update_calls{};
};

class FakeOperations final : public IServiceOperationStore {
public:
    Result<std::optional<OperationAdmission>> find_request(
        const OperationRequest& request) override {
        if (!record.has_value()) {
            return Result<std::optional<OperationAdmission>>::success(
                std::nullopt);
        }
        if (record->request_id == request.request_id
            && record->profile_id == request.profile_id
            && record->caller_sid == request.caller_sid
            && record->command_scope == request.command_scope
            && record->argument_digest == request.argument_digest) {
            return Result<std::optional<OperationAdmission>>::success(
                OperationAdmission{record.value(), true});
        }
        return Result<std::optional<OperationAdmission>>::failure(
            test_error(L"fake_operations.find"));
    }

    Result<OperationAdmission> admit(
        const OperationRequest& request,
        const product::Uuid& candidate_operation_id) override {
        REQUIRE(!record.has_value());
        record = OperationRecord{
            1, request.request_id, candidate_operation_id,
            request.profile_id, request.caller_sid, request.command_scope,
            request.argument_digest, OperationState::queued, "queued",
            request.created_unix_ms, request.created_unix_ms,
            std::nullopt, true, false, {}};
        return Result<OperationAdmission>::success(
            OperationAdmission{record.value(), false});
    }

    Result<OperationRecord> load(
        const product::Uuid& operation_id) override {
        if (record.has_value() && record->operation_id == operation_id) {
            return Result<OperationRecord>::success(record.value());
        }
        return Result<OperationRecord>::failure(
            test_error(L"fake_operations.load"));
    }

    Result<void> write(
        std::uint64_t expected_revision,
        const OperationRecord& proposed) override {
        REQUIRE(record.has_value());
        REQUIRE_EQ(record->revision, expected_revision);
        REQUIRE_EQ(proposed.revision, expected_revision + 1);
        record = proposed;
        history.push_back(proposed);
        return Result<void>::success();
    }

    Result<std::vector<OperationRecord>> reconcile_after_restart(
        std::uint64_t now_unix_ms) override {
        if (record.has_value()
            && record->state == OperationState::running) {
            ++record->revision;
            record->state = OperationState::reconciling;
            record->phase = "restart-reconcile";
            record->updated_unix_ms = (std::max)(
                record->updated_unix_ms, now_unix_ms);
            history.push_back(record.value());
        }
        return Result<std::vector<OperationRecord>>::success(
            record.has_value() ? std::vector<OperationRecord>{record.value()}
                               : std::vector<OperationRecord>{});
    }

    Result<OperationRecord> request_cancel(
        const product::Uuid&, std::uint64_t) override {
        return Result<OperationRecord>::failure(
            test_error(L"fake_operations.cancel"));
    }

    Result<OperationWaitObservation> wait_for_change(
        const product::Uuid&, std::uint64_t, std::uint64_t) override {
        return Result<OperationWaitObservation>::success(
            {OperationWaitDisposition::timed_out, std::nullopt});
    }

    std::optional<OperationRecord> record;
    std::vector<OperationRecord> history;
};

class FakeWorker final : public IWorkerControl {
public:
    bool begins_noncancellable(ActionKind action) const noexcept override {
        return action == ActionKind::provision_gpt_ntfs
            || action == ActionKind::checkpoint;
    }

    Result<WorkerActionResult> execute(
        const WorkerActionRequest& request) override {
        calls.push_back(request);
        if (failed_action.has_value() && request.action == *failed_action) {
            return Result<WorkerActionResult>::success(WorkerActionResult{
                CompletionOutcome::needs_attention_failure,
                {std::byte{'f'}}});
        }
        WorkerActionResult result{
            CompletionOutcome::succeeded_clean,
            {std::byte{'o'}, std::byte{'k'}}};
        if (request.action == ActionKind::create_and_verify_bundle) {
            REQUIRE(request.lifecycle_intent.has_value());
            result.lifecycle.bundle = BundleRegistrationEvidence{
                request.lifecycle_intent->expected_image_id.value(),
                "disk.rdm",
                request.lifecycle_intent->requested_capacity_bytes,
                kProductLogicalSectorBytes,
                kProductPhysicalSectorBytes,
                payload_block_bytes,
                create_file_identity,
                BundleEvidenceSource::created_and_verified};
        }
        if (request.action == ActionKind::inspect_bundle_read_only) {
            REQUIRE(request.lifecycle_intent.has_value());
            if (request.lifecycle_intent->kind
                    == LifecycleOperationKind::create) {
                result.lifecycle.bundle = BundleRegistrationEvidence{
                    request.lifecycle_intent->expected_image_id.value(),
                    "disk.rdm",
                    request.lifecycle_intent->requested_capacity_bytes,
                    kProductLogicalSectorBytes,
                    kProductPhysicalSectorBytes,
                    payload_block_bytes,
                    create_file_identity,
                    BundleEvidenceSource::read_only_inspection};
            } else {
                result.lifecycle.bundle = BundleRegistrationEvidence{
                    import_image_id,
                    "disk.rdm",
                    8ULL << 30,
                    kProductLogicalSectorBytes,
                    kProductPhysicalSectorBytes,
                    payload_block_bytes,
                    import_file_identity,
                    BundleEvidenceSource::read_only_inspection};
                result.lifecycle.volume = VolumeRegistrationEvidence{
                    uuid("21000000-0000-4000-8000-000000000001"),
                    uuid("21000000-0000-4000-8000-000000000002"),
                    uuid("21000000-0000-4000-8000-000000000003")};
            }
        }
        if (request.action == ActionKind::record_volume_identity) {
            result.lifecycle.volume = VolumeRegistrationEvidence{
                uuid("20000000-0000-4000-8000-000000000001"),
                uuid("20000000-0000-4000-8000-000000000002"),
                uuid("20000000-0000-4000-8000-000000000003")};
        }
        if (request.action == ActionKind::checkpoint) {
            result.lifecycle.first_checkpoint_acknowledged = true;
        }
        return Result<WorkerActionResult>::success(std::move(result));
    }

    std::optional<ActionKind> failed_action;
    std::uint64_t payload_block_bytes{kProductPayloadBlockBytes};
    BundleFileIdentity create_file_identity{
        0x12345678ULL,
        {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
         std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
         std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}}};
    product::Uuid import_image_id =
        uuid("50000000-0000-4000-8000-000000000001");
    BundleFileIdentity import_file_identity{
        0x87654321ULL,
        {std::byte{16}, std::byte{15}, std::byte{14}, std::byte{13},
         std::byte{12}, std::byte{11}, std::byte{10}, std::byte{9},
         std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
         std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}}};
    std::vector<WorkerActionRequest> calls;
};

class FakeLogger final : public IServiceLogger {
public:
    void write(const ServiceLogEvent& event) noexcept override {
        events.push_back(event);
    }
    std::vector<ServiceLogEvent> events;
};

class FakeIds final : public IServiceIdGenerator {
public:
    Result<product::Uuid> next() override {
        REQUIRE(!values.empty());
        auto result = values.front();
        values.erase(values.begin());
        return Result<product::Uuid>::success(std::move(result));
    }
    std::vector<product::Uuid> values;
};

class FakeMemory final : public IPhysicalMemoryQuery {
public:
    Result<PhysicalMemorySnapshot> query() override {
        ++calls;
        return Result<PhysicalMemorySnapshot>::success({total, available});
    }
    std::uint64_t total{16ULL << 30};
    std::uint64_t available{8ULL << 30};
    std::size_t calls{};
};

struct Fixture final {
    FakeClock clock;
    FakeProfiles profiles;
    FakeRegistrations registrations{profiles};
    FakeOperations operations;
    FakeWorker worker;
    FakeLogger logger;
    FakeIds ids;
    FakeMemory memory;
    ServiceCore core{
        clock, profiles, operations, worker, logger, ids, memory,
        &registrations};
};

control::DiskCreateCommand create_command(bool no_start) {
    return control::DiskCreateCommand{
        L"scratch",
        L"D:\\Images\\scratch.myrd",
        8ULL << 30,
        L"R:",
        std::nullopt,
        no_start,
        control::MemoryMode::automatic,
        std::nullopt,
        std::nullopt,
        std::nullopt};
}

ServiceResponse dispatch_create(Fixture& fixture, bool no_start) {
    fixture.ids.values.push_back(
        uuid("40000000-0000-4000-8000-000000000001"));
    auto response = fixture.core.dispatch(ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000001"),
        create_command(no_start),
        administrator(),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(response.ok());
    REQUIRE(response.value().operation_id.has_value());
    return response.move_value();
}

ServiceResponse dispatch_import(Fixture& fixture) {
    fixture.ids.values.push_back(
        uuid("40000000-0000-4000-8000-000000000002"));
    auto response = fixture.core.dispatch(ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000002"),
        control::DiskImportCommand{
            L"imported",
            L"D:\\Images\\existing.myrd",
            L"S:",
            std::nullopt},
        administrator(),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(response.ok());
    REQUIRE(response.value().operation_id.has_value());
    return response.move_value();
}

ServiceResponse dispatch_start(Fixture& fixture, const char* request_text) {
    fixture.ids.values.push_back(
        uuid("40000000-0000-4000-8000-000000000003"));
    auto response = fixture.core.dispatch(ServiceCommandRequest{
        uuid(request_text),
        control::DiskStartCommand{L"scratch"},
        administrator(),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(response.ok());
    REQUIRE(response.value().operation_id.has_value());
    return response.move_value();
}

} // namespace

TEST_CASE(create_no_start_persists_bundle_and_provisioning_without_device_or_format) {
    Fixture fixture;
    const auto response = dispatch_create(fixture, true);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    REQUIRE_EQ(
        fixture.worker.calls[0].action,
        ActionKind::create_and_verify_bundle);
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
    REQUIRE_EQ(fixture.registrations.publications.size(), 1U);
    const auto& profile = fixture.profiles.values.front();
    REQUIRE_EQ(
        profile.record.profile.lifecycle,
        profile::ProfileLifecycle::provisioning);
    REQUIRE_EQ(
        profile.state.disk.runtime,
        product::RuntimeState::stopped);
    REQUIRE(!profile.record.profile.gpt_disk_guid.has_value());
    REQUIRE(!profile.record.profile.partition_guid.has_value());
    REQUIRE(!profile.record.profile.volume_guid.has_value());
    REQUIRE_EQ(profile.record.profile.logical_sector_size, 512U);
    REQUIRE_EQ(profile.record.profile.physical_sector_size, 4096U);
    REQUIRE(profile.record.profile.memory.low_watermark_bytes
            < profile.record.profile.memory.soft_limit_bytes);
    REQUIRE(profile.record.profile.memory.soft_limit_bytes
            < profile.record.profile.memory.hard_limit_bytes);
}

TEST_CASE(create_marks_ready_and_running_only_after_first_acknowledged_checkpoint) {
    Fixture fixture;
    const auto response = dispatch_create(fixture, false);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    const auto& profile = fixture.profiles.values.front();
    REQUIRE_EQ(profile.record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
    REQUIRE_EQ(profile.state.disk.runtime, product::RuntimeState::running);
    REQUIRE(profile.record.profile.gpt_disk_guid.has_value());
    REQUIRE(profile.record.profile.partition_guid.has_value());
    REQUIRE(profile.record.profile.volume_guid.has_value());
    const auto checkpoint = std::find_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) { return call.action == ActionKind::checkpoint; });
    REQUIRE(checkpoint != fixture.worker.calls.end());
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
    REQUIRE_EQ(fixture.registrations.update_calls, 2U);
    const std::vector<std::string> expected_phases{
        "create-bundle",
        "publish-provisioning",
        "create-runtime",
        "recover-bundle",
        "start-proxy",
        "create-device",
        "locate-physical-drive",
        "prove-uninitialized",
        "provision-gpt-ntfs",
        "record-volume",
        "flush-volume",
        "checkpoint",
        "publish-ready",
        "assign-mount",
        "readiness-io"};
    std::vector<std::string> observed_phases;
    for (const auto& operation : fixture.operations.history) {
        if (std::find(
                expected_phases.begin(), expected_phases.end(),
                operation.phase) != expected_phases.end()) {
            observed_phases.push_back(operation.phase);
        }
    }
    REQUIRE_EQ(observed_phases, expected_phases);
    REQUIRE_EQ(fixture.registrations.reservations.front().phase,
               LifecycleReservationPhase::ready_published);
    REQUIRE(fixture.registrations.reservations.front().bundle.has_value());
    REQUIRE(std::none_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::publish_provisioning_profile
                || call.action == ActionKind::publish_imported_ready_profile
                || call.action == ActionKind::publish_ready_profile;
        }));
}

TEST_CASE(create_checkpoint_failure_preserves_provisioning_and_never_reports_ready) {
    Fixture fixture;
    fixture.worker.failed_action = ActionKind::checkpoint;
    const auto response = dispatch_create(fixture, false);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    const auto& profile = fixture.profiles.values.front();
    REQUIRE_EQ(profile.record.profile.lifecycle,
               profile::ProfileLifecycle::provisioning);
    REQUIRE_EQ(profile.state.disk.runtime, product::RuntimeState::stopped);
    REQUIRE_EQ(profile.state.disk.health,
               product::HealthState::needs_attention);
    REQUIRE(profile.record.profile.gpt_disk_guid.has_value());
    REQUIRE_EQ(fixture.registrations.update_calls, 1U);
}

TEST_CASE(import_is_read_only_and_publishes_one_stopped_ready_profile) {
    Fixture fixture;
    const auto response = dispatch_import(fixture);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    REQUIRE_EQ(
        fixture.worker.calls[0].action,
        ActionKind::inspect_bundle_read_only);
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
    REQUIRE_EQ(fixture.registrations.reservations.front().phase,
               LifecycleReservationPhase::profile_published);
    REQUIRE(fixture.registrations.reservations.front().bundle.has_value());
    REQUIRE(fixture.registrations.reservations.front().volume.has_value());
    const auto inspection_phase = std::find_if(
        fixture.operations.history.begin(), fixture.operations.history.end(),
        [](const auto& operation) {
            return operation.phase == "inspect-bundle";
        });
    const auto publication_phase = std::find_if(
        fixture.operations.history.begin(), fixture.operations.history.end(),
        [](const auto& operation) {
            return operation.phase == "publish-imported-ready";
        });
    REQUIRE(inspection_phase != fixture.operations.history.end());
    REQUIRE(publication_phase != fixture.operations.history.end());
    REQUIRE(inspection_phase < publication_phase);
    REQUIRE(std::none_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::provision_gpt_ntfs
                || call.action == ActionKind::checkpoint
                || call.action == ActionKind::compact;
        }));
    REQUIRE_EQ(fixture.profiles.values.size(), 1U);
    const auto& profile = fixture.profiles.values.front();
    REQUIRE_EQ(profile.record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
    REQUIRE_EQ(profile.state.disk.runtime, product::RuntimeState::stopped);
    REQUIRE_EQ(
        profile.record.profile.image_uuid,
        fixture.worker.import_image_id);
    REQUIRE(profile.record.profile.gpt_disk_guid.has_value());
    REQUIRE(profile.record.profile.partition_guid.has_value());
    REQUIRE(profile.record.profile.volume_guid.has_value());
    REQUIRE_EQ(profile.record.profile.memory.mode,
               profile::MemoryMode::automatic);
}

TEST_CASE(import_rejects_a_duplicate_registered_image_identity) {
    Fixture fixture;
    fixture.registrations.duplicate_image_id = fixture.worker.import_image_id;
    const auto response = dispatch_import(fixture);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE(fixture.profiles.values.empty());
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
}

TEST_CASE(import_rejects_a_duplicate_registered_file_identity) {
    Fixture fixture;
    fixture.registrations.duplicate_file_identity =
        fixture.worker.import_file_identity;
    const auto response = dispatch_import(fixture);
    auto terminal = fixture.core.execute_operation(
        response.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE(fixture.profiles.values.empty());
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
}

TEST_CASE(later_start_of_uninitialized_provisioning_profile_runs_one_time_provisioning) {
    Fixture fixture;
    auto created = dispatch_create(fixture, true);
    REQUIRE(fixture.core.execute_operation(created.operation_id.value()).ok());
    fixture.operations.record.reset();
    fixture.worker.calls.clear();

    const auto started = dispatch_start(
        fixture, "30000000-0000-4000-8000-000000000003");
    auto terminal = fixture.core.execute_operation(started.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    const auto& profile = fixture.profiles.values.front();
    REQUIRE_EQ(profile.record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
    REQUIRE_EQ(profile.state.disk.runtime, product::RuntimeState::running);
    REQUIRE(std::any_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::provision_gpt_ntfs;
        }));
    REQUIRE(profile.record.profile.volume_guid.has_value());
}

TEST_CASE(later_start_with_complete_pinned_identity_verifies_and_never_reformats) {
    Fixture fixture;
    auto created = dispatch_create(fixture, true);
    REQUIRE(fixture.core.execute_operation(created.operation_id.value()).ok());
    auto& profile = fixture.profiles.values.front();
    profile.record.profile.gpt_disk_guid =
        uuid("22000000-0000-4000-8000-000000000001");
    profile.record.profile.partition_guid =
        uuid("22000000-0000-4000-8000-000000000002");
    profile.record.profile.volume_guid =
        uuid("22000000-0000-4000-8000-000000000003");
    fixture.operations.record.reset();
    fixture.worker.calls.clear();

    const auto started = dispatch_start(
        fixture, "30000000-0000-4000-8000-000000000004");
    auto terminal = fixture.core.execute_operation(started.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE(std::any_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action
                == ActionKind::verify_existing_volume_identity;
        }));
    REQUIRE(std::none_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::prove_new_uninitialized
                || call.action == ActionKind::provision_gpt_ntfs
                || call.action == ActionKind::record_volume_identity;
        }));
}

TEST_CASE(later_start_with_partial_identity_is_attention_without_worker_or_format) {
    Fixture fixture;
    auto created = dispatch_create(fixture, true);
    REQUIRE(fixture.core.execute_operation(created.operation_id.value()).ok());
    auto& profile = fixture.profiles.values.front();
    profile.record.profile.gpt_disk_guid =
        uuid("23000000-0000-4000-8000-000000000001");
    fixture.operations.record.reset();
    fixture.worker.calls.clear();

    const auto started = dispatch_start(
        fixture, "30000000-0000-4000-8000-000000000005");
    auto terminal = fixture.core.execute_operation(started.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(profile.record.profile.lifecycle,
               profile::ProfileLifecycle::provisioning);
    REQUIRE_EQ(profile.state.disk.runtime, product::RuntimeState::stopped);
    REQUIRE_EQ(profile.state.disk.health,
               product::HealthState::needs_attention);
}

TEST_CASE(memory_materialization_uses_frozen_checked_block_aligned_values) {
    const auto physical = PhysicalMemorySnapshot{16ULL << 30, 8ULL << 30};
    auto automatic = materialize_memory_policy(
        control::MemoryMode::automatic,
        std::nullopt,
        8ULL << 30,
        physical);
    REQUIRE(automatic.ok());
    REQUIRE_EQ(automatic.value().mode, profile::MemoryMode::automatic);
    REQUIRE_EQ(automatic.value().hard_limit_bytes, 4ULL << 30);
    REQUIRE_EQ(
        automatic.value().hard_limit_bytes
            % profile::kProfileMemoryAlignmentBytes,
        0U);
    REQUIRE_EQ(
        automatic.value().soft_limit_bytes
            % profile::kProfileMemoryAlignmentBytes,
        0U);
    REQUIRE_EQ(
        automatic.value().low_watermark_bytes
            % profile::kProfileMemoryAlignmentBytes,
        0U);
    REQUIRE(automatic.value().low_watermark_bytes
            < automatic.value().soft_limit_bytes);
    REQUIRE(automatic.value().soft_limit_bytes
            < automatic.value().hard_limit_bytes);
    REQUIRE_EQ(
        automatic.value().snapshot_cow_limit_bytes,
        kAutomaticSnapshotCowLimitBytes);

    auto fixed = materialize_memory_policy(
        control::MemoryMode::fixed,
        2ULL << 30,
        8ULL << 30,
        physical);
    REQUIRE(fixed.ok());
    REQUIRE_EQ(fixed.value().mode, profile::MemoryMode::fixed);
    REQUIRE_EQ(fixed.value().hard_limit_bytes, 2ULL << 30);
}

TEST_CASE(memory_materialization_rejects_ambiguous_or_unviable_requests) {
    const auto physical = PhysicalMemorySnapshot{16ULL << 30, 8ULL << 30};
    REQUIRE(!materialize_memory_policy(
        control::MemoryMode::automatic,
        1ULL << 30,
        8ULL << 30,
        physical).ok());
    REQUIRE(!materialize_memory_policy(
        control::MemoryMode::fixed,
        std::nullopt,
        8ULL << 30,
        physical).ok());
    REQUIRE(!materialize_memory_policy(
        control::MemoryMode::fixed,
        16ULL << 30,
        8ULL << 30,
        physical).ok());
    REQUIRE(!materialize_memory_policy(
        control::MemoryMode::automatic,
        std::nullopt,
        kAutomaticSnapshotCowLimitBytes - 1,
        physical).ok());
    REQUIRE(!materialize_memory_policy(
        control::MemoryMode::automatic,
        std::nullopt,
        8ULL << 30,
        PhysicalMemorySnapshot{8ULL << 30, 9ULL << 30}).ok());
}

TEST_CASE(registration_rejects_any_nonfrozen_payload_block_geometry) {
    Fixture create_fixture;
    create_fixture.worker.payload_block_bytes = 1ULL << 20;
    const auto created = dispatch_create(create_fixture, true);
    auto create_terminal = create_fixture.core.execute_operation(
        created.operation_id.value());
    REQUIRE(create_terminal.ok());
    REQUIRE_EQ(create_terminal.value().state, OperationState::failed);
    REQUIRE(create_fixture.profiles.values.empty());

    Fixture import_fixture;
    import_fixture.worker.payload_block_bytes = 4ULL << 20;
    const auto imported = dispatch_import(import_fixture);
    auto import_terminal = import_fixture.core.execute_operation(
        imported.operation_id.value());
    REQUIRE(import_terminal.ok());
    REQUIRE_EQ(import_terminal.value().state, OperationState::failed);
    REQUIRE(import_fixture.profiles.values.empty());
    REQUIRE_EQ(kProductLogicalSectorBytes, 512U);
    REQUIRE_EQ(kProductPhysicalSectorBytes, 4096U);
    REQUIRE_EQ(kProductPayloadBlockBytes, 2ULL << 20);
}

TEST_CASE(initial_provisioning_commit_then_indeterminate_is_resolved_exactly) {
    Fixture fixture;
    fixture.registrations.publish_disposition =
        RegistrationCommitDisposition::indeterminate;
    const auto created = dispatch_create(fixture, true);
    auto terminal = fixture.core.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.values.size(), 1U);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE_EQ(fixture.profiles.values.front().record.profile.lifecycle,
               profile::ProfileLifecycle::provisioning);
}

TEST_CASE(imported_ready_commit_then_indeterminate_is_resolved_exactly) {
    Fixture fixture;
    fixture.registrations.publish_disposition =
        RegistrationCommitDisposition::indeterminate;
    const auto imported = dispatch_import(fixture);
    auto terminal = fixture.core.execute_operation(
        imported.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE_EQ(fixture.profiles.values.front().record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
}

TEST_CASE(volume_identity_commit_then_indeterminate_is_resolved_exactly) {
    Fixture fixture;
    fixture.registrations.affected_update_call = 1;
    fixture.registrations.affected_update_disposition =
        RegistrationCommitDisposition::indeterminate;
    const auto created = dispatch_create(fixture, false);
    auto terminal = fixture.core.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE(fixture.profiles.values.front().record.profile.volume_guid
                .has_value());
}

TEST_CASE(ready_commit_then_indeterminate_is_resolved_exactly) {
    Fixture fixture;
    fixture.registrations.affected_update_call = 2;
    fixture.registrations.affected_update_disposition =
        RegistrationCommitDisposition::indeterminate;
    const auto created = dispatch_create(fixture, false);
    auto terminal = fixture.core.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE_EQ(fixture.profiles.values.front().record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
}

TEST_CASE(ambiguous_initial_profile_commit_is_terminal_without_retrying_publish) {
    Fixture fixture;
    fixture.registrations.publish_disposition =
        RegistrationCommitDisposition::indeterminate;
    fixture.registrations.resolution_ambiguous = true;
    const auto created = dispatch_create(fixture, true);
    auto terminal = fixture.core.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
}

TEST_CASE(ambiguous_ready_commit_terminalizes_without_overwriting_durable_truth) {
    Fixture fixture;
    fixture.registrations.affected_update_call = 2;
    fixture.registrations.affected_update_disposition =
        RegistrationCommitDisposition::indeterminate;
    fixture.registrations.resolution_ambiguous = true;
    const auto created = dispatch_create(fixture, false);
    auto terminal = fixture.core.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.registrations.update_calls, 2U);
    REQUIRE_EQ(fixture.registrations.resolve_calls, 1U);
    REQUIRE_EQ(fixture.profiles.values.size(), 1U);
    REQUIRE_EQ(fixture.profiles.values.front().record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
    REQUIRE(fixture.profiles.values.front().record.profile.volume_guid
                .has_value());
    REQUIRE_EQ(
        fixture.profiles.values.front().operation_gate.active_operation_id,
        created.operation_id);
    REQUIRE_EQ(terminal.value().phase,
               std::string("terminal-indeterminate"));

    auto recovered = fixture.core.recover_after_restart();
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        fixture.profiles.values.front().operation_gate.active_operation_id,
        created.operation_id);
}

TEST_CASE(registration_reservation_failure_is_terminal_and_queryable_without_profile) {
    Fixture fixture;
    fixture.registrations.reserve_error =
        test_error(L"fake_registrations.reserve");
    fixture.ids.values.push_back(
        uuid("40000000-0000-4000-8000-000000000004"));
    auto response = fixture.core.dispatch(ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000006"),
        create_command(false),
        administrator(),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(response.ok());
    REQUIRE(response.value().operation_id.has_value());
    REQUIRE(response.value().operation.has_value());
    REQUIRE_EQ(response.value().disposition,
               ServiceResponseDisposition::completed);
    REQUIRE_EQ(response.value().operation->state, OperationState::failed);
    REQUIRE(fixture.profiles.values.empty());
    REQUIRE(fixture.worker.calls.empty());
    REQUIRE_EQ(fixture.operations.history.size(), 1U);
    REQUIRE(std::none_of(
        fixture.operations.history.begin(), fixture.operations.history.end(),
        [](const auto& record) {
            return record.state == OperationState::running;
        }));

    auto queried = fixture.core.dispatch(ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000007"),
        control::OperationShowCommand{
            response.value().operation_id.value()},
        administrator(),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(queried.ok());
    REQUIRE(queried.value().operation.has_value());
    REQUIRE_EQ(queried.value().operation->state, OperationState::failed);
}

TEST_CASE(crash_after_reservation_before_in_memory_enqueue_rebuilds_from_durable_intent) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);
    REQUIRE_EQ(fixture.registrations.reservations.size(), 1U);

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
}

TEST_CASE(service_restart_reconciles_and_rebuilds_registration_execution) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().size(), 1U);
    REQUIRE_EQ(fixture.operations.record->state, OperationState::running);
    REQUIRE_EQ(fixture.operations.record->phase, std::string("admitting"));

    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
}

TEST_CASE(request_replay_after_process_restart_executes_reserved_intent_once) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    fixture.clock.unix_ms += 30'000;
    fixture.memory.available = 4ULL << 30;
    auto replayed = restarted.dispatch(ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000001"),
        create_command(true),
        administrator(),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(replayed.ok());
    REQUIRE_EQ(replayed.value().operation_id, created.operation_id);
    REQUIRE_EQ(fixture.registrations.reservations.size(), 1U);

    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.registrations.publish_calls, 1U);
}

TEST_CASE(restart_after_ready_publication_resumes_mount_and_readiness_only) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, false);

    Fixture seed;
    const auto seeded = dispatch_create(seed, true);
    REQUIRE(seed.core.execute_operation(seeded.operation_id.value()).ok());
    auto ready = seed.profiles.values.front();
    ++ready.record.revision;
    ready.record.profile.lifecycle = profile::ProfileLifecycle::ready;
    ready.record.profile.gpt_disk_guid =
        uuid("24000000-0000-4000-8000-000000000001");
    ready.record.profile.partition_guid =
        uuid("24000000-0000-4000-8000-000000000002");
    ready.record.profile.volume_guid =
        uuid("24000000-0000-4000-8000-000000000003");
    const auto plan = plan_command(
        absent_profile_state(), ManagementCommand::create_and_start);
    REQUIRE(plan.accepted());
    ready.state = plan.admitted;
    ready.state.lifecycle = profile::ProfileLifecycle::ready;
    const auto acquired = plan_gate_access(
        idle_operation_gate(), plan, created.operation_id.value());
    REQUIRE(acquired.accepted());
    const auto noncancellable = mark_operation_noncancellable(
        acquired.after, created.operation_id.value());
    REQUIRE(noncancellable.accepted());
    ready.operation_gate = noncancellable.after;
    fixture.profiles.add(ready);
    fixture.operations.record->cancellable = false;

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 2U);
    REQUIRE_EQ(fixture.worker.calls[0].action,
               ActionKind::assign_exact_mount);
    REQUIRE_EQ(fixture.worker.calls[1].action,
               ActionKind::readiness_io);
}

TEST_CASE(restart_after_volume_identity_never_reformats_and_finishes_checkpoint) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, false);

    Fixture seed;
    const auto seeded = dispatch_create(seed, true);
    REQUIRE(seed.core.execute_operation(seeded.operation_id.value()).ok());
    auto provisioning = seed.profiles.values.front();
    ++provisioning.record.revision;
    provisioning.record.profile.gpt_disk_guid =
        uuid("25000000-0000-4000-8000-000000000001");
    provisioning.record.profile.partition_guid =
        uuid("25000000-0000-4000-8000-000000000002");
    provisioning.record.profile.volume_guid =
        uuid("25000000-0000-4000-8000-000000000003");
    const auto plan = plan_command(
        absent_profile_state(), ManagementCommand::create_and_start);
    REQUIRE(plan.accepted());
    provisioning.state = plan.admitted;
    const auto acquired = plan_gate_access(
        idle_operation_gate(), plan, created.operation_id.value());
    REQUIRE(acquired.accepted());
    const auto noncancellable = mark_operation_noncancellable(
        acquired.after, created.operation_id.value());
    REQUIRE(noncancellable.accepted());
    provisioning.operation_gate = noncancellable.after;
    fixture.profiles.add(provisioning);
    fixture.operations.record->cancellable = false;

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE(std::any_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action
                == ActionKind::verify_existing_volume_identity;
        }));
    REQUIRE(std::any_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::checkpoint;
        }));
    REQUIRE(std::none_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::prove_new_uninitialized
                || call.action == ActionKind::provision_gpt_ntfs
                || call.action == ActionKind::record_volume_identity;
        }));
    REQUIRE_EQ(fixture.profiles.values.front().record.profile.lifecycle,
               profile::ProfileLifecycle::ready);
}

TEST_CASE(bundle_evidence_survives_crash_and_create_runs_at_most_once) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);
    auto& reservation = fixture.registrations.reservations.front();
    auto produced = fixture.worker.execute(WorkerActionRequest{
        reservation.request_id,
        reservation.operation_id,
        reservation.intent.profile_id,
        ManagementCommand::create_stopped,
        ActionKind::create_and_verify_bundle,
        reservation.intent,
        std::nullopt});
    REQUIRE(produced.ok());
    REQUIRE(produced.value().lifecycle.bundle.has_value());
    ++reservation.revision;
    reservation.phase = LifecycleReservationPhase::bundle_verified;
    reservation.bundle = produced.value().lifecycle.bundle;
    fixture.operations.record->phase = "create-bundle";

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(std::count_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::create_and_verify_bundle;
        }), 1);
    REQUIRE_EQ(std::count_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::inspect_bundle_read_only;
        }), 1);
    REQUIRE_EQ(fixture.profiles.values.size(), 1U);
}

TEST_CASE(unknown_create_completion_never_retries_or_overwrites_bundle) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);
    auto& reservation = fixture.registrations.reservations.front();
    auto produced = fixture.worker.execute(WorkerActionRequest{
        reservation.request_id,
        reservation.operation_id,
        reservation.intent.profile_id,
        ManagementCommand::create_stopped,
        ActionKind::create_and_verify_bundle,
        reservation.intent,
        std::nullopt});
    REQUIRE(produced.ok());
    ++reservation.revision;
    reservation.phase = LifecycleReservationPhase::bundle_action_started;
    fixture.operations.record->phase = "create-bundle";

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE_EQ(std::count_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::create_and_verify_bundle;
        }), 1);
    REQUIRE_EQ(fixture.worker.calls.size(), 1U);
    REQUIRE(fixture.profiles.values.empty());
}

TEST_CASE(profile_commit_before_phase_update_resumes_without_bundle_action) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);

    Fixture seed;
    const auto seeded = dispatch_create(seed, true);
    REQUIRE(seed.core.execute_operation(seeded.operation_id.value()).ok());
    auto provisioning = seed.profiles.values.front();
    const auto plan = plan_command(
        absent_profile_state(), ManagementCommand::create_stopped);
    REQUIRE(plan.accepted());
    provisioning.state = plan.admitted;
    const auto acquired = plan_gate_access(
        idle_operation_gate(), plan, created.operation_id.value());
    REQUIRE(acquired.accepted());
    const auto noncancellable = mark_operation_noncancellable(
        acquired.after, created.operation_id.value());
    REQUIRE(noncancellable.accepted());
    provisioning.operation_gate = noncancellable.after;
    fixture.profiles.add(provisioning);
    fixture.operations.record->cancellable = false;
    fixture.operations.record->phase = "publish-provisioning";

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(import_inspection_evidence_is_revalidated_read_only_after_crash) {
    Fixture fixture;
    const auto imported = dispatch_import(fixture);
    auto& reservation = fixture.registrations.reservations.front();
    auto inspected = fixture.worker.execute(WorkerActionRequest{
        reservation.request_id,
        reservation.operation_id,
        reservation.intent.profile_id,
        ManagementCommand::import_profile,
        ActionKind::inspect_bundle_read_only,
        reservation.intent,
        std::nullopt});
    REQUIRE(inspected.ok());
    REQUIRE(inspected.value().lifecycle.bundle.has_value());
    REQUIRE(inspected.value().lifecycle.volume.has_value());
    ++reservation.revision;
    reservation.phase = LifecycleReservationPhase::inspection_verified;
    reservation.bundle = inspected.value().lifecycle.bundle;
    reservation.volume = inspected.value().lifecycle.volume;
    fixture.operations.record->phase = "inspect-bundle";

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(imported.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 2U);
    REQUIRE(std::all_of(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::inspect_bundle_read_only;
        }));
    REQUIRE_EQ(fixture.profiles.values.size(), 1U);
}

TEST_CASE(durable_bundle_evidence_mismatch_is_attention_without_recreate) {
    Fixture fixture;
    const auto created = dispatch_create(fixture, true);
    auto& reservation = fixture.registrations.reservations.front();
    auto produced = fixture.worker.execute(WorkerActionRequest{
        reservation.request_id,
        reservation.operation_id,
        reservation.intent.profile_id,
        ManagementCommand::create_stopped,
        ActionKind::create_and_verify_bundle,
        reservation.intent,
        std::nullopt});
    REQUIRE(produced.ok());
    ++reservation.revision;
    reservation.phase = LifecycleReservationPhase::bundle_verified;
    reservation.bundle = produced.value().lifecycle.bundle;
    fixture.worker.create_file_identity.file_id[0] = std::byte{99};
    fixture.operations.record->phase = "create-bundle";

    ServiceCore restarted{
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory, &fixture.registrations};
    REQUIRE(restarted.recover_after_restart().ok());
    auto terminal = restarted.execute_operation(created.operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::failed);
    REQUIRE_EQ(std::count_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::create_and_verify_bundle;
        }), 1);
    REQUIRE_EQ(std::count_if(
        fixture.worker.calls.begin(), fixture.worker.calls.end(),
        [](const auto& call) {
            return call.action == ActionKind::inspect_bundle_read_only;
        }), 1);
    REQUIRE(fixture.profiles.values.empty());
}
