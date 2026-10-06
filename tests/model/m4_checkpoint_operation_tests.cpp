#include "tests/support/test_framework.h"

#include "src/management/service_core.h"

#include <algorithm>
#include <optional>
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

Error error(const wchar_t* operation) {
    return {ErrorDomain::internal,
            static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
            operation, L"Injected checkpoint model failure"};
}

Error profile_not_found_error(const wchar_t* operation) {
    return {
        ErrorDomain::internal,
        static_cast<std::uint32_t>(ServiceCoreError::profile_not_found),
        operation,
        L"The requested profile does not exist"};
}

BundleFileIdentity test_bundle_file_identity() {
    BundleFileIdentity identity{};
    identity.volume_serial = 0x10203040U;
    identity.file_id[0] = std::byte{0x10};
    identity.file_id[7] = std::byte{0x7a};
    identity.file_id[15] = std::byte{0xf0};
    return identity;
}

std::uint64_t checkpoint_terminal_generation(
    const std::vector<std::byte>& terminal_result) {
    constexpr std::size_t kGenerationOffset = 50;
    REQUIRE(terminal_result.size() >= kGenerationOffset + sizeof(std::uint64_t));
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(
                         terminal_result[kGenerationOffset + index]))
            << (index * 8U);
    }
    return value;
}

ipc::CallerIdentity caller(const std::wstring& sid) {
    return {sid, true, false, false, false, false, false, false};
}

ManagedProfileSnapshot running_profile(product::PersistenceState persistence) {
    profile::Profile value{
        uuid("11111111-1111-4111-8111-111111111111"),
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        profile::ProfileLifecycle::ready,
        L"D:\\Images\\work.myrd",
        "manifest.myrd",
        uuid("22222222-2222-4222-8222-222222222222"),
        8ULL << 30,
        512,
        4096,
        uuid("33333333-3333-4333-8333-333333333331"),
        uuid("33333333-3333-4333-8333-333333333332"),
        uuid("33333333-3333-4333-8333-333333333333"),
        profile::MountKind::drive_letter,
        L"R:",
        true,
        profile::MemoryPolicy{
            profile::MemoryMode::automatic,
            1ULL << 30,
            896ULL << 20,
            768ULL << 20,
            128ULL << 20},
        profile::CheckpointPolicy{},
        profile::CompactionPolicy{}};
    return {
        profile::ProfileRecord{7, std::move(value)},
        registered_profile_state(
            profile::ProfileLifecycle::ready,
            product::DiskState{
                product::RuntimeState::running,
                product::ActivityState::idle,
                persistence,
                product::HealthState::healthy}),
        idle_operation_gate(),
        CheckpointDurableState{
            5,
            11,
            CheckpointDurablePhase::stable}};
}

// A profile that has not finished provisioning: not running, and with no
// checkpoint behind it.  This is the only state a `disk start` still has work
// to do in, and the only one whose plan performs a checkpoint of its own --
// `append_start_actions` appends `flush_exact_volume` and then `checkpoint`
// whenever the plan was planned from the provisioning lifecycle.
ManagedProfileSnapshot provisioning_profile() {
    auto snapshot = running_profile(product::PersistenceState::clean);
    snapshot.record.profile.lifecycle = profile::ProfileLifecycle::provisioning;
    snapshot.state = registered_profile_state(
        profile::ProfileLifecycle::provisioning,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
    snapshot.checkpoint = CheckpointDurableState{
        0, 0, CheckpointDurablePhase::stable};
    return snapshot;
}

// Selects the provisioning profile instead of the ready one, so a test can say
// which starting state it means without a second fixture type.
struct Provisioning final {};

class Clock final : public IServiceClock {
public:
    std::uint64_t unix_time_ms() const noexcept override { return 1000; }
    std::uint64_t monotonic_time_ms() const noexcept override { return 2000; }
};

class Profiles final : public IServiceProfileStore {
public:
    explicit Profiles(ManagedProfileSnapshot snapshot)
        : snapshot_(std::move(snapshot)) {}

    Result<ManagedProfileSnapshot> resolve(std::wstring_view target) override {
        if (forgotten) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.resolve.forgotten"));
        }
        if (target == snapshot_.record.profile.display_name) {
            return Result<ManagedProfileSnapshot>::success(snapshot_);
        }
        if (secondary_.has_value()
            && target == secondary_->record.profile.display_name) {
            return Result<ManagedProfileSnapshot>::success(
                secondary_.value());
        }
        return Result<ManagedProfileSnapshot>::failure(error(L"profiles.resolve"));
    }

    Result<ManagedProfileSnapshot> load(
        const product::Uuid& profile_id) override {
        if (forgotten) {
            if (fail_forgotten_load) {
                return Result<ManagedProfileSnapshot>::failure(
                    error(L"profiles.load.forgotten.injected"));
            }
            return Result<ManagedProfileSnapshot>::failure(
                profile_not_found_error(L"profiles.load.forgotten"));
        }
        if (fail_current_load) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.load.corrupt_current"));
        }
        if (profile_id == snapshot_.record.profile.profile_uuid) {
            return Result<ManagedProfileSnapshot>::success(snapshot_);
        }
        if (secondary_.has_value()
            && profile_id == secondary_->record.profile.profile_uuid) {
            return Result<ManagedProfileSnapshot>::success(
                secondary_.value());
        }
        return Result<ManagedProfileSnapshot>::failure(
            profile_not_found_error(L"profiles.load"));
    }

    Result<ManagedProfileSnapshot> publish_runtime(
        const ManagedProfileSnapshot& expected,
        const ProfileState& state,
        const OperationGateState& gate) override {
        if (fail_runtime_publish_once_before) {
            fail_runtime_publish_once_before = false;
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.runtime.injected"));
        }
        auto published = publish_all(
            expected, state, gate, expected.checkpoint,
            expected.compaction);
        if (published.ok() && fail_runtime_publish_once_after) {
            fail_runtime_publish_once_after = false;
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.runtime.after.injected"));
        }
        return published;
    }

    Result<ManagedProfileSnapshot> publish_checkpoint(
        const ManagedProfileSnapshot& expected,
        const ProfileState& state,
        const OperationGateState& gate,
        const CheckpointDurableState& checkpoint) override {
        if (before_checkpoint_publish && !inside_publish_callback) {
            inside_publish_callback = true;
            auto callback = std::move(before_checkpoint_publish);
            before_checkpoint_publish = {};
            callback(*this);
            inside_publish_callback = false;
        }
        auto published = publish_all(
            expected, state, gate, checkpoint, expected.compaction);
        if (published.ok()
            && fail_after_checkpoint_phase_once
                == std::optional<CheckpointDurablePhase>(checkpoint.phase)) {
            fail_after_checkpoint_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.checkpoint.after.injected"));
        }
        return published;
    }

    Result<ManagedProfileSnapshot> publish_compaction(
        const ManagedProfileSnapshot& expected,
        const ProfileState& state,
        const OperationGateState& gate,
        const CheckpointDurableState& checkpoint,
        const CompactionDurableState& compaction) override {
        if (before_checkpoint_publish && !inside_publish_callback) {
            inside_publish_callback = true;
            auto callback = std::move(before_checkpoint_publish);
            before_checkpoint_publish = {};
            callback(*this);
            inside_publish_callback = false;
        }
        return publish_all(expected, state, gate, checkpoint, compaction);
    }

    Result<ManagedProfileSnapshot> publish_stop(
        const ManagedProfileSnapshot& expected,
        const ProfileState& state,
        const OperationGateState& gate,
        const CheckpointDurableState& checkpoint,
        const CompactionDurableState& compaction,
        const StopDurableState& stop) override {
        if (fail_before_stop_phase_once
            == std::optional<StopDurablePhase>(stop.phase)) {
            fail_before_stop_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.stop.injected"));
        }
        auto published = publish_all(
            expected, state, gate, checkpoint, compaction);
        if (!published.ok()) return published;
        snapshot_.stop = stop;
        if (secondary_.has_value()
            && secondary_->record.profile.profile_uuid
                == expected.record.profile.profile_uuid) {
            secondary_->stop = stop;
            if (fail_after_stop_phase_once
                == std::optional<StopDurablePhase>(stop.phase)) {
                fail_after_stop_phase_once.reset();
                return Result<ManagedProfileSnapshot>::failure(
                    error(L"profiles.publish.stop.after.injected"));
            }
            return Result<ManagedProfileSnapshot>::success(
                secondary_.value());
        }
        publications.back().stop = stop;
        if (fail_after_stop_phase_once
            == std::optional<StopDurablePhase>(stop.phase)) {
            fail_after_stop_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.stop.after.injected"));
        }
        return Result<ManagedProfileSnapshot>::success(snapshot_);
    }

    Result<ManagedProfileSnapshot> publish_rebind(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& bundle,
        const ProfileState& state,
        const OperationGateState& gate) override {
        ++rebind_calls;
        if (fail_rebind_cas || snapshot_ != expected
            || proposed.revision != expected.record.revision + 1
            || bundle.image_id != proposed.profile.image_uuid
            || bundle.logical_capacity_bytes
                != proposed.profile.logical_capacity_bytes
            || bundle.logical_sector_bytes
                != proposed.profile.logical_sector_size
            || bundle.physical_sector_bytes
                != proposed.profile.physical_sector_size) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.rebind.conflict"));
        }
        snapshot_.record = proposed;
        snapshot_.state = state;
        snapshot_.operation_gate = gate;
        return Result<ManagedProfileSnapshot>::success(snapshot_);
    }

    Result<ManagedProfileSnapshot> load_repair_candidate(
        const product::Uuid& profile_id) override {
        ++repair_candidate_loads;
        if (!repair_candidate.has_value()
            || repair_candidate->record.profile.profile_uuid != profile_id) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.repair_candidate"));
        }
        return Result<ManagedProfileSnapshot>::success(
            repair_candidate.value());
    }

    Result<ManagedProfileSnapshot> repair_from_previous(
        const ManagedProfileSnapshot& expected_previous,
        const ProfileState& state,
        const OperationGateState& gate) override {
        ++repair_calls;
        if (!repair_candidate.has_value()
            || repair_candidate.value() != expected_previous
            || fail_repair) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.repair.conflict"));
        }
        auto repaired = expected_previous;
        ++repaired.record.revision;
        repaired.state = state;
        repaired.operation_gate = gate;
        snapshot_ = repaired;
        repair_candidate.reset();
        fail_current_load = false;
        return Result<ManagedProfileSnapshot>::success(snapshot_);
    }

    Result<RecoveryCommandReservation> reserve_recovery_command(
        const ManagedProfileSnapshot& expected,
        const RecoveryCommandReservation& proposed) override {
        ++recovery_reserve_calls;
        if (fail_recovery_reserve_once) {
            fail_recovery_reserve_once = false;
            return Result<RecoveryCommandReservation>::failure(
                error(L"profiles.recovery.reserve.injected"));
        }
        const bool repair = proposed.intent.kind
            == RecoveryOperationKind::repair_profile;
        const bool expected_matches = repair
            ? repair_candidate.has_value()
                && repair_candidate.value() == expected
            : snapshot_ == expected;
        if (!expected_matches
            || proposed.intent.profile_id
                != expected.record.profile.profile_uuid
            || proposed.expected_profile != expected.record
            || proposed.before_state != expected.state) {
            return Result<RecoveryCommandReservation>::failure(
                error(L"profiles.recovery.reserve"));
        }
        if (recovery_reservation.has_value()
            && recovery_reservation.value() != proposed) {
            return Result<RecoveryCommandReservation>::failure(
                error(L"profiles.recovery.reserve.conflict"));
        }
        recovery_reservation = proposed;
        if (fail_recovery_reserve_after_commit_once) {
            fail_recovery_reserve_after_commit_once = false;
            return Result<RecoveryCommandReservation>::failure(
                error(L"profiles.recovery.reserve.after.injected"));
        }
        return Result<RecoveryCommandReservation>::success(proposed);
    }

    Result<std::optional<RecoveryCommandReservation>> find_recovery_command(
        const product::Uuid& operation_id) override {
        ++recovery_load_calls;
        if (!recovery_reservation.has_value()
            || recovery_reservation->operation_id != operation_id) {
            return Result<std::optional<RecoveryCommandReservation>>::success(
                std::nullopt);
        }
        return Result<std::optional<RecoveryCommandReservation>>::success(
            recovery_reservation.value());
    }

    Result<ForgetTombstone> reserve_forget(
        const ManagedProfileSnapshot& expected,
        const ForgetTombstone& proposed) override {
        ++forget_reserve_calls;
        if (fail_forget_reserve_once) {
            fail_forget_reserve_once = false;
            return Result<ForgetTombstone>::failure(
                error(L"profiles.forget.reserve.injected"));
        }
        if (snapshot_ != expected
            || proposed.profile_id
                != expected.record.profile.profile_uuid
            || proposed.image_id != expected.record.profile.image_uuid
            || proposed.profile_revision != expected.record.revision
            || proposed.retained_bundle_path
                != expected.record.profile.bundle_path
            || proposed.authorization_owner_sid
                != expected.record.profile.owner_sid
            || proposed.retained_bundle_identity.has_value()
            || proposed.before_state != expected.state
            || proposed.phase != ForgetTombstonePhase::reserved) {
            return Result<ForgetTombstone>::failure(
                error(L"profiles.forget.reserve"));
        }
        auto durable = proposed;
        durable.retained_bundle_identity = registered_bundle_identity;
        if (forget_tombstone.has_value()
            && forget_tombstone.value() != durable) {
            return Result<ForgetTombstone>::failure(
                error(L"profiles.forget.reserve.conflict"));
        }
        forget_tombstone = durable;
        if (fail_forget_reserve_after_commit_once) {
            fail_forget_reserve_after_commit_once = false;
            return Result<ForgetTombstone>::failure(
                error(L"profiles.forget.reserve.after.injected"));
        }
        return Result<ForgetTombstone>::success(std::move(durable));
    }

    Result<std::optional<ForgetTombstone>> find_forget_tombstone(
        const product::Uuid& operation_id) override {
        if (fail_forget_lookup_once) {
            fail_forget_lookup_once = false;
            return Result<std::optional<ForgetTombstone>>::failure(
                error(L"profiles.forget.lookup.injected"));
        }
        if (!forget_tombstone.has_value()
            || forget_tombstone->operation_id != operation_id) {
            return Result<std::optional<ForgetTombstone>>::success(
                std::nullopt);
        }
        return Result<std::optional<ForgetTombstone>>::success(
            forget_tombstone.value());
    }

    Result<ForgetTombstone> remove_forget_metadata(
        const ForgetMetadataRemovalRequest& request,
        const ForgetSafetyEvidence& proof) override {
        ++forget_remove_calls;
        if (forgotten
            || !forget_tombstone.has_value()
            || request.operation_id != forget_tombstone->operation_id
            || request.request_id != forget_tombstone->request_id
            || request.profile_id != snapshot_.record.profile.profile_uuid
            || request.image_id != snapshot_.record.profile.image_uuid
            || request.profile_revision != snapshot_.record.revision
            || request.expected_state != snapshot_.state
            || request.expected_gate != snapshot_.operation_gate
            || proof.profile_id != forget_tombstone->profile_id
            || proof.image_id != forget_tombstone->image_id
            || proof.profile_revision != forget_tombstone->profile_revision
            || !proof.worker_absent || !proof.writer_lock_absent
            || !proof.matching_device_absent || !proof.bundle_present
            || proof.retained_bundle_identity
                != forget_tombstone->retained_bundle_identity) {
            return Result<ForgetTombstone>::failure(
                error(L"profiles.forget.remove"));
        }
        auto removed = forget_tombstone.value();
        removed.phase = ForgetTombstonePhase::metadata_removed;
        forget_tombstone = removed;
        forgotten = true;
        return Result<ForgetTombstone>::success(std::move(removed));
    }

    const ManagedProfileSnapshot& snapshot() const noexcept { return snapshot_; }
    void replace(ManagedProfileSnapshot snapshot) {
        snapshot_ = std::move(snapshot);
    }
    void add_secondary(ManagedProfileSnapshot snapshot) {
        secondary_ = std::move(snapshot);
    }
    const ManagedProfileSnapshot& secondary_snapshot() const {
        return secondary_.value();
    }
    // Enumeration is additive: M4 checkpoint tests never list, so this only
    // gives the M5 read-path tests something to enumerate.
    Result<std::vector<ManagedProfileSnapshot>> list_profiles() override {
        ++list_calls;
        std::vector<ManagedProfileSnapshot> listed{snapshot_};
        if (secondary_.has_value()) listed.push_back(secondary_.value());
        return Result<std::vector<ManagedProfileSnapshot>>::success(
            std::move(listed));
    }
    std::size_t list_calls{};
    std::vector<ManagedProfileSnapshot> publications;
    std::function<void(Profiles&)> before_checkpoint_publish;
    std::optional<CheckpointDurablePhase>
        fail_before_checkpoint_phase_once;
    std::optional<CheckpointDurablePhase>
        fail_after_checkpoint_phase_once;
    std::optional<StopDurablePhase> fail_before_stop_phase_once;
    std::optional<StopDurablePhase> fail_after_stop_phase_once;
    std::optional<CompactionDurablePhase>
        fail_before_compaction_phase_once;
    std::optional<CompactionDurablePhase>
        fail_after_compaction_phase_once;
    std::optional<ManagedProfileSnapshot> repair_candidate;
    bool fail_rebind_cas{};
    bool fail_runtime_publish_once_before{};
    bool fail_runtime_publish_once_after{};
    bool fail_repair{};
    bool fail_current_load{};
    std::size_t rebind_calls{};
    std::size_t repair_candidate_loads{};
    std::size_t repair_calls{};
    std::optional<RecoveryCommandReservation> recovery_reservation;
    std::size_t recovery_reserve_calls{};
    std::size_t recovery_load_calls{};
    bool fail_recovery_reserve_once{};
    bool fail_recovery_reserve_after_commit_once{};
    std::optional<ForgetTombstone> forget_tombstone;
    BundleFileIdentity registered_bundle_identity{
        test_bundle_file_identity()};
    bool forgotten{};
    bool fail_forgotten_load{};
    std::size_t forget_reserve_calls{};
    std::size_t forget_remove_calls{};
    bool fail_forget_reserve_once{};
    bool fail_forget_reserve_after_commit_once{};
    bool fail_forget_lookup_once{};

private:
    Result<ManagedProfileSnapshot> publish_all(
        const ManagedProfileSnapshot& expected,
        const ProfileState& state,
        const OperationGateState& gate,
        const CheckpointDurableState& checkpoint,
        const CompactionDurableState& compaction) {
        ManagedProfileSnapshot* target = nullptr;
        if (snapshot_ == expected) {
            target = &snapshot_;
        } else if (secondary_.has_value()
                   && secondary_.value() == expected) {
            target = &secondary_.value();
        }
        if (target == nullptr) {
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.conflict"));
        }
        if (fail_before_checkpoint_phase_once
                == std::optional<CheckpointDurablePhase>(checkpoint.phase)) {
            fail_before_checkpoint_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.checkpoint.injected"));
        }
        if (fail_before_compaction_phase_once
                == std::optional<CompactionDurablePhase>(compaction.phase)) {
            fail_before_compaction_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.compaction.injected"));
        }
        target->state = state;
        target->operation_gate = gate;
        target->checkpoint = checkpoint;
        target->compaction = compaction;
        publications.push_back(*target);
        if (fail_after_compaction_phase_once
                == std::optional<CompactionDurablePhase>(compaction.phase)) {
            fail_after_compaction_phase_once.reset();
            return Result<ManagedProfileSnapshot>::failure(
                error(L"profiles.publish.ack_lost"));
        }
        return Result<ManagedProfileSnapshot>::success(*target);
    }

    ManagedProfileSnapshot snapshot_;
    std::optional<ManagedProfileSnapshot> secondary_;
    bool inside_publish_callback{};
};

class Operations final : public IServiceOperationStore {
public:
    Result<std::optional<OperationAdmission>> find_request(
        const OperationRequest& request) override {
        const auto found = std::find_if(records.begin(), records.end(),
            [&](const auto& record) {
                return record.request_id == request.request_id;
            });
        if (found == records.end()) {
            return Result<std::optional<OperationAdmission>>::success(
                std::nullopt);
        }
        if (found->profile_id != request.profile_id
            || found->caller_sid != request.caller_sid
            || found->command_scope != request.command_scope
            || found->argument_digest != request.argument_digest) {
            return Result<std::optional<OperationAdmission>>::failure(
                error(L"operations.find.conflict"));
        }
        return Result<std::optional<OperationAdmission>>::success(
            OperationAdmission{*found, true});
    }

    Result<OperationAdmission> admit(
        const OperationRequest& request,
        const product::Uuid& candidate_operation_id) override {
        OperationRecord record{
            1,
            request.request_id,
            candidate_operation_id,
            request.profile_id,
            request.caller_sid,
            request.command_scope,
            request.argument_digest,
            OperationState::queued,
            "queued",
            request.created_unix_ms,
            request.created_unix_ms,
            std::nullopt,
            true,
            false,
            {}};
        records.push_back(record);
        return Result<OperationAdmission>::success({record, false});
    }

    Result<OperationRecord> load(
        const product::Uuid& operation_id) override {
        const auto found = std::find_if(records.begin(), records.end(),
            [&](const auto& record) {
                return record.operation_id == operation_id;
            });
        if (found == records.end()) {
            return Result<OperationRecord>::failure(error(L"operations.load"));
        }
        return Result<OperationRecord>::success(*found);
    }

    Result<void> write(
        std::uint64_t expected_revision,
        const OperationRecord& proposed) override {
        ++write_calls;
        if (failed_write_call.has_value()
            && failed_write_call.value() == write_calls) {
            return Result<void>::failure(error(L"operations.write.injected"));
        }
        const auto found = std::find_if(records.begin(), records.end(),
            [&](const auto& record) {
                return record.operation_id == proposed.operation_id;
            });
        if (found == records.end() || found->revision != expected_revision
            || proposed.revision != expected_revision + 1) {
            return Result<void>::failure(error(L"operations.write"));
        }
        *found = proposed;
        history.push_back(proposed);
        if (failed_after_write_call.has_value()
            && failed_after_write_call.value() == write_calls) {
            return Result<void>::failure(
                error(L"operations.write.after.injected"));
        }
        return Result<void>::success();
    }

    Result<std::vector<OperationRecord>> reconcile_after_restart(
        std::uint64_t now_unix_ms) override {
        for (auto& record : records) {
            if (record.state != OperationState::running) continue;
            ++record.revision;
            record.state = OperationState::reconciling;
            record.updated_unix_ms = (std::max)(
                record.updated_unix_ms, now_unix_ms);
            history.push_back(record);
        }
        return Result<std::vector<OperationRecord>>::success(records);
    }

    Result<OperationRecord> request_cancel(
        const product::Uuid& operation_id,
        std::uint64_t now_unix_ms) override {
        auto loaded = load(operation_id);
        if (!loaded.ok() || !loaded.value().cancellable) {
            return Result<OperationRecord>::failure(
                error(L"operations.cancel"));
        }
        auto proposed = loaded.value();
        ++proposed.revision;
        proposed.updated_unix_ms = now_unix_ms;
        proposed.cancel_requested = true;
        auto saved = write(loaded.value().revision, proposed);
        if (!saved.ok()) return Result<OperationRecord>::failure(saved.error());
        return Result<OperationRecord>::success(proposed);
    }

    Result<OperationWaitObservation> wait_for_change(
        const product::Uuid& operation_id,
        std::uint64_t after_revision,
        std::uint64_t) override {
        auto loaded = load(operation_id);
        if (!loaded.ok()) {
            return Result<OperationWaitObservation>::failure(loaded.error());
        }
        if (loaded.value().revision > after_revision) {
            return Result<OperationWaitObservation>::success(
                {OperationWaitDisposition::changed, loaded.move_value()});
        }
        return Result<OperationWaitObservation>::success(
            {OperationWaitDisposition::timed_out, std::nullopt});
    }

    std::vector<OperationRecord> records;
    std::vector<OperationRecord> history;
    std::size_t write_calls{};
    std::optional<std::size_t> failed_write_call;
    std::optional<std::size_t> failed_after_write_call;
};

class Worker final : public IWorkerControl {
public:
    WorkerCheckpointEvidence evidence(
        const CheckpointCommitBoundaryEvidence& correlation,
        CheckpointCommitDisposition disposition) const {
        WorkerCheckpointEvidence value{
            correlation,
            disposition,
            6,
            live_dirty_epoch,
            std::nullopt,
            std::nullopt};
        if (disposition == CheckpointCommitDisposition::committed) {
            return value;
        }
        const auto code = disposition
                == CheckpointCommitDisposition::commit_indeterminate
            ? 12U : failure_code;
        value.error = Error{
            ErrorDomain::persistence,
            code,
            L"checkpoint.publish",
            L"Injected exact persistence disposition",
            1234};
        CheckpointPersistenceErrorContext context{
            "checkpoint.publish",
            code,
            4,
            1234,
            3907,
            {},
            6,
            disposition,
            "injected checkpoint evidence"};
        std::copy(
            correlation.image_id.bytes().begin(),
            correlation.image_id.bytes().end(),
            context.image_uuid.begin());
        value.error_context = std::move(context);
        return value;
    }

    WorkerCompactionEvidence compaction_evidence(
        const CompactionMutationBoundaryEvidence& correlation,
        CompactionCommitDisposition disposition) const {
        CompactionMetrics metrics{
            correlation.prior_generation,
            correlation.candidate_generation,
            correlation.source_base_bytes,
            correlation.source_journal_bytes,
            12288,
            4096,
            correlation.required_peak_free_bytes,
            correlation.available_free_bytes,
            3, 2, 4, 3, 1, 0, 1, 1, 0, 0};
        if (disposition == CompactionCommitDisposition::not_committed
            && zero_not_committed_candidate_metrics) {
            metrics.committed_generation = 0;
            metrics.candidate_base_bytes = 0;
            metrics.candidate_journal_bytes = 0;
        }
        WorkerCompactionEvidence value{
            correlation,
            disposition,
            disposition == CompactionCommitDisposition::not_committed
                ? correlation.prior_generation
                : correlation.candidate_generation,
            live_dirty_epoch,
            metrics,
            std::nullopt,
            std::nullopt};
        if (disposition == CompactionCommitDisposition::committed) {
            return value;
        }
        const auto code = disposition
                == CompactionCommitDisposition::commit_indeterminate
            ? 12U : compaction_failure_code;
        value.error = Error{
            ErrorDomain::persistence,
            code,
            L"compaction.publish",
            L"Injected exact compaction disposition",
            1234};
        CompactionPersistenceErrorContext context{
            "compaction.publish",
            code,
            4,
            1234,
            correlation.native_m3_operation_id.value(),
            {},
            value.generation,
            disposition,
            "injected compaction evidence"};
        std::copy(
            correlation.image_id.bytes().begin(),
            correlation.image_id.bytes().end(),
            context.image_uuid.begin());
        value.error_context = std::move(context);
        return value;
    }

    bool begins_noncancellable(ActionKind) const noexcept override {
        return false;
    }

    Result<WorkerActionResult> execute(
        const WorkerActionRequest& request) override {
        calls.push_back(request.action);
        if (fail_action_once
            == std::optional<ActionKind>(request.action)) {
            fail_action_once.reset();
            return Result<WorkerActionResult>::failure(
                error(L"worker.action.injected"));
        }
        WorkerActionResult result{
            CompletionOutcome::succeeded_clean,
            {std::byte{'o'}, std::byte{'k'}}};
        if (request.action == ActionKind::verify_rebind_bundle) {
            if (!request.recovery_intent.has_value()
                || request.recovery_intent->kind
                    != RecoveryOperationKind::rebind
                || !request.durable_profile.has_value()) {
                return Result<WorkerActionResult>::failure(
                    error(L"worker.rebind.intent"));
            }
            BundleFileIdentity default_identity{7, {}};
            default_identity.file_id[0] = std::byte{1};
            result.lifecycle.bundle = rebind_evidence.value_or(
                BundleRegistrationEvidence{
                    request.recovery_intent->expected_image_id.value(),
                    request.durable_profile->profile.manifest_name,
                    request.durable_profile->profile.logical_capacity_bytes,
                    request.durable_profile->profile.logical_sector_size,
                    request.durable_profile->profile.physical_sector_size,
                    kProductPayloadBlockBytes,
                    default_identity,
                    BundleEvidenceSource::read_only_inspection});
        }
        return Result<WorkerActionResult>::success(std::move(result));
    }

    Result<WorkerActionResult> execute_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryHandler& boundary) override {
        calls.push_back(request.action);
        if (fail_before_boundary) {
            return Result<WorkerActionResult>::failure(
                error(L"worker.checkpoint.before_boundary"));
        }
        ++boundary_calls;
        CheckpointCommitBoundaryEvidence proposed{
            request.operation_id,
            request.profile_id,
            request.durable_profile->profile.image_uuid,
            4242,
            9001,
            3,
            5,
            6,
            checkpoint_cut_dirty_epoch,
            std::nullopt};
        if (before_boundary) before_boundary();
        auto decision = boundary(proposed);
        if (!decision.ok()) {
            return Result<WorkerActionResult>::failure(decision.error());
        }
        if (!decision.value().continue_commit) {
            return Result<WorkerActionResult>::success({
                CompletionOutcome::transient_failure,
                {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
                 std::byte{'c'}, std::byte{'e'}, std::byte{'l'}}});
        }
        if (after_boundary) after_boundary();
        ++commit_entries;
        last_correlation = proposed;
        if (fail_after_boundary) {
            return Result<WorkerActionResult>::failure(
                error(L"worker.checkpoint.after_boundary"));
        }
        auto checkpoint_evidence = evidence(proposed, initial_disposition);
        if (corrupt_checkpoint_evidence) {
            checkpoint_evidence.generation = proposed.candidate_generation + 1;
            if (checkpoint_evidence.error_context.has_value()) {
                checkpoint_evidence.error_context->generation =
                    checkpoint_evidence.generation;
            }
        }
        WorkerActionResult result{
            CompletionOutcome::succeeded_clean,
            {},
            {},
            checkpoint_evidence};
        return Result<WorkerActionResult>::success(std::move(result));
    }

    Result<WorkerActionResult> execute_compaction(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryHandler& boundary,
        const CompactionCutPublicationHandler& cut) override {
        calls.push_back(request.action);
        ++compaction_boundary_calls;
        const auto source_generation = last_correlation.has_value()
            ? last_correlation->candidate_generation : 5U;
        const auto candidate_generation = source_generation + 1U;
        if (compaction_low_space) {
            return Result<WorkerActionResult>::success(WorkerActionResult{
                CompletionOutcome::transient_failure,
                {},
                {},
                std::nullopt,
                std::nullopt,
                WorkerCompactionPreflightEvidence{
                    request.operation_id,
                    request.profile_id,
                    request.durable_profile->profile.image_uuid,
                    source_generation,
                    candidate_generation,
                    32768,
                    16384,
                    future_checkpoint_possible}});
        }
        CompactionMutationBoundaryEvidence proposed{
            request.operation_id,
            request.profile_id,
            request.durable_profile->profile.image_uuid,
            4242,
            9001,
            3,
            source_generation,
            candidate_generation,
            0,
            4096,
            8192,
            32768,
            1ULL << 30,
            request.operation_id.bytes()[0] == std::byte{0}
                ? 0U : 4907U};
        if (corrupt_compaction_boundary) {
            ++proposed.prior_generation;
        }
        if (before_compaction_boundary) before_compaction_boundary();
        auto decision = boundary(proposed);
        if (!decision.ok()) {
            return Result<WorkerActionResult>::failure(decision.error());
        }
        if (!decision.value().continue_compaction) {
            return Result<WorkerActionResult>::success({
                CompletionOutcome::transient_failure,
                {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
                 std::byte{'c'}, std::byte{'e'}, std::byte{'l'}}});
        }
        ++compaction_entries;
        if (after_compaction_boundary) after_compaction_boundary();
        if (fail_after_compaction_boundary) {
            return Result<WorkerActionResult>::failure(
                error(L"worker.compaction.after_boundary"));
        }
        proposed.cut_dirty_epoch = live_dirty_epoch;
        proposed.cut_dirty_epoch_known = true;
        if (corrupt_compaction_cut) {
            ++proposed.source_base_bytes;
        }
        ++compaction_cut_calls;
        auto cut_decision = cut(proposed);
        if (!cut_decision.ok()) {
            return Result<WorkerActionResult>::failure(
                cut_decision.error());
        }
        if (!cut_decision.value().continue_compaction) {
            return Result<WorkerActionResult>::success({
                CompletionOutcome::transient_failure,
                {std::byte{'c'}, std::byte{'u'}, std::byte{'t'}}});
        }
        last_compaction_correlation = proposed;
        return Result<WorkerActionResult>::success(WorkerActionResult{
            CompletionOutcome::succeeded_clean,
            {},
            {},
            std::nullopt,
            compaction_evidence(proposed, compaction_initial_disposition)});
    }

    Result<WorkerActionResult> reconcile_compaction(
        const WorkerActionRequest&,
        const CompactionMutationBoundaryEvidence& correlation) override {
        ++compaction_reconciliation_calls;
        if (fail_compaction_reconciliation) {
            return Result<WorkerActionResult>::failure(
                error(L"worker.compaction.reconcile"));
        }
        return Result<WorkerActionResult>::success(WorkerActionResult{
            CompletionOutcome::succeeded_clean,
            {},
            {},
            std::nullopt,
            compaction_evidence(
                correlation,
                compaction_resolved_disposition.value_or(
                    compaction_initial_disposition))});
    }

    Result<void> publish_compaction_epoch(
        const WorkerActionRequest&,
        const CompactionMutationBoundaryEvidence& correlation,
        const CompactionEpochPublicationHandler& publish) override {
        ++compaction_epoch_publications;
        REQUIRE(last_compaction_correlation.has_value());
        REQUIRE_EQ(correlation, last_compaction_correlation.value());
        if (before_compaction_epoch_publish) {
            before_compaction_epoch_publish();
        }
        return publish(CompactionEpochObservation{
            correlation, live_dirty_epoch});
    }

    Result<WorkerActionResult> reconcile_checkpoint(
        const WorkerActionRequest&,
        const CheckpointCommitBoundaryEvidence& correlation) override {
        ++reconciliation_calls;
        last_correlation = correlation;
        if (before_reconcile) before_reconcile();
        if (fail_reconciliation) {
            return Result<WorkerActionResult>::failure(
                error(L"worker.reconcile"));
        }
        const auto disposition = resolved_disposition.value_or(
            initial_disposition);
        auto checkpoint_evidence = evidence(correlation, disposition);
        if (resolved_generation.has_value()) {
            checkpoint_evidence.generation = resolved_generation.value();
            if (checkpoint_evidence.error_context.has_value()) {
                checkpoint_evidence.error_context->generation =
                    resolved_generation.value();
            }
        }
        return Result<WorkerActionResult>::success(WorkerActionResult{
            CompletionOutcome::succeeded_clean,
            {},
            {},
            std::move(checkpoint_evidence)});
    }

    Result<void> publish_checkpoint_epoch(
        const WorkerActionRequest&,
        const CheckpointCommitBoundaryEvidence& correlation,
        const CheckpointEpochPublicationHandler& publish) override {
        ++epoch_publications;
        REQUIRE(last_correlation.has_value());
        REQUIRE_EQ(correlation, last_correlation.value());
        // The commit landed but the epoch publication did not reach the handler.
        // That is not a failure of the commit and not a successful operation: it
        // is the one durable state a restart has to resolve by asking the bundle,
        // and modelling it is the only way to reach
        // `committed_pending_epoch_confirmation` across a restart.
        if (refuse_epoch_publications_remaining != 0) {
            --refuse_epoch_publications_remaining;
            return Result<void>::failure(error(L"publish_checkpoint_epoch"));
        }
        if (before_epoch_publish) before_epoch_publish();
        return publish(CheckpointEpochObservation{
            correlation, live_dirty_epoch});
    }

    Result<std::uint64_t> current_dirty_epoch(
        const product::Uuid&) override {
        return Result<std::uint64_t>::success(live_dirty_epoch);
    }

    Result<ForgetSafetyEvidence> prove_forget_safe(
        const WorkerActionRequest& request) override {
        ++forget_proof_calls;
        if (!request.durable_profile.has_value()) {
            return Result<ForgetSafetyEvidence>::failure(
                error(L"worker.forget.profile"));
        }
        // When set, the proof cannot be taken at all -- which is what the M5
        // worker control always answers, since it deliberately does not override
        // `prove_forget_safe` and keeps the inherited fail-closed default.  A
        // forget that cannot prove safety must retain both the registration and
        // the bundle, so the model needs a way to say "no proof".
        if (refuse_forget_proof) {
            return Result<ForgetSafetyEvidence>::failure(
                error(L"worker.forget.safety"));
        }
        return Result<ForgetSafetyEvidence>::success(
            forget_proof.value_or(ForgetSafetyEvidence{
                request.profile_id,
                request.durable_profile->profile.image_uuid,
                request.durable_profile->revision,
                true,
                true,
                true,
                true,
                test_bundle_file_identity()}));
    }

    bool refuse_forget_proof{};
    std::uint64_t live_dirty_epoch{11};
    std::uint64_t checkpoint_cut_dirty_epoch{11};
    std::uint32_t failure_code{15};
    std::uint32_t compaction_failure_code{15};
    CheckpointCommitDisposition initial_disposition{
        CheckpointCommitDisposition::committed};
    std::optional<CheckpointCommitDisposition> resolved_disposition;
    CompactionCommitDisposition compaction_initial_disposition{
        CompactionCommitDisposition::committed};
    std::optional<CompactionCommitDisposition>
        compaction_resolved_disposition;
    std::optional<std::uint64_t> resolved_generation;
    bool fail_reconciliation{};
    bool fail_before_boundary{};
    bool fail_after_boundary{};
    bool corrupt_checkpoint_evidence{};
    bool fail_after_compaction_boundary{};
    bool fail_compaction_reconciliation{};
    bool compaction_low_space{};
    bool future_checkpoint_possible{true};
    bool corrupt_compaction_boundary{};
    bool corrupt_compaction_cut{};
    bool zero_not_committed_candidate_metrics{};
    std::size_t boundary_calls{};
    std::size_t commit_entries{};
    std::size_t reconciliation_calls{};
    std::size_t epoch_publications{};
    std::size_t compaction_boundary_calls{};
    std::size_t compaction_entries{};
    std::size_t compaction_cut_calls{};
    std::size_t compaction_epoch_publications{};
    std::size_t compaction_reconciliation_calls{};
    std::vector<ActionKind> calls;
    std::optional<ActionKind> fail_action_once;
    std::optional<BundleRegistrationEvidence> rebind_evidence;
    std::optional<ForgetSafetyEvidence> forget_proof;
    std::size_t forget_proof_calls{};
    std::optional<CheckpointCommitBoundaryEvidence> last_correlation;
    std::optional<CompactionMutationBoundaryEvidence>
        last_compaction_correlation;
    std::function<void()> before_boundary;
    std::function<void()> after_boundary;
    std::function<void()> before_reconcile;
    std::function<void()> before_epoch_publish;
    // The epoch publication does not reach the handler this many times, so the
    // commit landed and its confirmation did not.  A count rather than a flag
    // because the state it produces -- durable commit, no confirmation -- is the
    // one a crash leaves behind, and a restart has to be able to finish it.
    std::size_t refuse_epoch_publications_remaining{};
    std::function<void()> before_compaction_boundary;
    std::function<void()> after_compaction_boundary;
    std::function<void()> before_compaction_epoch_publish;

    // Absent by default, so every M4 checkpoint test keeps the durable-only
    // read path it had before runtime consistency existed.
    Result<std::optional<WorkerRuntimeObservation>> observe_runtime(
        const product::Uuid& profile_id) override {
        observed_profiles.push_back(profile_id);
        if (observation_failure.has_value()) {
            return Result<std::optional<WorkerRuntimeObservation>>::failure(
                observation_failure.value());
        }
        return Result<std::optional<WorkerRuntimeObservation>>::success(
            observation);
    }
    // Strict by construction: no observation is a failure, not "clean".  A test
    // that drives a mutation decision off the runtime must set `observation`
    // first.
    Result<WorkerRuntimeObservation> require_runtime_observation(
        const product::Uuid& profile_id) override {
        observed_profiles.push_back(profile_id);
        if (observation_failure.has_value()) {
            return Result<WorkerRuntimeObservation>::failure(
                observation_failure.value());
        }
        if (!observation.has_value()) {
            return Result<WorkerRuntimeObservation>::failure(
                error(L"worker.require_runtime_observation.unbound"));
        }
        return Result<WorkerRuntimeObservation>::success(observation.value());
    }
    std::optional<WorkerRuntimeObservation> observation;
    std::optional<Error> observation_failure;
    std::vector<product::Uuid> observed_profiles;
};

class Logger final : public IServiceLogger {
public:
    void write(const ServiceLogEvent&) noexcept override {}
};

class Ids final : public IServiceIdGenerator {
public:
    Result<product::Uuid> next() override {
        if (values.empty()) {
            return Result<product::Uuid>::failure(error(L"ids.next"));
        }
        auto value = values.front();
        values.erase(values.begin());
        return Result<product::Uuid>::success(value);
    }
    std::vector<product::Uuid> values;
};

class Memory final : public IPhysicalMemoryQuery {
public:
    Result<PhysicalMemorySnapshot> query() override {
        return Result<PhysicalMemorySnapshot>::success(
            {16ULL << 30, 8ULL << 30});
    }
};

struct Fixture final {
    explicit Fixture(product::PersistenceState persistence)
        : profiles(running_profile(persistence)) {
        ids.values.push_back(
            uuid("44444444-4444-4444-8444-444444444444"));
    }

    explicit Fixture(Provisioning)
        : profiles(provisioning_profile()) {
        ids.values.push_back(
            uuid("44444444-4444-4444-8444-444444444444"));
    }

    ServiceCommandRequest start_request(std::string_view request_id) const {
        return {
            uuid(request_id),
            control::DiskStartCommand{L"work"},
            caller(profiles.snapshot().record.profile.owner_sid),
            true,
            false,
            std::nullopt,
            ClientWaitState::active};
    }

    ServiceCommandRequest request(std::string_view request_id) const {
        return {
            uuid(request_id),
            control::DiskCheckpointCommand{L"work"},
            caller(profiles.snapshot().record.profile.owner_sid),
            true,
            false,
            std::nullopt,
            ClientWaitState::active};
    }

    ServiceCommandRequest compact_request(std::string_view request_id) const {
        return {
            uuid(request_id),
            control::DiskCompactCommand{L"work"},
            caller(profiles.snapshot().record.profile.owner_sid),
            true,
            false,
            std::nullopt,
            ClientWaitState::active};
    }

    Clock clock;
    Profiles profiles;
    Operations operations;
    Worker worker;
    Logger logger;
    Ids ids;
    Memory memory;
};

} // namespace

#if !defined(MYRD_CHECKPOINT_FIXTURE_ONLY)

// A start planned from the provisioning lifecycle performs the profile's first
// checkpoint as one of its own actions, so its plan is a plan that checkpoints.
// Three other commands whose plans checkpoint -- `disk checkpoint` itself,
// `disk stop` with a checkpoint-required policy, and `disk compact` on a dirty
// disk -- each publish an admitted checkpoint state carrying `before_state`
// when they are admitted.  A provisioning start must do the same, because the
// commit boundary is the one place that state is read: the boundary proposes
// `commit_boundary`, and the durable-state validator refuses a
// `commit_boundary` record whose `before_state` is absent.  Without the
// admission the first checkpoint can never commit, and the operation stops at
// the boundary holding the profile's gate.
TEST_CASE(a_provisioning_start_admits_the_first_checkpoint_its_own_plan_performs) {
    Fixture fixture{Provisioning{}};
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.start_request(
        "55555555-5555-4555-8555-555555555560"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());

    const auto admitted = fixture.profiles.snapshot();
    REQUIRE_EQ(admitted.checkpoint.phase, CheckpointDurablePhase::admitted);
    REQUIRE(admitted.checkpoint.before_state.has_value());
    REQUIRE_EQ(
        admitted.checkpoint.before_state->lifecycle,
        profile::ProfileLifecycle::provisioning);
    // The admitted record is pre-boundary: it carries the decision, not an
    // outcome, and nothing may claim a cut that has not been taken.
    REQUIRE(!admitted.checkpoint.correlation.has_value());
    REQUIRE(!admitted.checkpoint.disposition.has_value());
    REQUIRE(!admitted.checkpoint.failure.has_value());
    REQUIRE(!admitted.checkpoint.reconciliation_failure.has_value());
    REQUIRE_EQ(
        admitted.operation_gate.active_operation_id,
        queued.value().operation_id);
}

TEST_CASE(checkpoint_commits_a_volume_the_durable_record_calls_clean) {
    Fixture fixture(product::PersistenceState::clean);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555551"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    // The durable record says `clean`, and it is not lying about what it knows
    // -- it just cannot know about a write that landed in the guest since the
    // last commit, because it only moves when an operation observes the live
    // dirty epoch.  A checkpoint that trusted it would be a silent no-op, which
    // is exactly what `disk checkpoint` must not be, so it takes the cut.
    REQUIRE_EQ(fixture.worker.calls.size(), 2U);
    REQUIRE_EQ(fixture.worker.calls[0], ActionKind::flush_exact_volume);
    REQUIRE_EQ(fixture.worker.calls[1], ActionKind::checkpoint);
    REQUIRE_EQ(fixture.worker.boundary_calls, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
}

TEST_CASE(checkpoint_dirty_profile_commits_through_boundary_and_epoch_fence) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555552"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.calls.size(), 2U);
    REQUIRE_EQ(fixture.worker.calls[0], ActionKind::flush_exact_volume);
    REQUIRE_EQ(fixture.worker.calls[1], ActionKind::checkpoint);
    REQUIRE_EQ(fixture.worker.boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.epoch_publications, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().compaction.checkpoints_since_compaction,
        1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.current_dirty_epoch, 11U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().checkpoint.phase,
        CheckpointDurablePhase::stable);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(checkpoint_after_a_worker_restart_commits_with_a_lower_epoch) {
    Fixture fixture(product::PersistenceState::clean);
    // The previous worker committed at epoch 11 and left that in the durable
    // record.  A new worker process counts mutations from zero, so its first cut
    // and its live observation are both 1 -- below the durable value.  The epoch
    // is a per-process counter, so that is not a regression: the commit must
    // proceed and re-baseline the durable epoch to the new session's value.
    fixture.worker.checkpoint_cut_dirty_epoch = 1;
    fixture.worker.live_dirty_epoch = 1;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555581"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.boundary_calls, 1U);
    REQUIRE_EQ(fixture.worker.epoch_publications, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    // Re-baselined to the new session's live epoch, not maxed with the stale 11.
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.current_dirty_epoch, 1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().checkpoint.phase,
        CheckpointDurablePhase::stable);
}

TEST_CASE(checkpoint_distinct_requests_coalesce_to_the_active_operation) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto first = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555553"));
    REQUIRE(first.ok());
    auto second = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555554"));

    REQUIRE(second.ok());
    REQUIRE_EQ(second.value().operation_id, first.value().operation_id);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(checkpoint_gate_cas_race_terminalizes_reservation_and_coalesces) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto competing_operation_id =
        uuid("99999999-9999-4999-8999-999999999999");
    fixture.profiles.before_checkpoint_publish = [&](Profiles& profiles) {
        REQUIRE_EQ(fixture.operations.records.size(), 1U);
        auto competing = fixture.operations.records.front();
        competing.request_id =
            uuid("99999999-9999-4999-8999-999999999998");
        competing.operation_id = competing_operation_id;
        competing.revision = 2;
        competing.state = OperationState::running;
        competing.phase = "admitting";
        fixture.operations.records.push_back(competing);

        auto snapshot = profiles.snapshot();
        const auto plan = plan_command(
            snapshot.state, ManagementCommand::checkpoint);
        const auto gate = plan_gate_access(
            snapshot.operation_gate, plan, competing_operation_id);
        REQUIRE(gate.accepted());
        snapshot.state = plan.admitted;
        snapshot.operation_gate = gate.after;
        snapshot.checkpoint.phase = CheckpointDurablePhase::admitted;
        snapshot.checkpoint.before_state = plan.before;
        profiles.replace(std::move(snapshot));
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto coalesced = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555550"));

    REQUIRE(coalesced.ok());
    REQUIRE_EQ(coalesced.value().operation_id, competing_operation_id);
    REQUIRE_EQ(fixture.operations.records.size(), 2U);
    const auto superseded = std::find_if(
        fixture.operations.records.begin(), fixture.operations.records.end(),
        [&](const auto& record) {
            return record.operation_id
                == uuid("44444444-4444-4444-8444-444444444444");
        });
    REQUIRE(superseded != fixture.operations.records.end());
    REQUIRE_EQ(superseded->state, OperationState::failed);
    REQUIRE(fixture.worker.calls.empty());
}

TEST_CASE(checkpoint_admission_commit_then_error_keeps_the_same_operation_owner) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.profiles.fail_after_checkpoint_phase_once =
        CheckpointDurablePhase::admitted;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555551"));

    REQUIRE(queued.ok());
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().operation_gate.active_operation_id,
        queued.value().operation_id);
    REQUIRE_EQ(
        fixture.operations.records.front().state,
        OperationState::running);
    auto completed = core.execute_operation(
        queued.value().operation_id.value());
    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE(!fixture.profiles.snapshot().operation_gate
                 .active_operation_id.has_value());
}

TEST_CASE(checkpoint_cancellation_wins_before_commit_boundary_without_m3) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    bool cancellation_accepted = false;
    fixture.worker.before_boundary = [&] {
        cancellation_accepted = fixture.operations.request_cancel(
            operation_id, 1001).ok();
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555555"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE(cancellation_accepted);
    REQUIRE_EQ(completed.value().state, OperationState::cancelled);
    REQUIRE(completed.value().terminal_result.size() >= 10U);
    REQUIRE_EQ(
        completed.value().terminal_result[9],
        static_cast<std::byte>(CheckpointCommitDisposition::not_committed));
    REQUIRE_EQ(fixture.worker.commit_entries, 0U);
    REQUIRE_EQ(fixture.worker.epoch_publications, 0U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
}

TEST_CASE(checkpoint_cancellation_after_commit_boundary_is_rejected) {
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

    auto queued = core.dispatch(fixture.request(
        "55555555-5555-4555-8555-555555555556"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE(cancellation_rejected);
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
}

TEST_CASE(checkpoint_contradictory_m3_evidence_remains_fail_closed) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.corrupt_checkpoint_evidence = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666675"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE(unresolved.value().terminal_result.empty());
    const auto& profile = fixture.profiles.snapshot();
    REQUIRE_EQ(
        profile.state.disk.persistence,
        product::PersistenceState::commit_indeterminate);
    REQUIRE_EQ(profile.state.disk.health, product::HealthState::needs_attention);
    REQUIRE(profile.operation_gate.active_operation_id.has_value());
    REQUIRE(!profile.checkpoint.disposition.has_value());
    REQUIRE(profile.checkpoint.reconciliation_failure.has_value());
}

TEST_CASE(checkpoint_worker_failure_after_p4_remains_fail_closed) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.fail_after_boundary = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666676"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE(unresolved.value().terminal_result.empty());
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
    REQUIRE(fixture.profiles.snapshot().checkpoint.correlation.has_value());
    REQUIRE(fixture.profiles.snapshot().checkpoint.reconciliation_failure
                .has_value());
}

TEST_CASE(checkpoint_not_committed_preserves_generation_and_dirty_state) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::not_committed;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666661"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.activity,
        product::ActivityState::idle);
    REQUIRE_EQ(checkpoint_terminal_generation(completed.value().terminal_result),
               5U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
    REQUIRE(fixture.profiles.snapshot().checkpoint.failure.has_value());
}

TEST_CASE(checkpoint_persistent_not_committed_becomes_persistence_blocked) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::not_committed;
    fixture.worker.failure_code = 13;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666662"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::persistence_blocked);
}

TEST_CASE(checkpoint_committed_needs_reconciliation_resolves_same_operation) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::committed_needs_reconciliation;
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::committed;
    bool resolving_observed = false;
    fixture.worker.before_reconcile = [&] {
        resolving_observed =
            fixture.profiles.snapshot().state.disk.activity
                == product::ActivityState::resolving_commit
            && fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value();
    };
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666663"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE(resolving_observed);
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
}

TEST_CASE(checkpoint_reconciliation_proves_prior_generation) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::committed_needs_reconciliation;
    fixture.worker.resolved_disposition =
        CheckpointCommitDisposition::not_committed;
    fixture.worker.resolved_generation = 5;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666674"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::failed);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(
        fixture.profiles.snapshot().checkpoint.failure->context.generation,
        5U);
    REQUIRE_EQ(checkpoint_terminal_generation(completed.value().terminal_result),
               5U);
    REQUIRE(!fixture.profiles.snapshot().operation_gate.active_operation_id
                 .has_value());
}

TEST_CASE(checkpoint_reconciliation_failure_preserves_m3_and_call_evidence) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::committed_needs_reconciliation;
    fixture.worker.fail_reconciliation = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666673"));
    REQUIRE(queued.ok());
    auto unresolved = core.execute_operation(*queued.value().operation_id);

    REQUIRE(unresolved.ok());
    REQUIRE_EQ(unresolved.value().state, OperationState::reconciling);
    REQUIRE(unresolved.value().terminal_result.empty());
    const auto& checkpoint = fixture.profiles.snapshot().checkpoint;
    REQUIRE(checkpoint.disposition.has_value());
    REQUIRE_EQ(
        checkpoint.disposition.value(),
        CheckpointCommitDisposition::committed_needs_reconciliation);
    REQUIRE(checkpoint.failure.has_value());
    REQUIRE_EQ(checkpoint.failure->domain, ErrorDomain::persistence);
    REQUIRE_EQ(checkpoint.failure->code, fixture.worker.failure_code);
    REQUIRE_EQ(checkpoint.failure->context.disposition,
               CheckpointCommitDisposition::committed_needs_reconciliation);
    REQUIRE(checkpoint.reconciliation_failure.has_value());
    REQUIRE_EQ(
        checkpoint.reconciliation_failure->domain, ErrorDomain::internal);
    REQUIRE_EQ(
        checkpoint.reconciliation_failure->code,
        static_cast<std::uint32_t>(ServiceCoreError::dependency_failure));
    REQUIRE_EQ(
        checkpoint.reconciliation_failure->operation,
        std::wstring(L"worker.reconcile"));
    REQUIRE_EQ(
        checkpoint.reconciliation_failure->unresolved_disposition,
        std::optional<CheckpointCommitDisposition>(
            CheckpointCommitDisposition::committed_needs_reconciliation));
    REQUIRE_EQ(
        checkpoint.reconciliation_failure->safe_context,
        std::string("worker checkpoint reconciliation produced no acceptable proof"));
}

TEST_CASE(checkpoint_unresolved_indeterminate_retains_gate_and_attention) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.initial_disposition =
        CheckpointCommitDisposition::commit_indeterminate;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "66666666-6666-4666-8666-666666666664"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(fixture.worker.reconciliation_calls, 1U);
    REQUIRE_EQ(completed.value().state, OperationState::reconciling);
    REQUIRE_EQ(completed.value().phase, "checkpoint-resolving");
    REQUIRE(completed.value().terminal_result.empty());
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.activity,
        product::ActivityState::resolving_commit);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::commit_indeterminate);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.health,
        product::HealthState::needs_attention);
    REQUIRE(fixture.profiles.snapshot().operation_gate.active_operation_id
                .has_value());
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 5U);

    auto mutation = core.dispatch(ServiceCommandRequest{
        uuid("66666666-6666-4666-8666-666666666665"),
        control::DiskStopCommand{L"work", false},
        caller(fixture.profiles.snapshot().record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(!mutation.ok());
    auto read = core.dispatch(ServiceCommandRequest{
        uuid("66666666-6666-4666-8666-666666666666"),
        control::DiskShowCommand{L"work"},
        caller(fixture.profiles.snapshot().record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});
    REQUIRE(read.ok());
    REQUIRE(read.value().profile.has_value());
    REQUIRE_EQ(
        read.value().profile->state.disk.persistence,
        product::PersistenceState::commit_indeterminate);
}

TEST_CASE(checkpoint_write_after_cut_remains_dirty_for_next_checkpoint) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.worker.live_dirty_epoch = 12;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "77777777-7777-4777-8777-777777777771"));
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(*queued.value().operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.confirmed_generation, 6U);
    REQUIRE_EQ(fixture.profiles.snapshot().checkpoint.current_dirty_epoch, 12U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::dirty);
}

TEST_CASE(checkpoint_public_read_overlays_a_later_authoritative_dirty_epoch) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = core.dispatch(fixture.request(
        "77777777-7777-4777-8777-777777777772"));
    REQUIRE(queued.ok());
    REQUIRE(core.execute_operation(*queued.value().operation_id).ok());
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.persistence,
        product::PersistenceState::clean);

    fixture.worker.live_dirty_epoch = 12;
    auto read = core.dispatch(ServiceCommandRequest{
        uuid("77777777-7777-4777-8777-777777777773"),
        control::DiskShowCommand{L"work"},
        caller(fixture.profiles.snapshot().record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        ClientWaitState::active});

    REQUIRE(read.ok());
    REQUIRE(read.value().profile.has_value());
    REQUIRE_EQ(
        read.value().profile->state.disk.persistence,
        product::PersistenceState::dirty);
    REQUIRE_EQ(read.value().profile->checkpoint.current_dirty_epoch, 12U);
}

TEST_CASE(checkpoint_precondition_revalidates_authoritative_dirty_epoch) {
    Fixture fixture(product::PersistenceState::clean);
    fixture.worker.live_dirty_epoch = 12;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    auto queued = core.dispatch(fixture.request(
        "77777777-7777-4777-8777-777777777774"));

    REQUIRE(queued.ok());
    REQUIRE_EQ(
        queued.value().disposition,
        ServiceResponseDisposition::operation_queued);
    REQUIRE_EQ(fixture.operations.records.size(), 1U);
    REQUIRE_EQ(
        fixture.profiles.snapshot().state.disk.activity,
        product::ActivityState::checkpointing);
    REQUIRE_EQ(
        fixture.profiles.snapshot().checkpoint.current_dirty_epoch, 12U);
}

TEST_CASE(checkpoint_restart_repairs_operation_first_commit_boundary_split) {
    Fixture fixture(product::PersistenceState::dirty);
    fixture.profiles.fail_before_checkpoint_phase_once =
        CheckpointDurablePhase::commit_boundary;
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.request(
            "77777777-7777-4777-8777-777777777775"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
        REQUIRE_EQ(fixture.worker.commit_entries, 0U);
        REQUIRE(!fixture.operations.load(operation_id).value().cancellable);
        REQUIRE_EQ(
            fixture.operations.load(operation_id).value().phase,
            std::string("checkpoint-commit-boundary"));
        REQUIRE(!fixture.profiles.snapshot().checkpoint.correlation.has_value());
        REQUIRE(fixture.profiles.snapshot().operation_gate.cancellable);
    }

    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto completed = restarted.execute_operation(operation_id);

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.worker.commit_entries, 1U);
    REQUIRE_EQ(fixture.worker.boundary_calls, 2U);
}

TEST_CASE(checkpoint_restart_rejects_unmarked_noncancellable_preboundary_record) {
    Fixture fixture(product::PersistenceState::dirty);
    ServiceCore first(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    auto queued = first.dispatch(fixture.request(
        "77777777-7777-4777-8777-777777777776"));
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

TEST_CASE(checkpoint_restart_rejects_unknown_stable_disposition) {
    Fixture fixture(product::PersistenceState::dirty);
    const auto operation_id =
        uuid("44444444-4444-4444-8444-444444444444");
    fixture.worker.before_epoch_publish = [&] {
        fixture.operations.failed_write_call =
            fixture.operations.write_calls + 1;
    };
    {
        ServiceCore first(
            fixture.clock, fixture.profiles, fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        auto queued = first.dispatch(fixture.request(
            "77777777-7777-4777-8777-777777777777"));
        REQUIRE(queued.ok());
        REQUIRE(!first.execute_operation(operation_id).ok());
    }
    auto corrupted = fixture.profiles.snapshot();
    REQUIRE_EQ(
        corrupted.checkpoint.phase,
        CheckpointDurablePhase::stable);
    corrupted.checkpoint.disposition =
        static_cast<CheckpointCommitDisposition>(0xff);
    fixture.profiles.replace(std::move(corrupted));

    const auto worker_entries = fixture.worker.commit_entries;
    ServiceCore restarted(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    REQUIRE(restarted.recover_after_restart().ok());
    auto rejected = restarted.execute_operation(operation_id);

    REQUIRE(!rejected.ok());
    REQUIRE_EQ(fixture.worker.commit_entries, worker_entries);
}

#endif
