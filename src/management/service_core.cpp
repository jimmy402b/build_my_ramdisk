#include "src/management/service_core.h"

#include "src/common/sha256.h"
#include "src/control/ipc_codec.h"
#include "src/profile/profile_validation.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace myrd::management {

ServiceCore::ServiceCore(
    IServiceClock& clock,
    IServiceProfileStore& profiles,
    IServiceOperationStore& operations,
    IWorkerControl& worker,
    IServiceLogger& logger,
    IServiceIdGenerator& ids,
    IPhysicalMemoryQuery& memory,
    IServiceRegistrationStore* registrations) noexcept
    : clock_(clock),
      profiles_(profiles),
      operations_(operations),
      worker_(worker),
      logger_(logger),
      ids_(ids),
      memory_(memory),
      registrations_(registrations) {}

namespace {

Error service_error(
    ServiceCoreError code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Error{
        ErrorDomain::internal,
        static_cast<std::uint32_t>(code),
        operation,
        message};
}

bool is_terminal(OperationState state) noexcept {
    return state == OperationState::succeeded
        || state == OperationState::failed
        || state == OperationState::cancelled;
}

// A plan performs a checkpoint when its action sequence contains one.  This is
// asked of the plan rather than restated per command, so a command that starts
// checkpointing cannot be missed at the admission point that has to prepare the
// durable checkpoint state for it.
bool plan_performs_checkpoint(const ActionPlan& plan) noexcept {
    const auto actions = plan.action_span();
    return std::find(actions.begin(), actions.end(), ActionKind::checkpoint)
        != actions.end();
}

// Whether `plan.before` can be the checkpoint's durable `before_state`.  The
// durable-state validator requires a `before_state`'s lifecycle to match the
// state it belongs to, so a plan planned from a profile that does not exist yet
// -- `disk create` without `--no-start` plans from the absent state -- cannot
// use its `before` for this, and publishing one anyway would be refused rather
// than recorded.
bool plan_before_can_be_checkpoint_before_state(const ActionPlan& plan) noexcept {
    return is_valid(plan.before)
        && plan.before.lifecycle.has_value()
        && plan.before.lifecycle == plan.admitted.lifecycle;
}

bool is_profile_not_found(const Error& error) noexcept {
    return error.domain == ErrorDomain::internal
        && error.code
            == static_cast<std::uint32_t>(
                ServiceCoreError::profile_not_found);
}

bool is_zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

bool is_successful_completion(CompletionOutcome outcome) noexcept {
    return outcome == CompletionOutcome::succeeded_clean
        || outcome == CompletionOutcome::succeeded_dirty
        || outcome == CompletionOutcome::compaction_deferred;
}

std::vector<std::byte> nonempty_terminal_result(
    std::vector<std::byte> result,
    bool succeeded) {
    if (!result.empty()) return result;
    if (succeeded) {
        return {std::byte{'o'}, std::byte{'k'}};
    }
    return {
        std::byte{'f'}, std::byte{'a'}, std::byte{'i'}, std::byte{'l'}};
}

std::vector<control::IdempotentWarning> warnings_for(
    TransitionDisposition disposition) {
    switch (disposition) {
    case TransitionDisposition::already_running:
        return {control::IdempotentWarning::already_running};
    case TransitionDisposition::already_stopped:
        return {control::IdempotentWarning::already_stopped};
    case TransitionDisposition::no_changes:
        return {control::IdempotentWarning::no_changes};
    default:
        return {};
    }
}

std::vector<std::byte> durable_noop_result(
    TransitionDisposition disposition) {
    const auto warning = warnings_for(disposition);
    if (warning.empty()) return {};
    return {
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'N'}, std::byte{'O'}, std::byte{'O'}, std::byte{'P'},
        static_cast<std::byte>(warning.front())};
}

std::vector<control::IdempotentWarning> warnings_from(
    const OperationRecord& record) {
    constexpr std::array<std::byte, 8> prefix{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'N'}, std::byte{'O'}, std::byte{'O'}, std::byte{'P'}};
    if (record.terminal_result.size() != prefix.size() + 1
        || !std::equal(prefix.begin(), prefix.end(),
                       record.terminal_result.begin())) {
        return {};
    }
    const auto raw = std::to_integer<std::uint8_t>(
        record.terminal_result.back());
    if (raw > static_cast<std::uint8_t>(
                  control::IdempotentWarning::no_changes)) {
        return {};
    }
    return {static_cast<control::IdempotentWarning>(raw)};
}

ServiceResponseDisposition queued_response_disposition(
    const ServiceCommandRequest& request) noexcept {
    if (request.client_wait == ClientWaitState::cancelled) {
        return ServiceResponseDisposition::client_wait_cancelled;
    }
    if (!request.no_wait && request.wait_timeout_ms == 0) {
        return ServiceResponseDisposition::wait_timeout;
    }
    return ServiceResponseDisposition::operation_queued;
}

std::uint64_t align_down_memory(std::uint64_t value) noexcept {
    return value - (value % profile::kProfileMemoryAlignmentBytes);
}

Result<product::Uuid> derive_request_uuid(
    const product::Uuid& request_id,
    std::string_view purpose) {
    std::string material = "myrd.m4.registration.v1:";
    material.append(purpose);
    material.push_back(':');
    material.append(request_id.to_string());
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(material.data()),
        material.size());
    auto digest = sha256_digest(bytes);
    if (!digest.ok()) {
        return Result<product::Uuid>::failure(digest.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string canonical;
    canonical.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            canonical.push_back('-');
        }
        const auto value = std::to_integer<unsigned>(digest.value()[index]);
        canonical.push_back(kHex[(value >> 4U) & 0x0FU]);
        canonical.push_back(kHex[value & 0x0FU]);
    }
    canonical[14] = '4';
    const auto variant = static_cast<unsigned>(canonical[19] <= '9'
        ? canonical[19] - '0' : canonical[19] - 'a' + 10);
    canonical[19] = kHex[8U | (variant & 0x03U)];
    return product::Uuid::parse(canonical);
}

bool nonzero_file_identity(const BundleFileIdentity& identity) noexcept {
    return identity.volume_serial != 0
        && std::any_of(
            identity.file_id.begin(), identity.file_id.end(),
            [](std::byte value) { return value != std::byte{0}; });
}

bool checkpoint_interval_equal(
    const std::optional<product::CheckpointInterval>& left,
    const std::optional<product::CheckpointInterval>& right) noexcept {
    if (left.has_value() != right.has_value()) return false;
    return !left.has_value()
        || (left->enabled == right->enabled
            && left->duration == right->duration);
}

bool optional_size_equal(
    const std::optional<control::OptionalSize>& left,
    const std::optional<control::OptionalSize>& right) noexcept {
    if (left.has_value() != right.has_value()) return false;
    return !left.has_value()
        || (left->enabled == right->enabled && left->bytes == right->bytes);
}

bool lifecycle_intent_equal(
    const LifecycleOperationIntent& left,
    const LifecycleOperationIntent& right) noexcept {
    return left.kind == right.kind
        && left.profile_id == right.profile_id
        && left.expected_image_id == right.expected_image_id
        && left.owner_sid == right.owner_sid
        && left.display_name == right.display_name
        && left.bundle_path == right.bundle_path
        && left.mount_target == right.mount_target
        && left.requested_capacity_bytes == right.requested_capacity_bytes
        && left.auto_start == right.auto_start
        && left.memory_mode == right.memory_mode
        && left.memory_hard_limit_bytes == right.memory_hard_limit_bytes
        && checkpoint_interval_equal(
            left.checkpoint_interval, right.checkpoint_interval)
        && optional_size_equal(left.dirty_threshold, right.dirty_threshold)
        && left.physical_memory.total_bytes
            == right.physical_memory.total_bytes
        && left.physical_memory.available_bytes
            == right.physical_memory.available_bytes
        && left.created_unix_ms == right.created_unix_ms;
}

bool reservation_equal(
    const LifecycleRegistrationReservation& left,
    const LifecycleRegistrationReservation& right) noexcept {
    return left.revision == right.revision
        && left.operation_id == right.operation_id
        && left.request_id == right.request_id
        && left.argument_digest == right.argument_digest
        && lifecycle_intent_equal(left.intent, right.intent)
        && left.phase == right.phase
        && left.bundle == right.bundle
        && left.volume == right.volume;
}

bool reservation_matches_operation(
    const LifecycleRegistrationReservation& reservation,
    const OperationRecord& operation) noexcept {
    const bool command_matches =
        (operation.command_scope == "disk.create"
         && reservation.intent.kind == LifecycleOperationKind::create)
        || (operation.command_scope == "disk.import"
            && reservation.intent.kind == LifecycleOperationKind::import);
    return reservation.revision != 0
        && reservation.operation_id == operation.operation_id
        && reservation.request_id == operation.request_id
        && reservation.argument_digest == operation.argument_digest
        && reservation.intent.profile_id == operation.profile_id
        && reservation.intent.created_unix_ms == operation.created_unix_ms
        && command_matches;
}

Result<ActionPlan> plan_lifecycle_reservation(
    const LifecycleRegistrationReservation& reservation) {
    const auto command = reservation.intent.kind
            == LifecycleOperationKind::import
        ? ManagementCommand::import_profile
        : (reservation.intent.auto_start
               ? ManagementCommand::create_and_start
               : ManagementCommand::create_stopped);
    auto plan = plan_command(absent_profile_state(), command);
    if (!plan.accepted()
        || plan.disposition != TransitionDisposition::allowed) {
        return Result<ActionPlan>::failure(service_error(
            ServiceCoreError::state_rejected,
            L"service_core.lifecycle_resume_plan",
            L"The durable lifecycle reservation cannot be planned"));
    }
    return Result<ActionPlan>::success(std::move(plan));
}

bool same_bundle_binding(
    const BundleRegistrationEvidence& left,
    const BundleRegistrationEvidence& right) noexcept {
    return left.image_id == right.image_id
        && left.manifest_name == right.manifest_name
        && left.logical_capacity_bytes == right.logical_capacity_bytes
        && left.logical_sector_bytes == right.logical_sector_bytes
        && left.physical_sector_bytes == right.physical_sector_bytes
        && left.payload_block_bytes == right.payload_block_bytes
        && left.file_identity == right.file_identity;
}

const char* lifecycle_action_phase(ActionKind action) noexcept {
    switch (action) {
    case ActionKind::create_and_verify_bundle: return "create-bundle";
    case ActionKind::inspect_bundle_read_only: return "inspect-bundle";
    case ActionKind::publish_provisioning_profile:
        return "publish-provisioning";
    case ActionKind::publish_imported_ready_profile:
        return "publish-imported-ready";
    case ActionKind::create_runtime_and_worker: return "create-runtime";
    case ActionKind::recover_bundle: return "recover-bundle";
    case ActionKind::start_proxy: return "start-proxy";
    case ActionKind::create_exact_device: return "create-device";
    case ActionKind::locate_exact_physical_drive:
        return "locate-physical-drive";
    case ActionKind::prove_new_uninitialized: return "prove-uninitialized";
    case ActionKind::provision_gpt_ntfs: return "provision-gpt-ntfs";
    case ActionKind::record_volume_identity: return "record-volume";
    case ActionKind::flush_exact_volume: return "flush-volume";
    case ActionKind::checkpoint: return "checkpoint";
    case ActionKind::publish_ready_profile: return "publish-ready";
    case ActionKind::verify_existing_volume_identity:
        return "verify-volume";
    case ActionKind::assign_exact_mount: return "assign-mount";
    case ActionKind::readiness_io: return "readiness-io";
    default: return "lifecycle-action";
    }
}

enum class RegistrationApplicationDisposition : std::uint8_t {
    applied = 0,
    not_committed,
    ambiguous,
};

struct RegistrationApplication final {
    RegistrationApplicationDisposition disposition{
        RegistrationApplicationDisposition::ambiguous};
    std::optional<ManagedProfileSnapshot> snapshot;
};

Result<LifecycleRegistrationReservation> persist_reservation_transition(
    IServiceRegistrationStore& registrations,
    const LifecycleRegistrationReservation& expected,
    LifecycleRegistrationReservation proposed) {
    ++proposed.revision;
    auto updated = registrations.update_reservation(expected, proposed);
    if (updated.ok() && reservation_equal(updated.value(), proposed)) {
        return Result<LifecycleRegistrationReservation>::success(
            updated.move_value());
    }
    auto resolved = registrations.load_reservation(expected.operation_id);
    if (resolved.ok() && reservation_equal(resolved.value(), proposed)) {
        return Result<LifecycleRegistrationReservation>::success(
            resolved.move_value());
    }
    return Result<LifecycleRegistrationReservation>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.persist_lifecycle_evidence",
        L"The lifecycle evidence transition is not durably exact"));
}

Result<RegistrationApplication> apply_registration_mutation(
    IServiceRegistrationStore& registrations,
    const product::Uuid& operation_id,
    const ManagedProfileSnapshot& intended,
    RegistrationMutationResult mutation) {
    if (mutation.disposition == RegistrationCommitDisposition::committed) {
        if (!mutation.snapshot.has_value()
            || mutation.snapshot.value() != intended) {
            return Result<RegistrationApplication>::success({
                RegistrationApplicationDisposition::ambiguous,
                std::nullopt});
        }
        return Result<RegistrationApplication>::success({
            RegistrationApplicationDisposition::applied,
            mutation.snapshot});
    }
    if (mutation.disposition
            == RegistrationCommitDisposition::not_committed) {
        if (mutation.snapshot.has_value()) {
            return Result<RegistrationApplication>::success({
                RegistrationApplicationDisposition::ambiguous,
                std::nullopt});
        }
        return Result<RegistrationApplication>::success({
            RegistrationApplicationDisposition::not_committed,
            std::nullopt});
    }

    auto resolution = registrations.resolve_registration(
        operation_id, intended.record.profile.profile_uuid);
    if (!resolution.ok()) {
        return Result<RegistrationApplication>::failure(
            resolution.error());
    }
    if (resolution.value().disposition
            == RegistrationResolutionDisposition::absent
        && !resolution.value().snapshot.has_value()) {
        return Result<RegistrationApplication>::success({
            RegistrationApplicationDisposition::not_committed,
            std::nullopt});
    }
    if (resolution.value().disposition
            == RegistrationResolutionDisposition::exact
        && resolution.value().snapshot.has_value()
        && resolution.value().snapshot.value() == intended) {
        return Result<RegistrationApplication>::success({
            RegistrationApplicationDisposition::applied,
            resolution.value().snapshot});
    }
    return Result<RegistrationApplication>::success({
        RegistrationApplicationDisposition::ambiguous,
        std::nullopt});
}

bool valid_bundle_evidence(
    const LifecycleOperationIntent& intent,
    const BundleRegistrationEvidence& evidence) noexcept {
    const auto expected_source = intent.kind == LifecycleOperationKind::create
        ? BundleEvidenceSource::created_and_verified
        : BundleEvidenceSource::read_only_inspection;
    return evidence.source == expected_source
        && (!intent.expected_image_id.has_value()
            || evidence.image_id == intent.expected_image_id.value())
        && evidence.logical_capacity_bytes != 0
        && (intent.requested_capacity_bytes == 0
            || evidence.logical_capacity_bytes
                == intent.requested_capacity_bytes)
        && evidence.logical_sector_bytes == kProductLogicalSectorBytes
        && evidence.physical_sector_bytes == kProductPhysicalSectorBytes
        && evidence.payload_block_bytes == kProductPayloadBlockBytes
        && !evidence.manifest_name.empty()
        && nonzero_file_identity(evidence.file_identity);
}

bool valid_rebind_bundle_evidence(
    const RecoveryOperationIntent& intent,
    const ManagedProfileSnapshot& expected,
    const BundleRegistrationEvidence& evidence) noexcept {
    return intent.kind == RecoveryOperationKind::rebind
        && intent.profile_id == expected.record.profile.profile_uuid
        && intent.expected_image_id.has_value()
        && intent.expected_image_id.value()
            == expected.record.profile.image_uuid
        && evidence.source == BundleEvidenceSource::read_only_inspection
        && evidence.image_id == intent.expected_image_id.value()
        && evidence.logical_capacity_bytes
            == expected.record.profile.logical_capacity_bytes
        && evidence.logical_sector_bytes
            == expected.record.profile.logical_sector_size
        && evidence.physical_sector_bytes
            == expected.record.profile.physical_sector_size
        && evidence.payload_block_bytes == kProductPayloadBlockBytes
        && !intent.bundle_path.empty()
        && !evidence.manifest_name.empty()
        && nonzero_file_identity(evidence.file_identity);
}

bool forget_reservation_matches(
    const ForgetTombstone& proposed,
    const ForgetTombstone& durable) noexcept {
    if (proposed.retained_bundle_identity.has_value()
        || !durable.retained_bundle_identity.has_value()
        || !nonzero_file_identity(
            durable.retained_bundle_identity.value())
        || durable.authorization_owner_sid.empty()) {
        return false;
    }
    auto expected = proposed;
    expected.retained_bundle_identity = durable.retained_bundle_identity;
    return expected == durable;
}

bool forget_tombstone_matches_operation(
    const ForgetTombstone& tombstone,
    const OperationRecord& operation) noexcept {
    return tombstone.operation_id == operation.operation_id
        && tombstone.request_id == operation.request_id
        && tombstone.profile_id == operation.profile_id
        && !tombstone.authorization_owner_sid.empty()
        && tombstone.retained_bundle_identity.has_value()
        && nonzero_file_identity(
            tombstone.retained_bundle_identity.value());
}

bool is_unowned_admission_terminal(
    const OperationRecord& operation) noexcept {
    constexpr std::array<std::byte, 12> kResult{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'O'}, std::byte{'R'}, std::byte{'P'}, std::byte{'H'},
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    return operation.state == OperationState::failed
        && operation.phase == "terminal"
        && operation.terminal_result.size() == kResult.size()
        && std::equal(
            kResult.begin(), kResult.end(),
            operation.terminal_result.begin());
}

std::size_t recorded_volume_identity_count(
    const profile::Profile& value) noexcept {
    return static_cast<std::size_t>(value.gpt_disk_guid.has_value())
        + static_cast<std::size_t>(value.partition_guid.has_value())
        + static_cast<std::size_t>(value.volume_guid.has_value());
}

profile::MountKind mount_kind(std::wstring_view target) noexcept {
    return target.size() == 2 && target[1] == L':'
        ? profile::MountKind::drive_letter
        : profile::MountKind::directory;
}

Result<profile::ProfileRecord> make_registration_record(
    const LifecycleOperationIntent& intent,
    const BundleRegistrationEvidence& evidence,
    const std::optional<VolumeRegistrationEvidence>& volume,
    profile::ProfileLifecycle lifecycle) {
    if (!valid_bundle_evidence(intent, evidence)) {
        return Result<profile::ProfileRecord>::failure(service_error(
            ServiceCoreError::dependency_failure,
            L"service_core.bundle_evidence",
            L"The worker returned incomplete or mismatched bundle evidence"));
    }
    auto memory = materialize_memory_policy(
        intent.memory_mode,
        intent.memory_hard_limit_bytes,
        evidence.logical_capacity_bytes,
        intent.physical_memory);
    if (!memory.ok()) {
        return Result<profile::ProfileRecord>::failure(memory.error());
    }
    profile::CheckpointPolicy checkpoint{};
    if (intent.checkpoint_interval.has_value()) {
        checkpoint.interval_enabled = intent.checkpoint_interval->enabled;
        checkpoint.interval_seconds = static_cast<std::uint64_t>(
            intent.checkpoint_interval->duration.count());
    }
    if (intent.dirty_threshold.has_value()) {
        checkpoint.dirty_threshold_enabled = intent.dirty_threshold->enabled;
        checkpoint.dirty_threshold_bytes = intent.dirty_threshold->bytes;
    }
    profile::ProfileRecord record{
        1,
        profile::Profile{
            intent.profile_id,
            intent.owner_sid,
            intent.display_name,
            intent.created_unix_ms,
            lifecycle,
            intent.bundle_path,
            evidence.manifest_name,
            evidence.image_id,
            evidence.logical_capacity_bytes,
            evidence.logical_sector_bytes,
            evidence.physical_sector_bytes,
            volume.has_value()
                ? std::optional<product::Uuid>(volume->gpt_disk_id)
                : std::nullopt,
            volume.has_value()
                ? std::optional<product::Uuid>(volume->partition_id)
                : std::nullopt,
            volume.has_value()
                ? std::optional<product::Uuid>(volume->volume_id)
                : std::nullopt,
            mount_kind(intent.mount_target),
            intent.mount_target,
            intent.auto_start,
            memory.move_value(),
            checkpoint,
            profile::CompactionPolicy{}}};
    const auto validation = profile::validate_profile(record);
    if (!validation.ok()) {
        return Result<profile::ProfileRecord>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.registration_profile",
            L"The materialized profile violates the durable profile schema"));
    }
    return Result<profile::ProfileRecord>::success(std::move(record));
}

bool valid_checkpoint_boundary(
    const CheckpointCommitBoundaryEvidence& evidence,
    const WorkerActionRequest& request,
    const ManagedProfileSnapshot& profile) noexcept {
    return !is_zero_uuid(evidence.product_operation_id)
        && evidence.product_operation_id == request.operation_id
        && evidence.profile_id == request.profile_id
        && evidence.profile_id == profile.record.profile.profile_uuid
        && evidence.image_id == profile.record.profile.image_uuid
        && evidence.worker_pid != 0
        && evidence.worker_process_creation_time != 0
        && evidence.worker_session_generation != 0
        && evidence.prior_generation
            == profile.checkpoint.confirmed_generation
        && evidence.prior_generation
            < (std::numeric_limits<std::uint64_t>::max)()
        && evidence.candidate_generation == evidence.prior_generation + 1;
    // The cut epoch is deliberately NOT compared against the durable
    // `current_dirty_epoch` here.  Both are per-process counters, but they came
    // from different worker processes when the runtime was restarted: the new
    // worker counts from zero, so its cut epoch can be below the value the
    // previous worker left in the durable record.  That is a re-baseline, not a
    // regression -- the epoch only ever rises within one session, and the worker
    // identity above (pid, creation time, session generation) plus the
    // generation equality are the real cross-session safety conditions.
}

std::wstring widen_ascii(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

bool known_checkpoint_disposition(
    const CheckpointCommitDisposition disposition) noexcept {
    switch (disposition) {
        case CheckpointCommitDisposition::not_committed:
        case CheckpointCommitDisposition::commit_indeterminate:
        case CheckpointCommitDisposition::committed:
        case CheckpointCommitDisposition::committed_needs_reconciliation:
            return true;
        default:
            return false;
    }
}

bool known_normal_stop_policy(
    const profile::NormalStopPolicy policy) noexcept {
    return policy == profile::NormalStopPolicy::checkpoint_required
        || policy == profile::NormalStopPolicy::require_clean;
}

bool valid_checkpoint_evidence(
    const WorkerCheckpointEvidence& evidence,
    const CheckpointCommitBoundaryEvidence& expected,
    const bool reconciliation = false) {
    if (!known_checkpoint_disposition(evidence.disposition)) return false;
    const bool generation_matches =
        evidence.generation == expected.candidate_generation
        || (reconciliation
            && evidence.disposition
                == CheckpointCommitDisposition::not_committed
            && evidence.generation == expected.prior_generation);
    if (evidence.correlation != expected
        || !generation_matches
        || evidence.observed_dirty_epoch < expected.cut_dirty_epoch
        || evidence.error.has_value() != evidence.error_context.has_value()) {
        return false;
    }
    if (evidence.disposition == CheckpointCommitDisposition::committed) {
        return !evidence.error.has_value();
    }
    if (!evidence.error.has_value()) return false;
    const auto& failure = evidence.error.value();
    const auto& context = evidence.error_context.value();
    return failure.domain == ErrorDomain::persistence
        && failure.code == context.code
        && failure.native_code == context.native_code
        && failure.operation == widen_ascii(context.operation)
        && !context.operation.empty()
        && context.operation.size() <= 255
        && !context.safe_context.empty()
        && context.safe_context.size() <= 255
        && context.native_operation_id != 0
        && std::equal(
            context.image_uuid.begin(), context.image_uuid.end(),
            expected.image_id.bytes().begin())
        && context.generation == evidence.generation
        && context.disposition == evidence.disposition;
}

bool valid_compaction_boundary(
    const CompactionMutationBoundaryEvidence& evidence,
    const WorkerActionRequest& request,
    const ManagedProfileSnapshot& profile) noexcept {
    return !is_zero_uuid(evidence.product_operation_id)
        && evidence.product_operation_id == request.operation_id
        && evidence.profile_id == request.profile_id
        && evidence.profile_id == profile.record.profile.profile_uuid
        && evidence.image_id == profile.record.profile.image_uuid
        && evidence.worker_pid != 0
        && evidence.worker_process_creation_time != 0
        && evidence.worker_session_generation != 0
        && evidence.native_m3_operation_id.has_value()
        && evidence.native_m3_operation_id.value() != 0
        && evidence.prior_generation
            == profile.checkpoint.confirmed_generation
        && evidence.prior_generation
            < (std::numeric_limits<std::uint64_t>::max)()
        && evidence.candidate_generation == evidence.prior_generation + 1
        && !evidence.cut_dirty_epoch_known
        && evidence.cut_dirty_epoch == 0
        && evidence.source_base_bytes != 0
        && evidence.source_journal_bytes != 0
        && evidence.required_peak_free_bytes != 0
        && evidence.available_free_bytes
            >= evidence.required_peak_free_bytes;
}

bool known_compaction_disposition(
    const CompactionCommitDisposition disposition) noexcept {
    switch (disposition) {
        case CompactionCommitDisposition::not_committed:
        case CompactionCommitDisposition::commit_indeterminate:
        case CompactionCommitDisposition::committed:
        case CompactionCommitDisposition::committed_needs_reconciliation:
            return true;
        default:
            return false;
    }
}

bool valid_compaction_evidence(
    const WorkerCompactionEvidence& evidence,
    const CompactionMutationBoundaryEvidence& expected) noexcept {
    if (!known_compaction_disposition(evidence.disposition)) return false;
    const auto& metrics = evidence.metrics;
    const bool generation_matches =
        evidence.generation == expected.candidate_generation
        || (evidence.disposition
                == CompactionCommitDisposition::not_committed
            && evidence.generation == expected.prior_generation);
    if (evidence.correlation != expected
        || !generation_matches
        || evidence.observed_dirty_epoch < expected.cut_dirty_epoch
        || metrics.source_generation != expected.prior_generation
        || metrics.source_base_bytes != expected.source_base_bytes
        || metrics.source_journal_bytes != expected.source_journal_bytes
        || metrics.required_peak_free_bytes
            != expected.required_peak_free_bytes
        || metrics.available_free_bytes != expected.available_free_bytes
        || evidence.error.has_value()
            != evidence.error_context.has_value()) {
        return false;
    }
    const bool candidate_must_exist = evidence.disposition
        != CompactionCommitDisposition::not_committed;
    if ((candidate_must_exist && !expected.cut_dirty_epoch_known)
        || (candidate_must_exist
            && (metrics.candidate_base_bytes == 0
                || metrics.candidate_journal_bytes == 0))
        || (evidence.disposition == CompactionCommitDisposition::committed
            && metrics.committed_generation
                != expected.candidate_generation)
        || (evidence.disposition != CompactionCommitDisposition::committed
            && metrics.committed_generation != 0
            && metrics.committed_generation
                != expected.candidate_generation)) {
        return false;
    }
    if (evidence.disposition == CompactionCommitDisposition::committed) {
        return !evidence.error.has_value();
    }
    if (!evidence.error.has_value()) return false;
    const auto& failure = evidence.error.value();
    const auto& context = evidence.error_context.value();
    return failure.domain == ErrorDomain::persistence
        && failure.code == context.code
        && failure.native_code == context.native_code
        && failure.operation == widen_ascii(context.operation)
        && !context.operation.empty()
        && context.operation.size() <= 255
        && !context.safe_context.empty()
        && context.safe_context.size() <= 255
        && context.native_operation_id
            == expected.native_m3_operation_id.value()
        && std::equal(
            context.image_uuid.begin(), context.image_uuid.end(),
            expected.image_id.bytes().begin())
        && context.generation == evidence.generation
        && context.disposition == evidence.disposition;
}

bool valid_compaction_preflight_failure(
    const WorkerCompactionPreflightEvidence& evidence,
    const WorkerActionRequest& request,
    const ManagedProfileSnapshot& profile) noexcept {
    return evidence.product_operation_id == request.operation_id
        && evidence.profile_id == request.profile_id
        && evidence.profile_id == profile.record.profile.profile_uuid
        && evidence.image_id == profile.record.profile.image_uuid
        && evidence.source_generation
            == profile.checkpoint.confirmed_generation
        && evidence.source_generation
            < (std::numeric_limits<std::uint64_t>::max)()
        && evidence.candidate_generation == evidence.source_generation + 1
        && evidence.required_peak_free_bytes != 0
        && evidence.available_free_bytes
            < evidence.required_peak_free_bytes;
}

CompactionFailureEvidence durable_compaction_failure(
    const WorkerCompactionEvidence& evidence) {
    const auto& failure = evidence.error.value();
    return {
        failure.domain,
        failure.code,
        failure.operation,
        failure.message,
        failure.native_code,
        evidence.error_context.value()};
}

CompactionReconciliationFailureEvidence
durable_compaction_reconciliation_failure(
    const Error& error,
    const std::optional<CompactionCommitDisposition> unresolved_disposition,
    std::string safe_context) {
    return {
        error.domain,
        error.code,
        error.operation,
        error.message,
        error.native_code,
        unresolved_disposition,
        std::move(safe_context)};
}

CheckpointFailureEvidence durable_checkpoint_failure(
    const WorkerCheckpointEvidence& evidence) {
    const auto& failure = evidence.error.value();
    return {
        failure.domain,
        failure.code,
        failure.operation,
        failure.message,
        failure.native_code,
        evidence.error_context.value()};
}

CheckpointReconciliationFailureEvidence
durable_checkpoint_reconciliation_failure(
    const Error& error,
    const std::optional<CheckpointCommitDisposition> unresolved_disposition,
    std::string safe_context) {
    return {
        error.domain,
        error.code,
        error.operation,
        error.message,
        error.native_code,
        unresolved_disposition,
        std::move(safe_context)};
}

bool is_persistent_checkpoint_failure(std::uint32_t code) noexcept {
    // M3 wire values: DiskFull=13, AccessDenied=14. Both require an
    // explicit manual retry/configuration change rather than a retry storm.
    return code == 13U || code == 14U;
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_bounded_string(
    std::vector<std::byte>& output, std::string_view value) {
    const auto length = (std::min)(value.size(), std::size_t{255});
    output.push_back(static_cast<std::byte>(length));
    output.insert(
        output.end(),
        reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data() + length));
}

std::vector<std::byte> checkpoint_terminal_result(
    const CheckpointCommitBoundaryEvidence& correlation,
    const CheckpointCommitDisposition disposition,
    const std::uint64_t generation,
    const std::uint64_t current_dirty_epoch,
    const CheckpointFailureEvidence* failure = nullptr) {
    std::vector<std::byte> result{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'C'}, std::byte{'K'}, std::byte{'P'}, std::byte{'T'},
        std::byte{1}, static_cast<std::byte>(disposition)};
    result.insert(
        result.end(), correlation.product_operation_id.bytes().begin(),
        correlation.product_operation_id.bytes().end());
    result.insert(
        result.end(), correlation.image_id.bytes().begin(),
        correlation.image_id.bytes().end());
    append_u64(result, correlation.prior_generation);
    append_u64(result, generation);
    append_u64(result, correlation.cut_dirty_epoch);
    append_u64(result, current_dirty_epoch);
    result.push_back(failure != nullptr ? std::byte{1} : std::byte{0});
    if (failure != nullptr) {
        result.push_back(static_cast<std::byte>(failure->domain));
        append_u32(result, failure->code);
        append_u32(result, failure->native_code);
        append_u32(result, failure->context.code);
        result.push_back(static_cast<std::byte>(failure->context.phase));
        append_u32(result, failure->context.native_code);
        append_u64(result, failure->context.native_operation_id);
        result.insert(
            result.end(),
            failure->context.image_uuid.begin(),
            failure->context.image_uuid.end());
        append_u64(result, failure->context.generation);
        result.push_back(
            static_cast<std::byte>(failure->context.disposition));
        append_bounded_string(result, failure->context.operation);
        append_bounded_string(result, failure->context.safe_context);
    }
    return result;
}

std::vector<std::byte> force_stop_terminal_result(
    const profile::ProfileRecord& profile_record,
    const StopDurableState& stop) {
    std::vector<std::byte> result{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'F'}, std::byte{'S'}, std::byte{'T'}, std::byte{'P'},
        std::byte{1},
        stop.volatile_data_loss_acknowledged ? std::byte{1} : std::byte{0},
        stop.volatile_data_loss_possible ? std::byte{1} : std::byte{0},
        stop.before_state.has_value()
            ? static_cast<std::byte>(stop.before_state->disk.persistence)
            : std::byte{0xff}};
    result.insert(
        result.end(), profile_record.profile.image_uuid.bytes().begin(),
        profile_record.profile.image_uuid.bytes().end());
    append_u64(result, stop.confirmed_generation_before);
    append_u64(result, stop.dirty_epoch_before);
    return result;
}

std::vector<std::byte> forget_terminal_result(
    const ForgetTombstone& tombstone) {
    std::vector<std::byte> result{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'F'}, std::byte{'R'}, std::byte{'G'}, std::byte{'T'}};
    append_u32(result, 1U);
    result.insert(
        result.end(), tombstone.profile_id.bytes().begin(),
        tombstone.profile_id.bytes().end());
    result.insert(
        result.end(), tombstone.image_id.bytes().begin(),
        tombstone.image_id.bytes().end());
    append_u64(result, tombstone.confirmed_generation);
    append_u64(result, tombstone.profile_revision);
    append_u32(
        result,
        static_cast<std::uint32_t>(tombstone.retained_bundle_path.size()));
    for (const auto code_unit : tombstone.retained_bundle_path) {
        const auto value = static_cast<std::uint16_t>(code_unit);
        result.push_back(static_cast<std::byte>(value & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    }
    return result;
}

std::vector<std::byte> compaction_terminal_result(
    const CompactionMutationBoundaryEvidence& correlation,
    const CompactionCommitDisposition disposition,
    const std::uint64_t generation,
    const std::uint64_t current_dirty_epoch,
    const CompactionMetrics& metrics,
    const CompactionDurableState& durable) {
    std::vector<std::byte> result{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'C'}, std::byte{'M'}, std::byte{'P'}, std::byte{'T'},
        std::byte{2}, static_cast<std::byte>(disposition)};
    result.insert(
        result.end(), correlation.product_operation_id.bytes().begin(),
        correlation.product_operation_id.bytes().end());
    result.insert(
        result.end(), correlation.image_id.bytes().begin(),
        correlation.image_id.bytes().end());
    append_u64(result, correlation.prior_generation);
    append_u64(result, generation);
    append_u64(result, correlation.cut_dirty_epoch);
    append_u64(result, current_dirty_epoch);
    result.push_back(
        durable.prerequisite_checkpoint_performed
            ? std::byte{1} : std::byte{0});
    result.push_back(
        durable.prerequisite_checkpoint_disposition.has_value()
            ? static_cast<std::byte>(
                durable.prerequisite_checkpoint_disposition.value())
            : std::byte{0xff});
    append_u64(result, durable.prerequisite_checkpoint_generation);
    append_u64(result, metrics.source_base_bytes);
    append_u64(result, metrics.source_journal_bytes);
    append_u64(result, metrics.candidate_base_bytes);
    append_u64(result, metrics.candidate_journal_bytes);
    append_u64(result, metrics.required_peak_free_bytes);
    append_u64(result, metrics.available_free_bytes);
    append_u64(result, metrics.live_extent_count);
    append_u64(result, metrics.candidate_payloads);
    append_u64(result, metrics.snapshot_records);
    append_u64(result, metrics.data_records);
    append_u64(result, metrics.zero_records);
    append_u64(result, metrics.discard_records);
    append_u64(result, metrics.index_nodes);
    append_u64(result, metrics.newer_dirty_preserved);
    append_u64(result, metrics.retained_candidate_count);
    append_u64(result, metrics.deleted_candidate_count);
    append_u64(result, durable.successful_compactions_total);
    append_u64(result, durable.checkpoints_since_compaction);
    return result;
}

std::vector<std::byte> compaction_preflight_terminal_result(
    const WorkerCompactionPreflightEvidence& evidence) {
    std::vector<std::byte> result{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'C'}, std::byte{'M'}, std::byte{'P'}, std::byte{'D'},
        std::byte{1},
        evidence.future_checkpoint_possible ? std::byte{1} : std::byte{0}};
    result.insert(
        result.end(), evidence.product_operation_id.bytes().begin(),
        evidence.product_operation_id.bytes().end());
    result.insert(
        result.end(), evidence.image_id.bytes().begin(),
        evidence.image_id.bytes().end());
    append_u64(result, evidence.source_generation);
    append_u64(result, evidence.candidate_generation);
    append_u64(result, evidence.required_peak_free_bytes);
    append_u64(result, evidence.available_free_bytes);
    return result;
}

Result<bool> operation_payload_matches(
    const OperationRecord& operation,
    const control::Command& command) {
    if (operation.command_scope != control::command_name(command)) {
        return Result<bool>::success(false);
    }
    auto encoded = ipc::encode_command_payload(command);
    if (!encoded.ok()) {
        return Result<bool>::failure(encoded.error());
    }
    auto digest = sha256_digest(encoded.value().payload);
    if (!digest.ok()) {
        return Result<bool>::failure(digest.error());
    }
    return Result<bool>::success(
        digest.value() == operation.argument_digest);
}

} // namespace

namespace {

// Whether a durable runtime state asserts that a worker is (or should be)
// running for this profile.  Only settled states count: `starting` and
// `stopping` are windows in which the worker may legitimately be absent or
// already gone, so treating them as a liveness claim would report a normal
// transition as a conflict.
bool asserts_live_worker(product::RuntimeState runtime) noexcept {
    return runtime == product::RuntimeState::running;
}

// Attach the live worker facts the service holds for one profile.  Observations
// are per-profile and independent, so one profile's worker can never change
// what another profile reports.  An observation failure fails the read rather
// than leaving the profile looking unobserved: a caller that cannot tell the
// truth about which disks are live must not answer with a plausible-looking
// list instead.
Result<ManagedProfileSnapshot> annotate_runtime_observation(
    IWorkerControl& worker,
    ManagedProfileSnapshot profile) {
    auto observed =
        worker.observe_runtime(profile.record.profile.profile_uuid);
    if (!observed.ok()) {
        return Result<ManagedProfileSnapshot>::failure(observed.error());
    }
    profile.runtime_observation = observed.move_value();
    return Result<ManagedProfileSnapshot>::success(std::move(profile));
}

Result<std::vector<ManagedProfileSnapshot>> annotate_runtime_observations(
    IWorkerControl& worker,
    std::vector<ManagedProfileSnapshot> profiles) {
    for (auto& profile : profiles) {
        auto annotated = annotate_runtime_observation(worker, std::move(profile));
        if (!annotated.ok()) {
            return Result<std::vector<ManagedProfileSnapshot>>::failure(
                annotated.error());
        }
        profile = annotated.move_value();
    }
    return Result<std::vector<ManagedProfileSnapshot>>::success(
        std::move(profiles));
}

// The live dirty epoch for a profile whose durable state claims a clean, live
// runtime, from whichever seam the adapter exposes it through.
//
// The observation is asked first.  It is the M5 seam, it is taken live, and
// when a worker is bound its epoch is the same session the epoch-only query
// would reach -- so an adapter that participates is asked once and the answer
// is the one taken with the rest of the runtime facts.  An adapter that does
// not participate answers nothing there, which is why the older epoch-only
// query is still consulted: for that adapter this is M4's read path, and it
// must keep reporting exactly what M4 reported.
//
// A refused query is an absence of an answer, not an absence of truth.  The
// inherited implementation says the worker does not expose an epoch at all; the
// production adapter says no session is bound.  Neither is a fact about the
// disk, so neither contradicts the durable claim the snapshot already carries,
// and the read reports that claim.  A refusal that *is* a fact still fails the
// read: it arrives as an observation failure, which the caller propagates
// before asking this.
std::optional<std::uint64_t> observed_dirty_epoch(
    IWorkerControl& worker, const ManagedProfileSnapshot& snapshot) {
    if (snapshot.runtime_observation.has_value()) {
        return snapshot.runtime_observation.value().dirty_epoch;
    }
    auto live =
        worker.current_dirty_epoch(snapshot.record.profile.profile_uuid);
    if (!live.ok()) return std::nullopt;
    return live.value();
}

} // namespace

ProfileState reported_profile_state(const ManagedProfileSnapshot& snapshot) {
    ProfileState reported = snapshot.state;
    bool fail_closed = false;
    if (snapshot.state_provenance == ProfileStateProvenance::synthesized) {
        // Nothing about this profile's runtime, persistence or health has been
        // observed, so report it as blocked and needing attention rather than
        // repeating the placeholder.  The snapshot's own state is left alone:
        // the store's CAS, publish_runtime and the M4 load-then-publish
        // bootstrap all depend on that placeholder being usable.
        fail_closed = true;
    } else if (snapshot.runtime_observation.has_value()) {
        // An authoritative observation was taken.  A disagreement about
        // liveness means the durable record is stale or a worker is running
        // unaccounted for; either way neither claim is safe to report.  A
        // generation or dirty-epoch difference is deliberately not a conflict
        // here -- see the declaration.
        const WorkerRuntimeObservation& observed =
            snapshot.runtime_observation.value();
        const bool claims_live = asserts_live_worker(reported.disk.runtime);
        fail_closed = (claims_live && !observed.worker_alive)
            || (!claims_live && observed.worker_alive
                && reported.disk.runtime == product::RuntimeState::stopped);
    }
    if (fail_closed) {
        reported.disk.persistence = product::PersistenceState::persistence_blocked;
        reported.disk.health = product::HealthState::needs_attention;
    }
    return reported;
}

Result<ManagedProfileSnapshot> IServiceProfileStore::publish_checkpoint(
    const ManagedProfileSnapshot&,
    const ProfileState&,
    const OperationGateState&,
    const CheckpointDurableState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_checkpoint",
        L"The profile store does not implement atomic checkpoint truth"));
}

Result<std::vector<ManagedProfileSnapshot>> IServiceProfileStore::list_profiles() {
    return Result<std::vector<ManagedProfileSnapshot>>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.list_profiles",
        L"The profile store does not expose protected profile enumeration"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::update_profile_record(
    const ManagedProfileSnapshot&,
    const profile::ProfileRecord&,
    const ProfileState&,
    const OperationGateState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.update_profile_record",
        L"The profile store does not implement atomic profile publication"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::publish_compaction(
    const ManagedProfileSnapshot&,
    const ProfileState&,
    const OperationGateState&,
    const CheckpointDurableState&,
    const CompactionDurableState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_compaction",
        L"The profile store does not implement atomic compaction truth"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::publish_stop(
    const ManagedProfileSnapshot&,
    const ProfileState&,
    const OperationGateState&,
    const CheckpointDurableState&,
    const CompactionDurableState&,
    const StopDurableState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_stop",
        L"The profile store does not implement atomic stop truth"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::publish_rebind(
    const ManagedProfileSnapshot&,
    const profile::ProfileRecord&,
    const BundleRegistrationEvidence&,
    const ProfileState&,
    const OperationGateState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_rebind",
        L"The profile store does not implement atomic rebind publication"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::load_repair_candidate(
    const product::Uuid&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.load_repair_candidate",
        L"The profile store does not expose verified repair candidates"));
}

Result<ManagedProfileSnapshot> IServiceProfileStore::repair_from_previous(
    const ManagedProfileSnapshot&,
    const ProfileState&,
    const OperationGateState&) {
    return Result<ManagedProfileSnapshot>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.repair_from_previous",
        L"The profile store does not implement verified previous-profile repair"));
}

Result<RecoveryCommandReservation>
IServiceProfileStore::reserve_recovery_command(
    const ManagedProfileSnapshot&,
    const RecoveryCommandReservation&) {
    return Result<RecoveryCommandReservation>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.reserve_recovery_command",
        L"The profile store does not expose durable recovery reservations"));
}

Result<std::optional<RecoveryCommandReservation>>
IServiceProfileStore::find_recovery_command(
    const product::Uuid&) {
    return Result<std::optional<RecoveryCommandReservation>>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.find_recovery_command",
        L"The profile store does not expose durable recovery reservations"));
}

Result<ForgetTombstone> IServiceProfileStore::reserve_forget(
    const ManagedProfileSnapshot&,
    const ForgetTombstone&) {
    return Result<ForgetTombstone>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.reserve_forget",
        L"The profile store does not implement durable forget tombstones"));
}

Result<std::optional<ForgetTombstone>>
IServiceProfileStore::find_forget_tombstone(
    const product::Uuid&) {
    return Result<std::optional<ForgetTombstone>>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.find_forget_tombstone",
        L"The profile store does not expose durable forget tombstones"));
}

Result<ForgetTombstone> IServiceProfileStore::remove_forget_metadata(
    const ForgetMetadataRemovalRequest&,
    const ForgetSafetyEvidence&) {
    return Result<ForgetTombstone>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.remove_forget_metadata",
        L"The profile store does not implement metadata-only forget"));
}

Result<WorkerActionResult> IWorkerControl::reclaim_leftover(
    const WorkerActionRequest&) {
    // The default an adapter inherits when it has no way to dismantle a leftover, and it
    // **fails** rather than succeeding.  That direction is the whole point: the sweep turns
    // a failed reclaim into the profile staying in its needs-attention count, while a
    // success would record the residue as cleared and leave an operator with a drive letter
    // that outlives the disk.
    return Result<WorkerActionResult>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.reclaim_leftover",
        L"The worker does not implement leftover reclamation"));
}

Result<WorkerActionResult> IWorkerControl::execute_checkpoint(
    const WorkerActionRequest&,
    const CheckpointCommitBoundaryHandler&) {
    return Result<WorkerActionResult>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.execute_checkpoint",
        L"The worker does not implement the checkpoint boundary protocol"));
}

Result<WorkerActionResult> IWorkerControl::reconcile_checkpoint(
    const WorkerActionRequest&,
    const CheckpointCommitBoundaryEvidence&) {
    return Result<WorkerActionResult>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.reconcile_checkpoint",
        L"The worker does not implement checkpoint reconciliation"));
}

Result<void> IWorkerControl::publish_checkpoint_epoch(
    const WorkerActionRequest&,
    const CheckpointCommitBoundaryEvidence&,
    const CheckpointEpochPublicationHandler&) {
    return Result<void>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_checkpoint_epoch",
        L"The worker does not implement the dirty-epoch publication fence"));
}

Result<WorkerActionResult> IWorkerControl::execute_compaction(
    const WorkerActionRequest&,
    const CompactionMutationBoundaryHandler&,
    const CompactionCutPublicationHandler&) {
    return Result<WorkerActionResult>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.execute_compaction",
        L"The worker does not implement the compaction boundary protocol"));
}

Result<WorkerActionResult> IWorkerControl::reconcile_compaction(
    const WorkerActionRequest&,
    const CompactionMutationBoundaryEvidence&) {
    return Result<WorkerActionResult>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.reconcile_compaction",
        L"The worker does not implement compaction reconciliation"));
}

Result<void> IWorkerControl::publish_compaction_epoch(
    const WorkerActionRequest&,
    const CompactionMutationBoundaryEvidence&,
    const CompactionEpochPublicationHandler&) {
    return Result<void>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.publish_compaction_epoch",
        L"The worker does not implement the compaction dirty-epoch fence"));
}

Result<std::uint64_t> IWorkerControl::current_dirty_epoch(
    const product::Uuid&) {
    return Result<std::uint64_t>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.current_dirty_epoch",
        L"The worker does not expose the authoritative dirty epoch"));
}

Result<WorkerRuntimeObservation> IWorkerControl::require_runtime_observation(
    const product::Uuid&) {
    return Result<WorkerRuntimeObservation>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.require_runtime_observation",
        L"The worker does not implement the strict runtime observation"));
}

Result<std::optional<WorkerRuntimeObservation>> IWorkerControl::observe_runtime(
    const product::Uuid&) {
    // Deliberately not a failure, unlike current_dirty_epoch: a caller that
    // cannot observe a worker has learned nothing, which is a legitimate answer
    // the read path already knows how to handle.  Failing here would turn every
    // adapter that predates runtime consistency into a broken read path.
    return Result<std::optional<WorkerRuntimeObservation>>::success(
        std::nullopt);
}

Result<ForgetSafetyEvidence> IWorkerControl::prove_forget_safe(
    const WorkerActionRequest&) {
    return Result<ForgetSafetyEvidence>::failure(service_error(
        ServiceCoreError::dependency_failure,
        L"service_core.prove_forget_safe",
        L"The worker control does not expose forget safety proof"));
}

Result<profile::MemoryPolicy> materialize_memory_policy(
    control::MemoryMode mode,
    std::optional<std::uint64_t> requested_hard_limit_bytes,
    std::uint64_t logical_capacity_bytes,
    PhysicalMemorySnapshot physical_memory) {
    if (logical_capacity_bytes < kAutomaticSnapshotCowLimitBytes
        || physical_memory.total_bytes == 0
        || physical_memory.available_bytes > physical_memory.total_bytes
        || (mode == control::MemoryMode::automatic
            && requested_hard_limit_bytes.has_value())
        || (mode == control::MemoryMode::fixed
            && !requested_hard_limit_bytes.has_value())) {
        return Result<profile::MemoryPolicy>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.materialize_memory",
            L"The requested memory policy cannot be materialized"));
    }
    const auto requested = mode == control::MemoryMode::automatic
        ? (std::min)(physical_memory.total_bytes / 4, logical_capacity_bytes)
        : requested_hard_limit_bytes.value();
    const auto hard = align_down_memory(requested);
    const auto soft = align_down_memory(hard - hard / 10);
    const auto low = align_down_memory(hard - hard / 4);
    if (hard > logical_capacity_bytes || low == 0
        || !(low < soft && soft < hard)) {
        return Result<profile::MemoryPolicy>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.materialize_memory",
            L"The effective memory limits are too small or out of order"));
    }
    return Result<profile::MemoryPolicy>::success(profile::MemoryPolicy{
        mode == control::MemoryMode::automatic
            ? profile::MemoryMode::automatic : profile::MemoryMode::fixed,
        hard,
        soft,
        low,
        kAutomaticSnapshotCowLimitBytes});
}

Result<ServiceResponse> ServiceCore::dispatch(
    const ServiceCommandRequest& request) {
    auto encoded = ipc::encode_command_payload(request.command);
    if (!encoded.ok()) {
        return Result<ServiceResponse>::failure(encoded.error());
    }

    auto resolved_request_id = request.request_id.has_value()
        ? Result<product::Uuid>::success(request.request_id.value())
        : ids_.next();
    if (!resolved_request_id.ok()) {
        return Result<ServiceResponse>::failure(resolved_request_id.error());
    }
    const auto request_id = resolved_request_id.move_value();
    if (is_zero_uuid(request_id)) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.request_id",
            L"The request identifier must be nonzero"));
    }

    const std::string command(control::command_name(request.command));
    if (std::holds_alternative<control::DiskListCommand>(request.command)) {
        if (request.no_wait || request.dry_run || request.wait_timeout_ms.has_value()) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::invalid_request,
                L"service_core.list",
                L"List is a read-only command"));
        }
        const auto authorization = ipc::authorize_public_command(
            static_cast<std::uint32_t>(encoded.value().command_id),
            request.caller, std::nullopt);
        if (!authorization.allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::authorization_denied,
                L"service_core.authorize_list",
                L"The caller is not authorized to enumerate profiles"));
        }
        auto listed = profiles_.list_profiles();
        if (!listed.ok()) return Result<ServiceResponse>::failure(listed.error());
        if (authorization.visibility == ipc::AuthorizationVisibility::none) {
            listed.value().clear();
        } else if (authorization.visibility
                   == ipc::AuthorizationVisibility::caller_only) {
            std::vector<ManagedProfileSnapshot> visible;
            visible.reserve(listed.value().size());
            for (auto& profile : listed.value()) {
                if (profile.record.profile.owner_sid == request.caller.primary_sid) {
                    visible.push_back(std::move(profile));
                }
            }
            listed = Result<std::vector<ManagedProfileSnapshot>>::success(
                std::move(visible));
        }
        // Annotate only what the caller may see, so an unauthorized profile's
        // worker is never even queried.
        auto annotated = annotate_runtime_observations(worker_, listed.move_value());
        if (!annotated.ok()) {
            return Result<ServiceResponse>::failure(annotated.error());
        }
        ServiceResponse response{
            request_id, command, ServiceResponseDisposition::completed,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, {}};
        response.profiles = annotated.move_value();
        return Result<ServiceResponse>::success(std::move(response));
    }

    const auto* config_set = std::get_if<control::DiskConfigSetCommand>(
        &request.command);
    if (config_set != nullptr) {
        if (request.dry_run || request.no_wait || request.wait_timeout_ms.has_value()
            || config_set->assignments.size() != 1
            || config_set->assignments.front().key != L"auto_start") {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::invalid_request,
                L"service_core.config_set",
                L"Only one auto_start assignment is supported"));
        }
        auto profile = profiles_.resolve(config_set->target);
        if (!profile.ok()) return Result<ServiceResponse>::failure(profile.error());
        const auto authorization = ipc::authorize_public_command(
            static_cast<std::uint32_t>(encoded.value().command_id),
            request.caller, profile.value().record.profile.owner_sid);
        if (!authorization.allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::authorization_denied,
                L"service_core.authorize_config",
                L"The caller is not authorized to change this profile"));
        }
        const auto value = config_set->assignments.front().value;
        const bool enabled = value == L"true" || value == L"on" || value == L"1";
        const bool disabled = value == L"false" || value == L"off" || value == L"0";
        if (!enabled && !disabled) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::invalid_request,
                L"service_core.config_set",
                L"auto_start must be true or false"));
        }
        if (profile.value().record.profile.auto_start == enabled) {
            auto annotated =
                annotate_runtime_observation(worker_, profile.move_value());
            if (!annotated.ok()) {
                return Result<ServiceResponse>::failure(annotated.error());
            }
            return Result<ServiceResponse>::success(ServiceResponse{
                request_id, command, ServiceResponseDisposition::completed,
                std::nullopt, std::nullopt, annotated.move_value(), std::nullopt,
                {control::IdempotentWarning::no_changes}, {}});
        }
        if (profile.value().operation_gate.active_operation_id.has_value()) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.config_set",
                L"Another operation is active for this profile"));
        }
        if (profile.value().record.revision == (std::numeric_limits<std::uint64_t>::max)()) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::invalid_request,
                L"service_core.config_set",
                L"The profile revision is exhausted"));
        }
        auto proposed = profile.value().record;
        ++proposed.revision;
        proposed.profile.auto_start = enabled;
        auto updated = profiles_.update_profile_record(
            profile.value(), proposed, profile.value().state,
            profile.value().operation_gate);
        if (!updated.ok()) return Result<ServiceResponse>::failure(updated.error());
        ServiceResponse response{
            request_id, command, ServiceResponseDisposition::completed,
            std::nullopt, std::nullopt, updated.move_value(), std::nullopt, {}, {}};
        return Result<ServiceResponse>::success(std::move(response));
    }
    const auto* operation_show = std::get_if<control::OperationShowCommand>(
        &request.command);
    const auto* operation_wait = std::get_if<control::OperationWaitCommand>(
        &request.command);
    if (operation_show != nullptr || operation_wait != nullptr) {
        if (request.no_wait || request.dry_run
            || (operation_show != nullptr
                && request.wait_timeout_ms.has_value())) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::invalid_request,
                L"service_core.dispatch",
                L"The operation query options are invalid"));
        }
        const auto& operation_id = operation_show != nullptr
            ? operation_show->operation_id : operation_wait->operation_id;
        auto operation = operations_.load(operation_id);
        if (!operation.ok()) {
            return Result<ServiceResponse>::failure(operation.error());
        }
        auto owner = profiles_.load(operation.value().profile_id);
        std::wstring authorization_owner;
        if (owner.ok()) {
            authorization_owner = owner.value().record.profile.owner_sid;
        } else if (operation.value().command_scope == "disk.create"
                   || operation.value().command_scope == "disk.import") {
            authorization_owner = operation.value().caller_sid;
        } else if (operation.value().command_scope == "disk.forget"
                   && is_profile_not_found(owner.error())) {
            auto tombstone = profiles_.find_forget_tombstone(
                operation.value().operation_id);
            if (!tombstone.ok()) {
                return Result<ServiceResponse>::failure(
                    tombstone.error());
            }
            if (!tombstone.value().has_value()
                || tombstone.value()->phase
                    != ForgetTombstonePhase::metadata_removed
                || !forget_tombstone_matches_operation(
                    tombstone.value().value(), operation.value())) {
                return Result<ServiceResponse>::failure(owner.error());
            }
            authorization_owner =
                tombstone.value()->authorization_owner_sid;
        } else {
            return Result<ServiceResponse>::failure(owner.error());
        }
        const auto authorization = ipc::authorize_public_command(
            static_cast<std::uint32_t>(encoded.value().command_id),
            request.caller,
            authorization_owner);
        if (!authorization.allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::authorization_denied,
                L"service_core.authorize_operation",
                L"The caller is not authorized for this operation"));
        }
        logger_.write(ServiceLogEvent{
            ServiceLogKind::request_authorized,
            request_id,
            operation_id,
            operation.value().profile_id,
            command,
            "authorized"});
        if (operation_show != nullptr || is_terminal(operation.value().state)) {
            ServiceResponse response{
                request_id,
                command,
                ServiceResponseDisposition::completed,
                operation_id,
                operation.move_value(),
                std::nullopt,
                std::nullopt,
                {}};
            return Result<ServiceResponse>::success(std::move(response));
        }
        if (request.client_wait == ClientWaitState::cancelled) {
            logger_.write(ServiceLogEvent{
                ServiceLogKind::client_wait_cancelled,
                request_id,
                operation_id,
                operation.value().profile_id,
                command,
                "client-wait-cancelled"});
            ServiceResponse response{
                request_id,
                command,
                ServiceResponseDisposition::client_wait_cancelled,
                operation_id,
                operation.move_value(),
                std::nullopt,
                std::nullopt,
                {}};
            return Result<ServiceResponse>::success(std::move(response));
        }
        constexpr std::uint64_t kMaximumWaitMs = 30'000;
        const auto requested_wait = (std::min)(
            request.wait_timeout_ms.value_or(kMaximumWaitMs),
            kMaximumWaitMs);
        const auto now = clock_.monotonic_time_ms();
        const auto deadline = requested_wait
                > (std::numeric_limits<std::uint64_t>::max)() - now
            ? (std::numeric_limits<std::uint64_t>::max)()
            : now + requested_wait;
        auto observation = operations_.wait_for_change(
            operation_id, operation.value().revision, deadline);
        if (!observation.ok()) {
            return Result<ServiceResponse>::failure(observation.error());
        }
        if (observation.value().disposition
                == OperationWaitDisposition::timed_out
            || !observation.value().record.has_value()) {
            logger_.write(ServiceLogEvent{
                ServiceLogKind::wait_timeout,
                request_id,
                operation_id,
                operation.value().profile_id,
                command,
                "wait-timeout"});
            ServiceResponse response{
                request_id,
                command,
                ServiceResponseDisposition::wait_timeout,
                operation_id,
                operation.move_value(),
                std::nullopt,
                std::nullopt,
                {}};
            return Result<ServiceResponse>::success(std::move(response));
        }
        ServiceResponse response{
            request_id,
            command,
            ServiceResponseDisposition::completed,
            operation_id,
            observation.value().record.value(),
            std::nullopt,
            std::nullopt,
            {}};
        return Result<ServiceResponse>::success(std::move(response));
    }

    const auto* create = std::get_if<control::DiskCreateCommand>(
        &request.command);
    const auto* import = std::get_if<control::DiskImportCommand>(
        &request.command);
    if (create != nullptr || import != nullptr) {
        if (registrations_ == nullptr || request.dry_run
            || request.wait_timeout_ms.has_value()
            || (request.no_wait && request.wait_timeout_ms.has_value())) {
            return Result<ServiceResponse>::failure(service_error(
                registrations_ == nullptr
                    ? ServiceCoreError::dependency_failure
                    : ServiceCoreError::invalid_request,
                L"service_core.registration_dispatch",
                L"Registration support or request options are invalid"));
        }
        const auto owner_sid = create != nullptr
            ? create->owner_sid.value_or(request.caller.primary_sid)
            : import->owner_sid.value_or(request.caller.primary_sid);
        const auto authorization = ipc::authorize_public_command(
            static_cast<std::uint32_t>(encoded.value().command_id),
            request.caller,
            owner_sid);
        if (!authorization.allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::authorization_denied,
                L"service_core.authorize_registration",
                L"Only an elevated administrator may register a profile"));
        }
        auto profile_id = derive_request_uuid(request_id, "profile");
        if (!profile_id.ok()) {
            return Result<ServiceResponse>::failure(profile_id.error());
        }
        std::optional<product::Uuid> expected_image_id;
        if (create != nullptr) {
            auto image_id = derive_request_uuid(request_id, "image");
            if (!image_id.ok()) {
                return Result<ServiceResponse>::failure(image_id.error());
            }
            expected_image_id = image_id.move_value();
        }
        auto memory = memory_.query();
        if (!memory.ok()) {
            return Result<ServiceResponse>::failure(memory.error());
        }
        LifecycleOperationIntent intent{
            create != nullptr ? LifecycleOperationKind::create
                              : LifecycleOperationKind::import,
            profile_id.value(),
            expected_image_id,
            owner_sid,
            create != nullptr ? create->name : import->name,
            create != nullptr ? create->bundle : import->bundle,
            create != nullptr ? create->mount : import->mount,
            create != nullptr ? create->size_bytes : 0,
            create != nullptr && !create->no_start,
            create != nullptr
                ? create->memory_mode.value_or(control::MemoryMode::automatic)
                : control::MemoryMode::automatic,
            create != nullptr ? create->memory_hard_limit_bytes
                              : std::nullopt,
            create != nullptr ? create->checkpoint_interval : std::nullopt,
            create != nullptr ? create->dirty_threshold : std::nullopt,
            memory.value(),
            clock_.unix_time_ms()};
        if (create != nullptr) {
            auto materialized = materialize_memory_policy(
                intent.memory_mode,
                intent.memory_hard_limit_bytes,
                intent.requested_capacity_bytes,
                intent.physical_memory);
            if (!materialized.ok()) {
                return Result<ServiceResponse>::failure(materialized.error());
            }
        }
        auto digest = sha256_digest(encoded.value().payload);
        if (!digest.ok()) {
            return Result<ServiceResponse>::failure(digest.error());
        }
        const auto management_command = create != nullptr
            ? (create->no_start ? ManagementCommand::create_stopped
                                : ManagementCommand::create_and_start)
            : ManagementCommand::import_profile;
        const auto plan = plan_command(
            absent_profile_state(), management_command);
        if (!plan.accepted()
            || plan.disposition != TransitionDisposition::allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.registration_plan",
                L"The registration operation plan was rejected"));
        }
        const OperationRequest operation_request{
            request_id,
            intent.profile_id,
            request.caller.primary_sid,
            command,
            digest.value(),
            intent.created_unix_ms};
        auto existing = operations_.find_request(operation_request);
        if (!existing.ok()) {
            return Result<ServiceResponse>::failure(existing.error());
        }
        if (existing.value().has_value()) {
            const auto& replay = existing.value()->record;
            if (!is_terminal(replay.state)) {
                auto replay_intent = intent;
                replay_intent.created_unix_ms = replay.created_unix_ms;
                const LifecycleRegistrationReservation proposed_reservation{
                    1,
                    replay.operation_id,
                    request_id,
                    digest.value(),
                    replay_intent};
                auto reserved = registrations_->reserve(
                    proposed_reservation);
                if (!reserved.ok()) {
                    return Result<ServiceResponse>::failure(
                        reserved.error());
                }
                if (!reservation_matches_operation(
                        reserved.value(), replay)) {
                    return Result<ServiceResponse>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.registration_replay",
                        L"The durable lifecycle reservation conflicts with the replay"));
                }
            }
            return Result<ServiceResponse>::success(ServiceResponse{
                request_id,
                command,
                is_terminal(replay.state)
                    ? ServiceResponseDisposition::completed
                    : queued_response_disposition(request),
                replay.operation_id,
                replay,
                std::nullopt,
                std::nullopt,
                warnings_from(replay)});
        }
        auto operation_id = ids_.next();
        if (!operation_id.ok()) {
            return Result<ServiceResponse>::failure(operation_id.error());
        }
        const auto gate = plan_gate_access(
            idle_operation_gate(), plan, operation_id.value());
        if (!gate.accepted()) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.registration_gate",
                L"The registration operation gate was rejected"));
        }
        auto admission = operations_.admit(
            operation_request, operation_id.value());
        if (!admission.ok()) {
            return Result<ServiceResponse>::failure(admission.error());
        }
        if (!admission.value().replayed) {
            const LifecycleRegistrationReservation proposed_reservation{
                1,
                operation_id.value(),
                request_id,
                digest.value(),
                intent};
            auto reserved = registrations_->reserve(proposed_reservation);
            if (!reserved.ok()
                || !reservation_equal(
                    reserved.value(), proposed_reservation)) {
                auto terminal = admission.value().record;
                ++terminal.revision;
                terminal.state = OperationState::failed;
                terminal.phase = "terminal";
                terminal.updated_unix_ms = (std::max)(
                    terminal.updated_unix_ms, clock_.unix_time_ms());
                terminal.terminal_unix_ms = terminal.updated_unix_ms;
                terminal.cancellable = false;
                terminal.terminal_result = {
                    std::byte{'r'}, std::byte{'e'}, std::byte{'s'},
                    std::byte{'e'}, std::byte{'r'}, std::byte{'v'},
                    std::byte{'e'}};
                auto terminal_write = operations_.write(
                    admission.value().record.revision, terminal);
                if (!terminal_write.ok()) {
                    return Result<ServiceResponse>::failure(
                        terminal_write.error());
                }
                logger_.write(ServiceLogEvent{
                    ServiceLogKind::operation_terminal,
                    request_id,
                    terminal.operation_id,
                    intent.profile_id,
                    command,
                    "terminal"});
                return Result<ServiceResponse>::success(ServiceResponse{
                    request_id,
                    command,
                    ServiceResponseDisposition::completed,
                    terminal.operation_id,
                    terminal,
                    std::nullopt,
                    std::nullopt,
                    {}});
            }
            auto admitting = admission.value().record;
            ++admitting.revision;
            admitting.state = OperationState::running;
            admitting.phase = "admitting";
            admitting.updated_unix_ms = (std::max)(
                admitting.updated_unix_ms, clock_.unix_time_ms());
            auto write = operations_.write(
                admission.value().record.revision, admitting);
            if (!write.ok()) {
                return Result<ServiceResponse>::failure(write.error());
            }
            admission.value().record = admitting;
            {
                std::scoped_lock lock(pending_mutex_);
                pending_.push_back(PendingExecution{
                    admitting.operation_id,
                    request_id,
                    plan,
                    intent});
            }
            logger_.write(ServiceLogEvent{
                ServiceLogKind::operation_queued,
                request_id,
                admitting.operation_id,
                intent.profile_id,
                command,
                "queued"});
        }
        return Result<ServiceResponse>::success(ServiceResponse{
            request_id,
            command,
            queued_response_disposition(request),
            admission.value().record.operation_id,
            admission.value().record,
            std::nullopt,
            std::nullopt,
            {}});
    }

    const auto* show = std::get_if<control::DiskShowCommand>(&request.command);
    const auto* start = std::get_if<control::DiskStartCommand>(
        &request.command);
    const auto* stop = std::get_if<control::DiskStopCommand>(
        &request.command);
    const auto* checkpoint = std::get_if<control::DiskCheckpointCommand>(
        &request.command);
    const auto* compact = std::get_if<control::DiskCompactCommand>(
        &request.command);
    const auto* repair = std::get_if<control::DiskRepairProfileCommand>(
        &request.command);
    const auto* rebind = std::get_if<control::DiskRebindCommand>(
        &request.command);
    const auto* forget = std::get_if<control::DiskForgetCommand>(
        &request.command);
    const bool force_stop_requested = stop != nullptr
        && stop->force && stop->accept_volatile_data_loss;
    if (show == nullptr && start == nullptr && stop == nullptr
        && checkpoint == nullptr
        && compact == nullptr && repair == nullptr && rebind == nullptr
        && forget == nullptr) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::unsupported_command,
            L"service_core.dispatch",
            L"The management command is outside this core command surface"));
    }
    if ((start != nullptr || stop != nullptr || checkpoint != nullptr
         || compact != nullptr || repair != nullptr || rebind != nullptr
         || forget != nullptr)
        && ((request.dry_run
             && (request.no_wait || request.wait_timeout_ms.has_value()))
            || (request.no_wait && request.wait_timeout_ms.has_value()))) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.dispatch",
            L"The mutation wait options are mutually incompatible"));
    }
    if (show != nullptr && (request.no_wait || request.dry_run
                            || request.wait_timeout_ms.has_value())) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.dispatch",
            L"Query commands do not accept operation wait options"));
    }

    if (!request.dry_run
        && (repair != nullptr || rebind != nullptr || forget != nullptr)) {
        const auto replay_authorization = ipc::authorize_public_command(
            static_cast<std::uint32_t>(encoded.value().command_id),
            request.caller,
            std::nullopt);
        if (!replay_authorization.allowed) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::authorization_denied,
                L"service_core.authorize_recovery_replay",
                L"The caller is not authorized for this recovery command"));
        }
        auto replay_digest = sha256_digest(encoded.value().payload);
        if (!replay_digest.ok()) {
            return Result<ServiceResponse>::failure(
                replay_digest.error());
        }
        const auto replay_profile_id = repair != nullptr
            ? repair->profile_id
            : rebind != nullptr ? rebind->profile_id
                                : forget->profile_id;
        const OperationRequest replay_request{
            request_id,
            replay_profile_id,
            request.caller.primary_sid,
            command,
            replay_digest.value(),
            clock_.unix_time_ms()};
        auto replay = operations_.find_request(replay_request);
        if (!replay.ok()) {
            return Result<ServiceResponse>::failure(replay.error());
        }
        if (replay.value().has_value()) {
            const auto& record = replay.value()->record;
            if (forget != nullptr) {
                auto tombstone = profiles_.find_forget_tombstone(
                    record.operation_id);
                if (!tombstone.ok()) {
                    return Result<ServiceResponse>::failure(
                        tombstone.error());
                }
                const bool unowned_terminal =
                    is_unowned_admission_terminal(record);
                if (!tombstone.value().has_value()) {
                    if (!unowned_terminal) {
                        return Result<ServiceResponse>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.forget_replay",
                            L"The forget replay has no durable tombstone"));
                    }
                } else {
                    const auto& durable = tombstone.value().value();
                    const bool valid_phase =
                        record.state == OperationState::succeeded
                        ? durable.phase
                            == ForgetTombstonePhase::metadata_removed
                        : !unowned_terminal
                            && (durable.phase
                                    == ForgetTombstonePhase::reserved
                                || durable.phase
                                    == ForgetTombstonePhase::metadata_removed);
                    if (!valid_phase
                        || !forget_tombstone_matches_operation(
                            durable, record)) {
                        return Result<ServiceResponse>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.forget_replay",
                            L"The durable forget tombstone conflicts with the replay"));
                    }
                }
            }
            return Result<ServiceResponse>::success(ServiceResponse{
                request_id,
                command,
                is_terminal(record.state)
                    ? ServiceResponseDisposition::completed
                    : queued_response_disposition(request),
                record.operation_id,
                record,
                std::nullopt,
                std::nullopt,
                warnings_from(record)});
        }
    }

    auto profile = repair != nullptr
        ? profiles_.load_repair_candidate(repair->profile_id)
        : rebind != nullptr
        ? profiles_.load(rebind->profile_id)
        : forget != nullptr
        ? profiles_.load(forget->profile_id)
        : profiles_.resolve(
            show != nullptr ? std::wstring_view(show->target)
            : start != nullptr ? std::wstring_view(start->target)
            : stop != nullptr ? std::wstring_view(stop->target)
            : checkpoint != nullptr ? std::wstring_view(checkpoint->target)
                                    : std::wstring_view(compact->target));
    if (!profile.ok()) {
        return Result<ServiceResponse>::failure(profile.error());
    }
    if (stop != nullptr && !force_stop_requested
        && !known_normal_stop_policy(
            profile.value().record.profile.checkpoint.normal_stop)) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.stop_policy",
            L"The persisted normal-stop policy is invalid"));
    }
    if ((repair != nullptr && !repair->from_previous)
        || (rebind != nullptr
            && (rebind->bundle.empty()
                || rebind->profile_id
                    != profile.value().record.profile.profile_uuid
                || rebind->expected_image_id
                    != profile.value().record.profile.image_uuid))) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.recovery_request",
            L"The repair or rebind identity confirmation is invalid"));
    }
    if (forget != nullptr
        && (forget->profile_id
                != profile.value().record.profile.profile_uuid
            || forget->confirmation_id != forget->profile_id)) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::invalid_request,
            L"service_core.forget_confirmation",
            L"The forget confirmation must exactly match the profile UUID"));
    }
    const auto authorization = ipc::authorize_public_command(
        static_cast<std::uint32_t>(encoded.value().command_id),
        request.caller,
        profile.value().record.profile.owner_sid);
    if (!authorization.allowed) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::authorization_denied,
            L"service_core.authorize",
            L"The caller is not authorized for this profile"));
    }
    if (force_stop_requested
        && !(request.caller.local_system
             || request.caller.service_identity
             || (request.caller.administrator
                 && request.caller.elevated))) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::authorization_denied,
            L"service_core.authorize_force_stop",
            L"Force stop requires an elevated administrator"));
    }

    logger_.write(ServiceLogEvent{
        ServiceLogKind::request_authorized,
        request_id,
        std::nullopt,
        profile.value().record.profile.profile_uuid,
        command,
        "authorized"});

    if (show != nullptr) {
        // The runtime facts are attached first, because a read that cannot be
        // observed must not answer with a plausible-looking profile -- and an
        // observation *failure* still fails the command here, unchanged.
        auto annotated =
            annotate_runtime_observation(worker_, profile.move_value());
        if (!annotated.ok()) {
            return Result<ServiceResponse>::failure(annotated.error());
        }
        auto snapshot = annotated.move_value();
        // Only a profile that claims to be live and clean with a committed
        // checkpoint can be contradicted by a later epoch; any other profile
        // already says something the epoch could not improve on.  This is M4's
        // gate, unchanged.
        if (snapshot.state.disk.runtime == product::RuntimeState::running
            && snapshot.state.disk.persistence
                == product::PersistenceState::clean
            && snapshot.checkpoint.confirmed_generation != 0) {
            const auto live = observed_dirty_epoch(worker_, snapshot);
            if (live.has_value()
                && live.value() > snapshot.checkpoint.current_dirty_epoch) {
                snapshot.checkpoint.current_dirty_epoch = live.value();
                snapshot.state.disk.persistence =
                    product::PersistenceState::dirty;
            }
        }
        ServiceResponse response{
            request_id,
            command,
            ServiceResponseDisposition::completed,
            std::nullopt,
            std::nullopt,
            std::move(snapshot),
            std::nullopt,
            {}};
        return Result<ServiceResponse>::success(std::move(response));
    }

    if ((checkpoint != nullptr || compact != nullptr)
        && profile.value().state.disk.runtime
            == product::RuntimeState::running
        && profile.value().state.disk.persistence
            == product::PersistenceState::clean
        && profile.value().checkpoint.confirmed_generation != 0) {
        auto live_epoch = worker_.current_dirty_epoch(
            profile.value().record.profile.profile_uuid);
        if (!live_epoch.ok()) {
            return Result<ServiceResponse>::failure(live_epoch.error());
        }
        if (live_epoch.value()
            > profile.value().checkpoint.current_dirty_epoch) {
            auto observed_dirty = profile.value();
            observed_dirty.checkpoint.current_dirty_epoch =
                live_epoch.value();
            observed_dirty.state.disk.persistence =
                product::PersistenceState::dirty;
            if (request.dry_run) {
                profile.value() = std::move(observed_dirty);
            } else {
                auto published = profiles_.publish_checkpoint(
                    profile.value(),
                    observed_dirty.state,
                    observed_dirty.operation_gate,
                    observed_dirty.checkpoint);
                if (!published.ok()) {
                    return Result<ServiceResponse>::failure(
                        published.error());
                }
                profile.value() = published.move_value();
            }
        }
    }

    if (request.dry_run) {
        const auto management_command = start != nullptr
            ? ManagementCommand::start
            : stop != nullptr
                ? (force_stop_requested ? ManagementCommand::force_stop
                                        : ManagementCommand::stop)
            : compact != nullptr ? ManagementCommand::compact
            : repair != nullptr ? ManagementCommand::repair_profile
            : rebind != nullptr ? ManagementCommand::rebind
            : forget != nullptr ? ManagementCommand::forget
                                : ManagementCommand::checkpoint;
        const auto plan = plan_command(profile.value().state,
                                       management_command);
        if (!plan.accepted()) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.plan",
                L"The profile state rejects this management command"));
        }
        if (management_command == ManagementCommand::start
            && plan.disposition == TransitionDisposition::allowed) {
            auto physical_memory = memory_.query();
            if (!physical_memory.ok()) {
                return Result<ServiceResponse>::failure(
                    physical_memory.error());
            }
            if (physical_memory.value().total_bytes == 0
                || physical_memory.value().available_bytes
                    > physical_memory.value().total_bytes) {
                return Result<ServiceResponse>::failure(service_error(
                    ServiceCoreError::dependency_failure,
                    L"service_core.memory_preflight",
                    L"The physical-memory observation is invalid"));
            }
        }
        ServiceResponse response{
            request_id,
            command,
            ServiceResponseDisposition::dry_run,
            std::nullopt,
            std::nullopt,
            profile.move_value(),
            plan,
            warnings_for(plan.disposition)};
        return Result<ServiceResponse>::success(std::move(response));
    }

    auto digest = sha256_digest(encoded.value().payload);
    if (!digest.ok()) {
        return Result<ServiceResponse>::failure(digest.error());
    }
    const OperationRequest operation_request{
        request_id,
        profile.value().record.profile.profile_uuid,
        request.caller.primary_sid,
        command,
        digest.value(),
        clock_.unix_time_ms()};
    auto existing = operations_.find_request(operation_request);
    if (!existing.ok()) {
        return Result<ServiceResponse>::failure(existing.error());
    }
    if (existing.value().has_value()) {
        const auto& replay = existing.value().value().record;
        ServiceResponse response{
            request_id,
            command,
            is_terminal(replay.state)
                ? ServiceResponseDisposition::completed
                : queued_response_disposition(request),
            replay.operation_id,
            replay,
            std::nullopt,
            std::nullopt,
            warnings_from(replay)};
        return Result<ServiceResponse>::success(std::move(response));
    }

    if ((checkpoint != nullptr || compact != nullptr)
        && profile.value().operation_gate.active_operation_id.has_value()
        && profile.value().operation_gate.active_command
            == (compact != nullptr ? ManagementCommand::compact
                                   : ManagementCommand::checkpoint)) {
        auto active = operations_.load(
            profile.value().operation_gate.active_operation_id.value());
        if (!active.ok()) {
            return Result<ServiceResponse>::failure(active.error());
        }
        if (is_terminal(active.value().state)
            || active.value().profile_id
                != profile.value().record.profile.profile_uuid
            || active.value().command_scope
                != (compact != nullptr ? "disk.compact"
                                       : "disk.checkpoint")) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.mutation_coalesce",
                L"The active persistence gate has no equivalent operation"));
        }
        return Result<ServiceResponse>::success(ServiceResponse{
            request_id,
            command,
            queued_response_disposition(request),
            active.value().operation_id,
            active.move_value(),
            std::nullopt,
            std::nullopt,
            {}});
    }

    const auto management_command = start != nullptr
        ? ManagementCommand::start
        : stop != nullptr
            ? (force_stop_requested ? ManagementCommand::force_stop
                                    : ManagementCommand::stop)
        : compact != nullptr ? ManagementCommand::compact
        : repair != nullptr ? ManagementCommand::repair_profile
        : rebind != nullptr ? ManagementCommand::rebind
        : forget != nullptr ? ManagementCommand::forget
                            : ManagementCommand::checkpoint;
    const auto plan = plan_command(profile.value().state,
                                   management_command);
    if (!plan.accepted()) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::state_rejected,
            L"service_core.plan",
            L"The profile state rejects this management command"));
    }
    if (management_command == ManagementCommand::start
        && plan.disposition == TransitionDisposition::allowed) {
        auto physical_memory = memory_.query();
        if (!physical_memory.ok()) {
            return Result<ServiceResponse>::failure(physical_memory.error());
        }
        if (physical_memory.value().total_bytes == 0
            || physical_memory.value().available_bytes
                > physical_memory.value().total_bytes) {
            return Result<ServiceResponse>::failure(service_error(
                ServiceCoreError::dependency_failure,
                L"service_core.memory_preflight",
                L"The physical-memory observation is invalid"));
        }
    }
    std::optional<RecoveryOperationIntent> recovery_intent;
    if (repair != nullptr) {
        recovery_intent = RecoveryOperationIntent{
            RecoveryOperationKind::repair_profile,
            repair->profile_id,
            {},
            std::nullopt,
            std::nullopt,
            true};
    } else if (rebind != nullptr) {
        recovery_intent = RecoveryOperationIntent{
            RecoveryOperationKind::rebind,
            rebind->profile_id,
            rebind->bundle,
            rebind->expected_image_id,
            std::nullopt,
            false};
    } else if (forget != nullptr) {
        recovery_intent = RecoveryOperationIntent{
            RecoveryOperationKind::forget,
            forget->profile_id,
            {},
            std::nullopt,
            forget->confirmation_id,
            false};
    }
    auto operation_id = ids_.next();
    if (!operation_id.ok()) {
        return Result<ServiceResponse>::failure(operation_id.error());
    }
    const auto gate = plan_gate_access(
        profile.value().operation_gate, plan, operation_id.value());
    if (!gate.accepted()) {
        return Result<ServiceResponse>::failure(service_error(
            ServiceCoreError::state_rejected,
            L"service_core.operation_gate",
            L"Another exclusive profile operation is active"));
    }
    auto admission = operations_.admit(
        operation_request,
        operation_id.value());
    if (!admission.ok()) {
        return Result<ServiceResponse>::failure(admission.error());
    }

    if (!admission.value().replayed) {
        if (recovery_intent.has_value()
            && recovery_intent->kind != RecoveryOperationKind::forget) {
            const RecoveryCommandReservation proposed{
                operation_id.value(),
                request_id,
                recovery_intent.value(),
                plan.before,
                profile.value().record};
            auto reserved = profiles_.reserve_recovery_command(
                profile.value(), proposed);
            if (!reserved.ok()) {
                auto observed = profiles_.find_recovery_command(
                    operation_id.value());
                if (!observed.ok()) {
                    return Result<ServiceResponse>::failure(
                        observed.error());
                }
                if (!observed.value().has_value()) {
                    auto terminalized = terminalize_unowned_admission(
                        admission.value().record,
                        L"service_core.recovery_reservation_not_committed");
                    if (!terminalized.ok()) {
                        return Result<ServiceResponse>::failure(
                            terminalized.error());
                    }
                    return Result<ServiceResponse>::failure(
                        reserved.error());
                }
                reserved = Result<RecoveryCommandReservation>::success(
                    observed.value().value());
            }
            if (reserved.value() != proposed) {
                return Result<ServiceResponse>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.recovery_reservation",
                    L"The durable recovery reservation does not match the admitted operation"));
            }
        }
        if (plan.disposition == TransitionDisposition::allowed
            && management_command == ManagementCommand::checkpoint) {
            auto checkpoint_state = profile.value().checkpoint;
            checkpoint_state.phase = CheckpointDurablePhase::admitted;
            checkpoint_state.before_state = plan.before;
            checkpoint_state.correlation.reset();
            checkpoint_state.disposition.reset();
            checkpoint_state.failure.reset();
            checkpoint_state.reconciliation_failure.reset();
            auto published = profiles_.publish_checkpoint(
                profile.value(), plan.admitted, gate.after,
                checkpoint_state);
            if (!published.ok()) {
                auto latest_profile = profiles_.load(
                    profile.value().record.profile.profile_uuid);
                auto intended = profile.value();
                intended.state = plan.admitted;
                intended.operation_gate = gate.after;
                intended.checkpoint = checkpoint_state;
                if (latest_profile.ok()
                    && latest_profile.value() == intended) {
                    published = Result<ManagedProfileSnapshot>::success(
                        latest_profile.move_value());
                } else if (latest_profile.ok()
                    && latest_profile.value().operation_gate
                        .active_operation_id.has_value()
                    && latest_profile.value().operation_gate
                            .active_operation_id.value()
                        != admission.value().record.operation_id
                    && latest_profile.value().operation_gate.active_command
                        == ManagementCommand::checkpoint) {
                    auto active = operations_.load(
                        latest_profile.value().operation_gate
                            .active_operation_id.value());
                    if (active.ok()
                        && !is_terminal(active.value().state)
                        && active.value().profile_id
                            == profile.value().record.profile.profile_uuid
                        && active.value().command_scope
                            == "disk.checkpoint") {
                        auto superseded = admission.value().record;
                        ++superseded.revision;
                        superseded.state = OperationState::failed;
                        superseded.phase = "terminal";
                        superseded.updated_unix_ms = (std::max)(
                            superseded.updated_unix_ms,
                            clock_.unix_time_ms());
                        superseded.terminal_unix_ms =
                            superseded.updated_unix_ms;
                        superseded.cancellable = false;
                        superseded.terminal_result = {
                            std::byte{'c'}, std::byte{'o'}, std::byte{'a'},
                            std::byte{'l'}, std::byte{'e'}, std::byte{'s'},
                            std::byte{'c'}, std::byte{'e'}, std::byte{'d'}};
                        auto terminalized = operations_.write(
                            admission.value().record.revision,
                            superseded);
                        if (!terminalized.ok()) {
                            return Result<ServiceResponse>::failure(
                                terminalized.error());
                        }
                        return Result<ServiceResponse>::success(
                            ServiceResponse{
                                request_id,
                                command,
                                queued_response_disposition(request),
                                active.value().operation_id,
                                active.move_value(),
                                std::nullopt,
                                std::nullopt,
                                {}});
                    }
                } else if (latest_profile.ok()
                           && latest_profile.value().operation_gate
                                   .active_operation_id
                               == std::optional<product::Uuid>(
                                   admission.value().record.operation_id)) {
                    return Result<ServiceResponse>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.checkpoint_admission_resolution",
                        L"The checkpoint profile publication is contradictory"));
                }
                if (!published.ok()) {
                    return Result<ServiceResponse>::failure(
                        published.error());
                }
            }
            profile.value() = published.move_value();
        } else if (plan.disposition == TransitionDisposition::allowed
                   && management_command == ManagementCommand::compact) {
            auto compaction_state = profile.value().compaction;
            compaction_state.phase =
                plan.before.disk.persistence
                        == product::PersistenceState::dirty
                    ? CompactionDurablePhase::checkpointing
                    : CompactionDurablePhase::admitted;
            compaction_state.before_state = plan.before;
            compaction_state.correlation.reset();
            compaction_state.disposition.reset();
            compaction_state.metrics.reset();
            compaction_state.failure.reset();
            compaction_state.reconciliation_failure.reset();
            compaction_state.preflight_failure.reset();
            compaction_state.prerequisite_checkpoint_performed =
                plan.before.disk.persistence
                    == product::PersistenceState::dirty;
            compaction_state.prerequisite_checkpoint_generation =
                profile.value().checkpoint.confirmed_generation;
            compaction_state.prerequisite_checkpoint_disposition.reset();
            auto checkpoint_state = profile.value().checkpoint;
            if (plan.before.disk.persistence
                == product::PersistenceState::dirty) {
                checkpoint_state.phase = CheckpointDurablePhase::admitted;
                checkpoint_state.before_state = plan.before;
                checkpoint_state.correlation.reset();
                checkpoint_state.disposition.reset();
                checkpoint_state.failure.reset();
                checkpoint_state.reconciliation_failure.reset();
            }
            auto published = profiles_.publish_compaction(
                profile.value(), plan.admitted, gate.after,
                checkpoint_state, compaction_state);
            if (!published.ok()) {
                return Result<ServiceResponse>::failure(published.error());
            }
            profile.value() = published.move_value();
        } else if (plan.disposition == TransitionDisposition::allowed
                   && management_command == ManagementCommand::start
                   && plan_performs_checkpoint(plan)
                   && plan_before_can_be_checkpoint_before_state(plan)) {
            // A `disk start` planned from the provisioning lifecycle performs
            // the profile's first checkpoint as one of its own actions, so this
            // admission is also that checkpoint's -- exactly as it is for
            // `disk stop` with a checkpoint-required policy and for
            // `disk compact` on a dirty disk above.
            //
            // It has to happen here because the commit boundary is where the
            // record is read: the boundary proposes `commit_boundary`, and the
            // durable-state validator refuses a `commit_boundary` record whose
            // `before_state` is absent.  Without this admission the boundary
            // publish is refused, the first checkpoint can never commit, and
            // the operation stops at the boundary still holding the profile's
            // operation gate.
            //
            // Only `start` is admitted here.  `disk create` without `--no-start`
            // performs the same first checkpoint, but it is planned from the
            // absent profile state, so its `before_state` cannot be `plan.before`
            // (the guard above) and its admission belongs to the registration
            // path, not this one; it is the same class of gap and is not
            // exercised by the acceptance scenarios.
            // Only the checkpoint is admitted here.  The profile's own state and
            // gate transition belong to the start path below, which publishes
            // them once for every start; moving them here as well would claim
            // the runtime is up before the worker that provides it exists, and
            // would publish the same transition twice.
            auto checkpoint_state = profile.value().checkpoint;
            checkpoint_state.phase = CheckpointDurablePhase::admitted;
            checkpoint_state.before_state = plan.before;
            checkpoint_state.correlation.reset();
            checkpoint_state.disposition.reset();
            checkpoint_state.failure.reset();
            checkpoint_state.reconciliation_failure.reset();
            auto published = profiles_.publish_checkpoint(
                profile.value(), profile.value().state,
                profile.value().operation_gate, checkpoint_state);
            if (!published.ok()) {
                return Result<ServiceResponse>::failure(published.error());
            }
            profile.value() = published.move_value();
        } else if (plan.disposition == TransitionDisposition::allowed
                   && (management_command == ManagementCommand::stop
                       || management_command
                           == ManagementCommand::force_stop)) {
            auto checkpoint_state = profile.value().checkpoint;
            const auto policy =
                profile.value().record.profile.checkpoint.normal_stop;
            if (management_command == ManagementCommand::stop
                && policy == profile::NormalStopPolicy::checkpoint_required
                && plan.before.disk.persistence
                    == product::PersistenceState::dirty) {
                checkpoint_state.phase = CheckpointDurablePhase::admitted;
                checkpoint_state.before_state = plan.before;
                checkpoint_state.correlation.reset();
                checkpoint_state.disposition.reset();
                checkpoint_state.failure.reset();
                checkpoint_state.reconciliation_failure.reset();
            }
            StopDurableState stop_state{
                management_command == ManagementCommand::force_stop
                    ? StopDurablePhase::checkpoint_confirmed
                    : StopDurablePhase::admitted,
                operation_id.value(),
                plan.before,
                stop->target,
                policy,
                management_command == ManagementCommand::force_stop,
                management_command == ManagementCommand::force_stop,
                management_command == ManagementCommand::force_stop,
                profile.value().checkpoint.confirmed_generation,
                profile.value().checkpoint.current_dirty_epoch};
            auto published = profiles_.publish_stop(
                profile.value(), plan.admitted, gate.after,
                checkpoint_state, profile.value().compaction, stop_state);
            if (!published.ok()) {
                auto observed = profiles_.load(
                    profile.value().record.profile.profile_uuid);
                if (!observed.ok()) {
                    return Result<ServiceResponse>::failure(
                        observed.error());
                }
                if (observed.value() == profile.value()) {
                    auto terminalized = terminalize_unowned_admission(
                        admission.value().record,
                        L"service_core.stop_reservation_not_committed");
                    if (!terminalized.ok()) {
                        return Result<ServiceResponse>::failure(
                            terminalized.error());
                    }
                    return Result<ServiceResponse>::failure(
                        published.error());
                }
                const bool exact_commit =
                    observed.value().record == profile.value().record
                    && observed.value().state == plan.admitted
                    && observed.value().operation_gate == gate.after
                    && observed.value().checkpoint == checkpoint_state
                    && observed.value().compaction
                        == profile.value().compaction
                    && observed.value().stop == stop_state;
                if (!exact_commit) {
                    return Result<ServiceResponse>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.stop_reservation_resolution",
                        L"The stop reservation publication is indeterminate"));
                }
                published = Result<ManagedProfileSnapshot>::success(
                    observed.move_value());
            }
            profile.value() = published.move_value();
        } else if (plan.disposition == TransitionDisposition::allowed
                   && management_command == ManagementCommand::forget) {
            const ForgetTombstone proposed{
                operation_id.value(),
                request_id,
                profile.value().record.profile.profile_uuid,
                profile.value().record.profile.image_uuid,
                profile.value().record.revision,
                profile.value().checkpoint.confirmed_generation,
                profile.value().record.profile.bundle_path,
                profile.value().record.profile.owner_sid,
                std::nullopt,
                plan.before,
                ForgetTombstonePhase::reserved};
            auto reserved = profiles_.reserve_forget(
                profile.value(), proposed);
            if (!reserved.ok()) {
                auto observed = profiles_.find_forget_tombstone(
                    operation_id.value());
                if (!observed.ok()) {
                    return Result<ServiceResponse>::failure(
                        observed.error());
                }
                if (!observed.value().has_value()) {
                    auto terminalized = terminalize_unowned_admission(
                        admission.value().record,
                        L"service_core.forget_reservation_not_committed");
                    if (!terminalized.ok()) {
                        return Result<ServiceResponse>::failure(
                            terminalized.error());
                    }
                    return Result<ServiceResponse>::failure(
                        reserved.error());
                }
                reserved = Result<ForgetTombstone>::success(
                    observed.value().value());
            }
            if (!forget_reservation_matches(proposed, reserved.value())) {
                return Result<ServiceResponse>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.forget_reservation",
                    L"The durable forget tombstone does not match the admitted operation"));
            }
        }
        auto admitting = admission.value().record;
        ++admitting.revision;
        admitting.state = OperationState::running;
        admitting.phase = "admitting";
        admitting.updated_unix_ms = (std::max)(
            admitting.updated_unix_ms, clock_.unix_time_ms());
        auto admitting_write = operations_.write(
            admission.value().record.revision, admitting);
        if (!admitting_write.ok()) {
            const bool task20_command =
                management_command == ManagementCommand::stop
                || management_command == ManagementCommand::force_stop
                || management_command == ManagementCommand::rebind
                || management_command == ManagementCommand::repair_profile
                || management_command == ManagementCommand::forget;
            if (!task20_command) {
                return Result<ServiceResponse>::failure(
                    admitting_write.error());
            }
            auto observed = operations_.load(
                admission.value().record.operation_id);
            if (!observed.ok()) {
                return Result<ServiceResponse>::failure(observed.error());
            }
            if (observed.value() == admission.value().record) {
                admitting_write = operations_.write(
                    admission.value().record.revision, admitting);
                if (!admitting_write.ok()) {
                    return Result<ServiceResponse>::failure(
                        admitting_write.error());
                }
            } else if (observed.value() != admitting) {
                return Result<ServiceResponse>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.admitting_resolution",
                    L"The operation admission write is indeterminate"));
            }
        }
        admission.value().record = admitting;
        if (plan.disposition == TransitionDisposition::allowed
            && management_command == ManagementCommand::start) {
            auto published = profiles_.publish_runtime(
                profile.value(), plan.admitted, gate.after);
            if (!published.ok()) {
                return Result<ServiceResponse>::failure(published.error());
            }
            profile.value() = published.move_value();
        } else if (plan.disposition == TransitionDisposition::allowed
                   && (management_command == ManagementCommand::rebind
                       || management_command == ManagementCommand::forget)) {
            auto published = profiles_.publish_runtime(
                profile.value(), plan.admitted, gate.after);
            if (!published.ok()) {
                auto observed = profiles_.load(
                    profile.value().record.profile.profile_uuid);
                if (!observed.ok()) {
                    return Result<ServiceResponse>::failure(observed.error());
                }
                auto intended = profile.value();
                intended.state = plan.admitted;
                intended.operation_gate = gate.after;
                if (observed.value() == profile.value()) {
                    published = profiles_.publish_runtime(
                        profile.value(), plan.admitted, gate.after);
                    if (!published.ok()) {
                        return Result<ServiceResponse>::failure(
                            published.error());
                    }
                } else if (observed.value() == intended) {
                    published = Result<ManagedProfileSnapshot>::success(
                        observed.move_value());
                } else {
                    return Result<ServiceResponse>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recovery_gate_resolution",
                        L"The recovery-operation gate publication is indeterminate"));
                }
            }
            profile.value() = published.move_value();
        }
        {
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                admission.value().record.operation_id,
                request_id,
                plan,
                std::nullopt,
                false,
                std::move(recovery_intent)});
        }
        logger_.write(ServiceLogEvent{
            ServiceLogKind::operation_queued,
            request_id,
            admission.value().record.operation_id,
            profile.value().record.profile.profile_uuid,
            command,
            "queued"});
    }

    ServiceResponse response{
        request_id,
        command,
        queued_response_disposition(request),
        admission.value().record.operation_id,
        admission.value().record,
        std::nullopt,
        std::nullopt,
        warnings_for(plan.disposition)};
    return Result<ServiceResponse>::success(std::move(response));
}

Result<std::optional<product::Uuid>>
ServiceCore::claim_next_pending_operation() {
    std::scoped_lock lock(pending_mutex_);
    for (auto& candidate : pending_) {
        if (candidate.claimed) continue;
        // The scan and the flag that ends it happen under the same lock, so the
        // entry is marked before any other caller can see it.  Two callers
        // racing here get two different operations, never one operation twice.
        candidate.claimed = true;
        return Result<std::optional<product::Uuid>>::success(
            std::optional<product::Uuid>(candidate.operation_id));
    }
    return Result<std::optional<product::Uuid>>::success(
        std::optional<product::Uuid>{});
}

Result<OperationRecord> ServiceCore::execute_claimed_operation(
    const product::Uuid& operation_id) {
    // Only a claim this core handed out may be discharged here.  A caller that
    // never claimed has no evidence it is the operation's one executor, and
    // running the action sequence anyway is exactly the duplication the claim
    // exists to prevent -- so it is refused rather than forwarded.
    {
        std::scoped_lock lock(pending_mutex_);
        const auto pending = std::find_if(
            pending_.begin(), pending_.end(), [&](const auto& candidate) {
                return candidate.operation_id == operation_id;
            });
        if (pending == pending_.end() || !pending->claimed) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.execute_claimed",
                L"The operation was not claimed from this core"));
        }
    }
    return execute_operation(operation_id);
}

Result<OperationRecord> ServiceCore::execute_operation(
    const product::Uuid& operation_id) {
    auto operation = operations_.load(operation_id);
    if (!operation.ok()) return operation;
    if (is_terminal(operation.value().state)) return operation;
    std::optional<PendingExecution> pending_execution;
    {
        std::scoped_lock lock(pending_mutex_);
        const auto pending = std::find_if(
            pending_.begin(), pending_.end(), [&](const auto& candidate) {
                return candidate.operation_id == operation_id;
            });
        if (pending != pending_.end()) pending_execution = *pending;
    }
    const bool lifecycle_scope = operation.value().command_scope
            == "disk.create"
        || operation.value().command_scope == "disk.import";
    if (!pending_execution.has_value() && lifecycle_scope) {
        if (registrations_ == nullptr) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::dependency_failure,
                L"service_core.resume_registration",
                L"Registration persistence is unavailable"));
        }
        auto reservation = registrations_->load_reservation(operation_id);
        if (!reservation.ok()) {
            return Result<OperationRecord>::failure(reservation.error());
        }
        if (!reservation_matches_operation(
                reservation.value(), operation.value())) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.resume_registration",
                L"The lifecycle reservation does not match the operation"));
        }
        auto resumed_plan = plan_lifecycle_reservation(reservation.value());
        if (!resumed_plan.ok()) {
            return Result<OperationRecord>::failure(resumed_plan.error());
        }
        auto admitting = operation.value();
        if (admitting.state == OperationState::queued
            || admitting.state == OperationState::reconciling) {
            ++admitting.revision;
            admitting.state = OperationState::running;
            admitting.phase = "admitting";
            admitting.updated_unix_ms = (std::max)(
                admitting.updated_unix_ms, clock_.unix_time_ms());
            auto written = operations_.write(
                operation.value().revision, admitting);
            if (!written.ok()) {
                return Result<OperationRecord>::failure(written.error());
            }
            operation.value() = admitting;
        } else if (admitting.state != OperationState::running
                   || admitting.phase != "admitting") {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.resume_registration",
                L"The lifecycle operation requires restart reconciliation"));
        }
        pending_execution = PendingExecution{
            .operation_id = admitting.operation_id,
            .request_id = admitting.request_id,
            .plan = resumed_plan.move_value(),
            .lifecycle_intent = reservation.value().intent,
            .restart_recovered = true,
            // Rebuilt by the call that is about to run it, so it is claimed by
            // construction.  Leaving it unclaimed would let a scheduler hand the
            // same operation to a second executor once this one returns.
            .claimed = true};
        {
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(pending_execution.value());
        }
    }
    if (!pending_execution.has_value()
        && operation.value().state == OperationState::queued
        && operation.value().command_scope == "disk.start") {
        auto queued_profile = profiles_.load(operation.value().profile_id);
        if (!queued_profile.ok()) {
            return Result<OperationRecord>::failure(queued_profile.error());
        }
        const auto resumed_plan = plan_command(
            queued_profile.value().state, ManagementCommand::start);
        if (!resumed_plan.accepted()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.resume_plan",
                L"The queued operation can no longer acquire its profile"));
        }
        const auto resumed_gate = plan_gate_access(
            queued_profile.value().operation_gate,
            resumed_plan,
            operation_id);
        if (!resumed_gate.accepted()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.resume_gate",
                L"The queued operation conflicts with the profile gate"));
        }
        if (resumed_plan.disposition == TransitionDisposition::allowed) {
            auto physical_memory = memory_.query();
            if (!physical_memory.ok()) {
                return Result<OperationRecord>::failure(
                    physical_memory.error());
            }
            if (physical_memory.value().total_bytes == 0
                || physical_memory.value().available_bytes
                    > physical_memory.value().total_bytes) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::dependency_failure,
                    L"service_core.resume_memory",
                    L"The physical-memory observation is invalid"));
            }
        }
        auto admitting = operation.value();
        ++admitting.revision;
        admitting.state = OperationState::running;
        admitting.phase = "admitting";
        admitting.updated_unix_ms = (std::max)(
            admitting.updated_unix_ms, clock_.unix_time_ms());
        auto written = operations_.write(
            operation.value().revision, admitting);
        if (!written.ok()) {
            return Result<OperationRecord>::failure(written.error());
        }
        if (resumed_plan.disposition == TransitionDisposition::allowed) {
            auto published = profiles_.publish_runtime(
                queued_profile.value(),
                resumed_plan.admitted,
                resumed_gate.after);
            if (!published.ok()) {
                return Result<OperationRecord>::failure(published.error());
            }
        }
        {
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                .operation_id = admitting.operation_id,
                .request_id = admitting.request_id,
                .plan = resumed_plan,
                // Claimed for the same reason as the lifecycle rebuild above.
                .claimed = true});
        }
        pending_execution = PendingExecution{
            .operation_id = admitting.operation_id,
            .request_id = admitting.request_id,
            .plan = resumed_plan,
            .claimed = true};
        operation.value() = std::move(admitting);
    }
    if (operation.value().state != OperationState::running
        || operation.value().phase != "admitting") {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.execute",
            L"Only a locally admitted operation may begin execution"));
    }
    if (!pending_execution.has_value()) {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.execute",
            L"The durable operation requires restart reconciliation"));
    }
    std::optional<LifecycleRegistrationReservation> lifecycle_reservation;
    if (pending_execution->lifecycle_intent.has_value()) {
        auto loaded = registrations_->load_reservation(operation_id);
        if (!loaded.ok()) {
            return Result<OperationRecord>::failure(loaded.error());
        }
        if (!reservation_matches_operation(
                loaded.value(), operation.value())
            || !lifecycle_intent_equal(
                loaded.value().intent,
                pending_execution->lifecycle_intent.value())) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.lifecycle_reservation",
                L"The in-memory lifecycle plan does not match durable intent"));
        }
        lifecycle_reservation = loaded.move_value();
    }
    std::optional<ManagedProfileSnapshot> current_profile;
    if (!pending_execution->lifecycle_intent.has_value()) {
        auto profile = pending_execution->recovery_intent.has_value()
                && pending_execution->recovery_intent->kind
                    == RecoveryOperationKind::repair_profile
            ? profiles_.load_repair_candidate(operation.value().profile_id)
            : profiles_.load(operation.value().profile_id);
        if (!profile.ok()) {
            return Result<OperationRecord>::failure(profile.error());
        }
        current_profile = profile.move_value();
    } else if (pending_execution->restart_recovered) {
        auto resolution = registrations_->resolve_registration(
            operation_id, operation.value().profile_id);
        if (!resolution.ok()) {
            return Result<OperationRecord>::failure(resolution.error());
        }
        if (resolution.value().disposition
                == RegistrationResolutionDisposition::exact
            && resolution.value().snapshot.has_value()) {
            current_profile = resolution.value().snapshot;
        } else if (resolution.value().disposition
                       != RegistrationResolutionDisposition::absent
                   || resolution.value().snapshot.has_value()) {
            return finalize_operation(
                operation.value(), current_profile,
                pending_execution->plan,
                CompletionOutcome::commit_indeterminate,
                OperationState::failed,
                {std::byte{'r'}, std::byte{'e'}, std::byte{'c'},
                 std::byte{'o'}, std::byte{'v'}, std::byte{'e'},
                 std::byte{'r'}},
                true);
        }
    }
    if (pending_execution->restart_recovered
        && lifecycle_reservation.has_value()
        && !current_profile.has_value()) {
        const auto& reservation = lifecycle_reservation.value();
        const bool evidence_complete = reservation.bundle.has_value()
            && valid_bundle_evidence(
                reservation.intent, reservation.bundle.value())
            && (reservation.intent.kind == LifecycleOperationKind::create
                || reservation.volume.has_value());
        if (reservation.phase != LifecycleReservationPhase::reserved
            && !evidence_complete) {
            return finalize_operation(
                operation.value(), current_profile,
                pending_execution->plan,
                CompletionOutcome::needs_attention_failure,
                OperationState::failed,
                {std::byte{'e'}, std::byte{'v'}, std::byte{'i'},
                 std::byte{'d'}, std::byte{'e'}, std::byte{'n'},
                 std::byte{'c'}, std::byte{'e'}});
        }
    }
    const auto& pending_plan = pending_execution->plan;
    if (pending_execution->recovery_intent.has_value()
        && pending_execution->recovery_intent->kind
            == RecoveryOperationKind::repair_profile
        && current_profile.has_value()) {
        const auto repair_gate = plan_gate_access(
            current_profile->operation_gate, pending_plan, operation_id);
        if (!repair_gate.accepted()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::state_rejected,
                L"service_core.repair_gate",
                L"The repair candidate cannot acquire its operation gate"));
        }
        current_profile->state = pending_plan.admitted;
        current_profile->operation_gate = repair_gate.after;
    }
    const bool normal_stop =
        pending_plan.command == ManagementCommand::stop;
    const bool force_stop =
        pending_plan.command == ManagementCommand::force_stop;
    const bool stop_operation = normal_stop || force_stop;
    if (stop_operation
        && pending_plan.disposition == TransitionDisposition::allowed
        && (!current_profile.has_value()
            || current_profile->stop.operation_id
                != std::optional<product::Uuid>(operation_id)
            || !current_profile->stop.before_state.has_value()
            || current_profile->stop.force != force_stop
            || current_profile->stop.volatile_data_loss_acknowledged
                != force_stop
            || current_profile->stop.phase == StopDurablePhase::stable)) {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.stop_state",
            L"The durable normal-stop reservation is missing or contradictory"));
    }
    const bool idempotent_noop = pending_plan.disposition
        != TransitionDisposition::allowed;
    if (!idempotent_noop
        && current_profile.has_value()
        && current_profile->operation_gate.active_operation_id
            != std::optional<product::Uuid>(operation_id)) {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.execute",
            L"The profile operation gate does not own this operation"));
    }

    bool recovering_checkpoint_from_m3 = false;
    const bool restarting_prerequisite_checkpoint =
        pending_execution->restart_recovered
        && pending_plan.command == ManagementCommand::compact
        && current_profile.has_value()
        && current_profile->compaction.phase
            == CompactionDurablePhase::checkpointing;
    const bool restarting_stop_checkpoint =
        pending_execution->restart_recovered
        && normal_stop
        && current_profile.has_value()
        && current_profile->stop.phase == StopDurablePhase::resolving
        && current_profile->checkpoint.correlation.has_value();
    if (pending_execution->restart_recovered
        && (pending_plan.command == ManagementCommand::checkpoint
            || restarting_prerequisite_checkpoint
            || restarting_stop_checkpoint)
        && current_profile.has_value()) {
        const auto& checkpoint = current_profile->checkpoint;
        if (checkpoint.phase == CheckpointDurablePhase::stable
            && checkpoint.correlation.has_value()
            && checkpoint.disposition.has_value()) {
            if (restarting_prerequisite_checkpoint) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_checkpoint",
                    L"The prerequisite checkpoint is terminal but the parent compaction was not advanced atomically"));
            }
            if (!known_checkpoint_disposition(
                    checkpoint.disposition.value())) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_checkpoint_terminal",
                    L"The final checkpoint disposition is unknown"));
            }
            const auto& correlation = checkpoint.correlation.value();
            if (correlation.product_operation_id != operation_id
                || correlation.profile_id != operation.value().profile_id
                || correlation.image_id
                    != current_profile->record.profile.image_uuid) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_checkpoint_terminal",
                    L"The final checkpoint truth has changed identity"));
            }
            const bool committed = checkpoint.disposition.value()
                == CheckpointCommitDisposition::committed;
            if (committed
                && checkpoint.confirmed_generation
                    != correlation.candidate_generation) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_checkpoint_terminal",
                    L"The final checkpoint generation is contradictory"));
            }
            const auto final_completion = committed
                ? (current_profile->state.disk.persistence
                            == product::PersistenceState::clean
                       ? CompletionOutcome::succeeded_clean
                       : CompletionOutcome::succeeded_dirty)
                : (current_profile->state.disk.persistence
                            == product::PersistenceState::persistence_blocked
                       ? CompletionOutcome::persistence_blocked
                       : CompletionOutcome::transient_failure);
            return finalize_operation(
                operation.value(),
                current_profile,
                pending_plan,
                final_completion,
                committed ? OperationState::succeeded
                          : OperationState::failed,
                checkpoint_terminal_result(
                    correlation,
                    checkpoint.disposition.value(),
                    checkpoint.confirmed_generation,
                    checkpoint.current_dirty_epoch,
                    checkpoint.failure.has_value()
                        ? &checkpoint.failure.value() : nullptr));
        }
        recovering_checkpoint_from_m3 =
            checkpoint.phase != CheckpointDurablePhase::admitted;
        if (recovering_checkpoint_from_m3
            && !checkpoint.correlation.has_value()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.resume_checkpoint",
                L"A post-boundary checkpoint has no durable correlation"));
        }
    }

    bool prerequisite_checkpoint_ready_for_compaction = false;
    bool recovering_compaction_from_m3 = false;
    if (pending_execution->restart_recovered
        && pending_plan.command == ManagementCommand::compact
        && current_profile.has_value()) {
        const auto& compaction = current_profile->compaction;
        if (compaction.phase == CompactionDurablePhase::stable
            && compaction.prerequisite_checkpoint_performed
            && compaction.prerequisite_checkpoint_disposition
                == std::optional<CheckpointCommitDisposition>(
                    CheckpointCommitDisposition::not_committed)) {
            const auto& checkpoint = current_profile->checkpoint;
            if (compaction.prerequisite_checkpoint_generation
                    != checkpoint.confirmed_generation
                || checkpoint.phase != CheckpointDurablePhase::stable
                || checkpoint.disposition
                    != std::optional<CheckpointCommitDisposition>(
                        CheckpointCommitDisposition::not_committed)
                || !checkpoint.correlation.has_value()
                || !checkpoint.failure.has_value()
                || checkpoint.correlation->product_operation_id
                    != operation_id
                || checkpoint.correlation->profile_id
                    != operation.value().profile_id
                || checkpoint.correlation->image_id
                    != current_profile->record.profile.image_uuid
                || checkpoint.correlation->prior_generation
                    != checkpoint.confirmed_generation
                || operation.value().cancellable) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_checkpoint",
                    L"The failed prerequisite checkpoint truth is incomplete or contradictory"));
            }
            const auto final_completion =
                current_profile->state.disk.persistence
                        == product::PersistenceState::persistence_blocked
                    ? CompletionOutcome::persistence_blocked
                    : CompletionOutcome::transient_failure;
            return finalize_operation(
                operation.value(), current_profile, pending_plan,
                final_completion, OperationState::failed,
                checkpoint_terminal_result(
                    checkpoint.correlation.value(),
                    CheckpointCommitDisposition::not_committed,
                    checkpoint.confirmed_generation,
                    checkpoint.current_dirty_epoch,
                    &checkpoint.failure.value()));
        }
        if (compaction.phase == CompactionDurablePhase::stable
            && compaction.preflight_failure.has_value()) {
            const auto& preflight = compaction.preflight_failure.value();
            const WorkerActionRequest recovery_request{
                operation.value().request_id,
                operation_id,
                operation.value().profile_id,
                pending_plan.command,
                ActionKind::compact,
                std::nullopt,
                current_profile->record};
            const bool prerequisite_valid =
                !compaction.prerequisite_checkpoint_performed
                || (compaction.prerequisite_checkpoint_disposition
                        == std::optional<CheckpointCommitDisposition>(
                            CheckpointCommitDisposition::committed)
                    && compaction.prerequisite_checkpoint_generation
                        == current_profile->checkpoint.confirmed_generation
                    && compaction.last_counted_checkpoint_operation_id
                        == std::optional<product::Uuid>(operation_id)
                    && current_profile->checkpoint.phase
                        == CheckpointDurablePhase::stable
                    && current_profile->checkpoint.disposition
                        == std::optional<CheckpointCommitDisposition>(
                            CheckpointCommitDisposition::committed)
                    && current_profile->checkpoint.correlation.has_value()
                    && current_profile->checkpoint.correlation
                            ->product_operation_id
                        == operation_id);
            const bool persistence_matches =
                preflight.future_checkpoint_possible
                ? current_profile->state.disk.persistence
                    != product::PersistenceState::persistence_blocked
                : current_profile->state.disk.persistence
                    == product::PersistenceState::persistence_blocked;
            if (!valid_compaction_preflight_failure(
                    preflight, recovery_request, current_profile.value())
                || compaction.correlation.has_value()
                || compaction.metrics.has_value()
                || compaction.failure.has_value()
                || compaction.reconciliation_failure.has_value()
                || compaction.disposition
                    != std::optional<CompactionCommitDisposition>(
                        CompactionCommitDisposition::not_committed)
                || !prerequisite_valid
                || !persistence_matches
                || operation.value().cancellable
                    == compaction.prerequisite_checkpoint_performed) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_preflight",
                    L"The durable low-space compaction proof is incomplete or contradictory"));
            }
            return finalize_operation(
                operation.value(), current_profile, pending_plan,
                preflight.future_checkpoint_possible
                    ? CompletionOutcome::compaction_deferred
                    : CompletionOutcome::persistence_blocked,
                OperationState::failed,
                compaction_preflight_terminal_result(preflight));
        }
        if (compaction.phase
            == CompactionDurablePhase::checkpoint_confirmed) {
            const auto& checkpoint = current_profile->checkpoint;
            if (!compaction.prerequisite_checkpoint_performed
                || compaction.prerequisite_checkpoint_disposition
                    != std::optional<CheckpointCommitDisposition>(
                        CheckpointCommitDisposition::committed)
                || compaction.prerequisite_checkpoint_generation
                    != checkpoint.confirmed_generation
                || compaction.last_counted_checkpoint_operation_id
                    != std::optional<product::Uuid>(operation_id)
                || checkpoint.phase != CheckpointDurablePhase::stable
                || checkpoint.disposition
                    != std::optional<CheckpointCommitDisposition>(
                        CheckpointCommitDisposition::committed)
                || !checkpoint.correlation.has_value()
                || checkpoint.correlation->product_operation_id
                    != operation_id
                || checkpoint.correlation->profile_id
                    != operation.value().profile_id
                || checkpoint.correlation->image_id
                    != current_profile->record.profile.image_uuid
                || checkpoint.correlation->candidate_generation
                    != checkpoint.confirmed_generation
                || operation.value().cancellable) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_checkpoint",
                    L"The confirmed prerequisite checkpoint truth is incomplete or contradictory"));
            }
            prerequisite_checkpoint_ready_for_compaction = true;
        }
        if (compaction.phase == CompactionDurablePhase::stable
            && compaction.correlation.has_value()
            && compaction.disposition.has_value()) {
            if (!known_compaction_disposition(
                    compaction.disposition.value())) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_terminal",
                    L"The final compaction disposition is unknown"));
            }
            const auto& correlation = compaction.correlation.value();
            if (correlation.product_operation_id != operation_id
                || correlation.profile_id != operation.value().profile_id
                || correlation.image_id
                    != current_profile->record.profile.image_uuid
                || !compaction.metrics.has_value()) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_terminal",
                    L"The final compaction truth has changed identity"));
            }
            const bool committed = compaction.disposition.value()
                == CompactionCommitDisposition::committed;
            if (committed
                && (current_profile->checkpoint.confirmed_generation
                        != correlation.candidate_generation
                    || compaction
                            .last_published_compaction_operation_id
                        != std::optional<product::Uuid>(operation_id))) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.resume_compaction_terminal",
                    L"The final compaction generation or publication marker is contradictory"));
            }
            const auto final_completion = committed
                ? (current_profile->state.disk.persistence
                            == product::PersistenceState::clean
                       ? CompletionOutcome::succeeded_clean
                       : CompletionOutcome::succeeded_dirty)
                : (current_profile->state.disk.persistence
                            == product::PersistenceState::persistence_blocked
                       ? CompletionOutcome::persistence_blocked
                       : CompletionOutcome::transient_failure);
            return finalize_operation(
                operation.value(),
                current_profile,
                pending_plan,
                final_completion,
                committed ? OperationState::succeeded
                          : OperationState::failed,
                compaction_terminal_result(
                    correlation,
                    compaction.disposition.value(),
                    current_profile->checkpoint.confirmed_generation,
                    current_profile->checkpoint.current_dirty_epoch,
                    compaction.metrics.value(),
                    compaction));
        }
        recovering_compaction_from_m3 =
            compaction.phase == CompactionDurablePhase::mutation_boundary
            || compaction.phase
                == CompactionDurablePhase::evidence_recorded
            || compaction.phase == CompactionDurablePhase::resolving
            || compaction.phase
                == CompactionDurablePhase::
                    committed_pending_epoch_confirmation
            || compaction.phase
                == CompactionDurablePhase::product_truth_published;
        if (recovering_compaction_from_m3
            && !compaction.correlation.has_value()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.resume_compaction",
                L"A post-boundary compaction has no durable correlation"));
        }
    }

    if (operation.value().cancel_requested
        && operation.value().cancellable) {
        return finalize_operation(
            operation.value(), current_profile, pending_plan,
            CompletionOutcome::transient_failure,
            OperationState::cancelled,
            {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
             std::byte{'c'}, std::byte{'e'}, std::byte{'l'}});
    }

    const bool revalidating_durable_bundle =
        pending_execution->restart_recovered
        && !current_profile.has_value()
        && lifecycle_reservation.has_value()
        && lifecycle_reservation->bundle.has_value();
    std::vector<ActionKind> execution_actions{
        pending_plan.action_span().begin(), pending_plan.action_span().end()};
    if (recovering_checkpoint_from_m3) {
        execution_actions = pending_plan.command == ManagementCommand::compact
            ? std::vector<ActionKind>{
                ActionKind::checkpoint, ActionKind::compact}
            : normal_stop
            ? std::vector<ActionKind>{
                ActionKind::checkpoint,
                ActionKind::remove_mount,
                ActionKind::remove_exact_device,
                ActionKind::stop_runtime_and_release_lock}
            : std::vector<ActionKind>{ActionKind::checkpoint};
    }
    if (prerequisite_checkpoint_ready_for_compaction) {
        execution_actions = {ActionKind::compact};
    }
    if (recovering_compaction_from_m3) {
        execution_actions = {ActionKind::compact};
    }
    if (revalidating_durable_bundle
        && pending_execution->lifecycle_intent->kind
            == LifecycleOperationKind::create) {
        std::replace(
            execution_actions.begin(), execution_actions.end(),
            ActionKind::create_and_verify_bundle,
            ActionKind::inspect_bundle_read_only);
    }
    if (pending_execution->lifecycle_intent.has_value()
        && current_profile.has_value()) {
        execution_actions.erase(
            std::remove_if(
                execution_actions.begin(), execution_actions.end(),
                [](ActionKind action) {
                    return action == ActionKind::create_and_verify_bundle
                        || action == ActionKind::inspect_bundle_read_only
                        || action
                            == ActionKind::publish_provisioning_profile
                        || action
                            == ActionKind::publish_imported_ready_profile;
                }),
            execution_actions.end());
        if (current_profile->record.profile.lifecycle
                == profile::ProfileLifecycle::ready) {
            const auto& intent =
                pending_execution->lifecycle_intent.value();
            if (intent.kind == LifecycleOperationKind::import
                || !intent.auto_start) {
                execution_actions.clear();
            } else {
                execution_actions.erase(
                    std::remove_if(
                        execution_actions.begin(),
                        execution_actions.end(),
                        [](ActionKind action) {
                            return action != ActionKind::assign_exact_mount
                                && action != ActionKind::readiness_io;
                        }),
                    execution_actions.end());
            }
        }
    }
    if (stop_operation && !idempotent_noop
        && current_profile->stop.phase
            >= StopDurablePhase::checkpoint_confirmed
        && current_profile->stop.phase != StopDurablePhase::resolving) {
        const std::size_t completed_actions = [&]() -> std::size_t {
            switch (current_profile->stop.phase) {
            case StopDurablePhase::checkpoint_confirmed:
            case StopDurablePhase::removal_boundary:
                return normal_stop ? 4U : 0U;
            case StopDurablePhase::mount_removed:
                return normal_stop ? 5U : 1U;
            case StopDurablePhase::device_removed:
                return normal_stop ? 6U : 2U;
            case StopDurablePhase::runtime_stopped:
                return normal_stop ? 7U : 3U;
            default:
                return (std::numeric_limits<std::size_t>::max)();
            }
        }();
        if (completed_actions > execution_actions.size()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.resume_stop_actions",
                L"The durable stop phase cannot map to the canonical action plan"));
        }
        execution_actions.erase(
            execution_actions.begin(),
            execution_actions.begin()
                + static_cast<std::ptrdiff_t>(completed_actions));
    }
    const bool provisioning_start =
        pending_plan.command == ManagementCommand::start
        || (pending_execution->lifecycle_intent.has_value()
            && pending_execution->lifecycle_intent->kind
                == LifecycleOperationKind::create
            && pending_plan.command
                == ManagementCommand::create_and_start);
    // A provisioning start performs the profile's first checkpoint as one of its
    // own actions, and the commit boundary requires that checkpoint to carry a
    // `before_state`: the durable-state validator refuses a `commit_boundary`
    // record without one, and the operation is then abandoned at the boundary
    // with the volume already provisioned and mounted.
    //
    // Which state that is depends on when the checkpoint runs, so the admission
    // is written at the two moments a provisioning profile can come into
    // existence rather than at dispatch.  `disk start` dispatches against a
    // profile that already exists and admits there; `disk create` publishes its
    // profile mid-plan, and a restart can resume it with that publication
    // already behind it.
    const auto admit_first_checkpoint = [&]() -> Result<void> {
        if (!provisioning_start || !current_profile.has_value()
            || current_profile->record.profile.lifecycle
                != profile::ProfileLifecycle::provisioning
            || !plan_performs_checkpoint(pending_plan)
            // A checkpoint still waiting to be decided is the only one that may
            // be admitted.  An already-admitted or in-flight one belongs to the
            // operation that opened it, and re-running this lambda -- which the
            // resumed plan does -- must not reopen it.
            //
            // A *committed* checkpoint is a different matter: it is `stable`
            // again, and a resumed plan that still lists the checkpoint re-runs
            // it.  That one is admitted afresh, exactly as the stop path opens
            // its checkpoint.  The committed generation survives -- this only
            // clears the decision, not the truth -- and the `before_state` it
            // dropped when it committed has to be restored or the boundary
            // cannot be published at all.
            || current_profile->checkpoint.phase
                != CheckpointDurablePhase::stable) {
            return Result<void>::success();
        }
        auto checkpoint_state = current_profile->checkpoint;
        checkpoint_state.phase = CheckpointDurablePhase::admitted;
        checkpoint_state.before_state = current_profile->state;
        checkpoint_state.correlation.reset();
        checkpoint_state.disposition.reset();
        checkpoint_state.failure.reset();
        checkpoint_state.reconciliation_failure.reset();
        auto published = profiles_.publish_checkpoint(
            current_profile.value(), current_profile->state,
            current_profile->operation_gate, checkpoint_state);
        if (!published.ok()) {
            return Result<void>::failure(published.error());
        }
        current_profile = published.move_value();
        return Result<void>::success();
    };
    if (provisioning_start
        && current_profile.has_value()
        && current_profile->record.profile.lifecycle
            == profile::ProfileLifecycle::provisioning) {
        const auto identity_count = recorded_volume_identity_count(
            current_profile->record.profile);
        if (identity_count != 0 && identity_count != 3) {
            return finalize_operation(
                operation.value(), current_profile, pending_plan,
                CompletionOutcome::needs_attention_failure,
                OperationState::failed,
                {std::byte{'i'}, std::byte{'d'}, std::byte{'e'},
                 std::byte{'n'}, std::byte{'t'}, std::byte{'i'},
                 std::byte{'t'}, std::byte{'y'}});
        }
        if (identity_count == 3) {
            // The durable profile already records the complete volume identity,
            // so the provisioning steps are replaced by a verification of the
            // volume that already exists.  Where that verification lands is not
            // arbitrary: it stands in for the partition/format/record steps,
            // which run *after* the worker is launched and the exact device is
            // located.  Inserting it where `prove_new_uninitialized` sits would
            // place it before `create_runtime_and_worker`, and it would then
            // fail for want of a runtime to verify against -- the destructive
            // preflight is dropped outright instead, because a start that is
            // not going to partition has nothing left for it to prove.
            std::vector<ActionKind> reconciled;
            reconciled.reserve(execution_actions.size());
            bool inserted_verification = false;
            const auto insert_verification = [&]() {
                if (inserted_verification) return;
                reconciled.push_back(
                    ActionKind::verify_existing_volume_identity);
                inserted_verification = true;
            };
            for (const auto action : execution_actions) {
                if (action == ActionKind::prove_new_uninitialized) {
                    continue;
                }
                if (action == ActionKind::provision_gpt_ntfs
                    || action == ActionKind::record_volume_identity) {
                    insert_verification();
                    continue;
                }
                // Defensive: a start plan that provisions always carries the
                // two steps above, but the verification must still precede the
                // mount and readiness proof if a plan shape ever changes.
                if (action == ActionKind::assign_exact_mount) {
                    insert_verification();
                }
                reconciled.push_back(action);
            }
            execution_actions = std::move(reconciled);
        }
        // A resumed `disk create` reaches its first checkpoint through this
        // path: the publication is already durable, so its action was dropped
        // from the remaining plan and nothing else would admit the checkpoint.
        auto admitted = admit_first_checkpoint();
        if (!admitted.ok()) {
            return Result<OperationRecord>::failure(admitted.error());
        }
    }

    auto running = operation.value();
    ++running.revision;
    running.phase = "running";
    running.updated_unix_ms = (std::max)(
        running.updated_unix_ms, clock_.unix_time_ms());
    auto running_write = operations_.write(operation.value().revision, running);
    if (!running_write.ok()) {
        return Result<OperationRecord>::failure(running_write.error());
    }
    logger_.write(ServiceLogEvent{
        ServiceLogKind::operation_running,
        running.request_id,
        running.operation_id,
        running.profile_id,
        running.command_scope,
        running.phase});

    CompletionOutcome completion = CompletionOutcome::succeeded_clean;
    std::vector<std::byte> terminal_result;
    bool worker_succeeded = true;
    bool registration_truth_ambiguous = false;
    auto current_operation = running;
    const auto park_checkpoint_reconciliation = [&]()
        -> Result<OperationRecord> {
        if (current_operation.state != OperationState::running) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.park_checkpoint_reconciliation",
                L"Only a running checkpoint may await further M3 proof"));
        }
        auto parked = current_operation;
        ++parked.revision;
        parked.state = OperationState::reconciling;
        parked.phase = "checkpoint-resolving";
        parked.updated_unix_ms = (std::max)(
            parked.updated_unix_ms, clock_.unix_time_ms());
        parked.terminal_unix_ms.reset();
        parked.cancellable = false;
        parked.terminal_result.clear();
        auto written = operations_.write(current_operation.revision, parked);
        if (!written.ok()) {
            return Result<OperationRecord>::failure(written.error());
        }
        {
            std::scoped_lock lock(pending_mutex_);
            const auto pending = std::find_if(
                pending_.begin(), pending_.end(), [&](const auto& candidate) {
                    return candidate.operation_id == parked.operation_id;
                });
            if (pending != pending_.end()) pending_.erase(pending);
        }
        current_operation = parked;
        return Result<OperationRecord>::success(std::move(parked));
    };
    const auto park_compaction_reconciliation = [&]()
        -> Result<OperationRecord> {
        if (current_operation.state != OperationState::running) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.park_compaction_reconciliation",
                L"Only a running compaction may await further M3 proof"));
        }
        auto parked = current_operation;
        ++parked.revision;
        parked.state = OperationState::reconciling;
        parked.phase = "compaction-resolving";
        parked.updated_unix_ms = (std::max)(
            parked.updated_unix_ms, clock_.unix_time_ms());
        parked.terminal_unix_ms.reset();
        parked.cancellable = false;
        parked.terminal_result.clear();
        auto written = operations_.write(current_operation.revision, parked);
        if (!written.ok()) {
            return Result<OperationRecord>::failure(written.error());
        }
        {
            std::scoped_lock lock(pending_mutex_);
            const auto pending = std::find_if(
                pending_.begin(), pending_.end(), [&](const auto& candidate) {
                    return candidate.operation_id == parked.operation_id;
                });
            if (pending != pending_.end()) pending_.erase(pending);
        }
        current_operation = parked;
        return Result<OperationRecord>::success(std::move(parked));
    };
    const auto park_stop_reconciliation = [&]()
        -> Result<OperationRecord> {
        if (current_operation.state != OperationState::running
            || !current_profile.has_value()
            || !stop_operation) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.park_stop_reconciliation",
                L"Only a running normal stop may await teardown proof"));
        }
        auto parked = current_operation;
        ++parked.revision;
        parked.state = OperationState::reconciling;
        parked.phase = "stop-reconciling";
        parked.updated_unix_ms = (std::max)(
            parked.updated_unix_ms, clock_.unix_time_ms());
        parked.terminal_unix_ms.reset();
        parked.cancellable = false;
        parked.terminal_result.clear();
        auto written = operations_.write(current_operation.revision, parked);
        if (!written.ok()) {
            return Result<OperationRecord>::failure(written.error());
        }
        {
            std::scoped_lock lock(pending_mutex_);
            const auto pending = std::find_if(
                pending_.begin(), pending_.end(), [&](const auto& candidate) {
                    return candidate.operation_id == parked.operation_id;
                });
            if (pending != pending_.end()) pending_.erase(pending);
        }
        current_operation = parked;
        return Result<OperationRecord>::success(std::move(parked));
    };
    std::optional<BundleRegistrationEvidence> bundle_evidence;
    std::optional<VolumeRegistrationEvidence> volume_evidence;
    std::optional<BundleRegistrationEvidence> rebind_bundle_evidence;
    std::optional<profile::ProfileRecord> rebind_record;
    bool first_checkpoint_acknowledged = false;
    bool checkpoint_cancelled_before_boundary = false;
    std::optional<CheckpointCommitBoundaryEvidence>
        cancelled_checkpoint_correlation;
    bool compaction_cancelled_before_boundary = false;
    if (force_stop && !idempotent_noop) {
        auto stop_state = current_profile->stop;
        auto checkpoint_state = current_profile->checkpoint;
        auto state = current_profile->state;
        auto live_epoch = worker_.current_dirty_epoch(
            current_operation.profile_id);
        if (live_epoch.ok()) {
            stop_state.dirty_epoch_before = (std::max)(
                stop_state.dirty_epoch_before, live_epoch.value());
            if (live_epoch.value()
                > checkpoint_state.current_dirty_epoch) {
                checkpoint_state.current_dirty_epoch = live_epoch.value();
                state.disk.persistence = product::PersistenceState::dirty;
            }
        }
        auto acknowledged = profiles_.publish_stop(
            current_profile.value(), state,
            current_profile->operation_gate, checkpoint_state,
            current_profile->compaction, stop_state);
        if (!acknowledged.ok()) {
            return Result<OperationRecord>::failure(acknowledged.error());
        }
        current_profile = acknowledged.move_value();
    }
    if (normal_stop && !idempotent_noop
        && (current_profile->stop.phase == StopDurablePhase::admitted
            || current_profile->stop.phase == StopDurablePhase::quiesced
            || current_profile->stop.phase
                == StopDurablePhase::volume_flushed)) {
        const auto publish_stop_truth = [&](StopDurableState stop_state,
                                            ProfileState state,
                                            CheckpointDurableState checkpoint,
                                            CompactionDurableState compaction)
            -> Result<void> {
            auto published = profiles_.publish_stop(
                current_profile.value(), state,
                current_profile->operation_gate, checkpoint, compaction,
                stop_state);
            if (!published.ok()) {
                return Result<void>::failure(published.error());
            }
            current_profile = published.move_value();
            return Result<void>::success();
        };
        const auto execute_stop_action = [&](ActionKind action)
            -> Result<WorkerActionResult> {
            return worker_.execute(WorkerActionRequest{
                current_operation.request_id,
                current_operation.operation_id,
                current_operation.profile_id,
                pending_plan.command,
                action,
                std::nullopt,
                current_profile->record});
        };
        // The write freeze, driven through the same executor every other stop
        // step uses.  The release is not wrapped here: it *is* the
        // `resume_mutations` step, and both rollback paths below already drive it
        // through that executor.
        const auto freeze_writes = [&]() -> Result<WorkerActionResult> {
            return execute_stop_action(ActionKind::freeze_writes);
        };
        const auto rollback_stop = [&](std::vector<std::byte> result)
            -> Result<OperationRecord> {
            auto resumed = execute_stop_action(ActionKind::resume_mutations);
            if (!resumed.ok()
                || !is_successful_completion(resumed.value().completion)) {
                auto unresolved_stop = current_profile->stop;
                unresolved_stop.phase = StopDurablePhase::resolving;
                auto unresolved_state = current_profile->state;
                unresolved_state.disk.health =
                    product::HealthState::needs_attention;
                auto persisted = publish_stop_truth(
                    unresolved_stop, unresolved_state,
                    current_profile->checkpoint,
                    current_profile->compaction);
                if (!persisted.ok()) {
                    return Result<OperationRecord>::failure(
                        persisted.error());
                }
                return park_stop_reconciliation();
            }
            auto rollback_state = current_profile->stop.before_state.value();
            rollback_state.disk.persistence =
                current_profile->state.disk.persistence;
            auto rollback_checkpoint = current_profile->checkpoint;
            if (rollback_checkpoint.phase == CheckpointDurablePhase::admitted
                && !rollback_checkpoint.correlation.has_value()) {
                rollback_checkpoint.phase = CheckpointDurablePhase::stable;
                rollback_checkpoint.before_state.reset();
                rollback_checkpoint.disposition.reset();
                rollback_checkpoint.failure.reset();
                rollback_checkpoint.reconciliation_failure.reset();
            }
            auto persisted = publish_stop_truth(
                StopDurableState{}, rollback_state, rollback_checkpoint,
                current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(persisted.error());
            }
            return finalize_operation(
                current_operation, current_profile, pending_plan,
                CompletionOutcome::transient_failure,
                OperationState::failed, std::move(result));
        };

        auto stop_state = current_profile->stop;
        if (stop_state.phase == StopDurablePhase::admitted) {
            auto quiesced = execute_stop_action(
                ActionKind::quiesce_mutations);
            if (!quiesced.ok()
                || !is_successful_completion(
                    quiesced.value().completion)) {
                return finalize_operation(
                    current_operation, current_profile, pending_plan,
                    CompletionOutcome::transient_failure,
                    OperationState::failed,
                    {std::byte{'q'}, std::byte{'u'}, std::byte{'i'},
                     std::byte{'e'}, std::byte{'s'}, std::byte{'c'},
                     std::byte{'e'}});
            }
            stop_state = current_profile->stop;
            stop_state.phase = StopDurablePhase::quiesced;
            auto persisted = publish_stop_truth(
                stop_state, current_profile->state,
                current_profile->checkpoint, current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(
                    persisted.error());
            }
        }

        // The write freeze.  It runs after the quiesce proved which runtime is
        // being stopped and before anything is flushed or committed, and it is
        // what makes the commit that follows the *last* word on this volume
        // rather than one more snapshot taken while writes continued.  Without
        // it the removal below is refused 25% of the time, because the volume it
        // is refusing over really is dirty again.
        //
        // A failure here is transient and retryable by construction: a busy
        // volume is the only realistic cause and it is an observation, not a
        // broken runtime.  The profile is left running, and the rollback releases
        // nothing because nothing was taken.
        if (stop_state.phase == StopDurablePhase::admitted) {
            auto frozen = freeze_writes();
            if (!frozen.ok()
                || !is_successful_completion(frozen.value().completion)) {
                return finalize_operation(
                    current_operation, current_profile, pending_plan,
                    CompletionOutcome::transient_failure,
                    OperationState::failed,
                    {std::byte{'f'}, std::byte{'r'}, std::byte{'e'}, std::byte{'e'},
                     std::byte{'z'}, std::byte{'e'}});
            }
            stop_state = current_profile->stop;
            stop_state.phase = StopDurablePhase::quiesced;
            auto persisted = publish_stop_truth(
                stop_state, current_profile->state,
                current_profile->checkpoint, current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(
                    persisted.error());
            }
        }

        if (current_profile->stop.phase == StopDurablePhase::quiesced) {
            auto flushed = execute_stop_action(
                ActionKind::flush_exact_volume);
            if (!flushed.ok()
                || !is_successful_completion(flushed.value().completion)) {
                return rollback_stop(
                    {std::byte{'f'}, std::byte{'l'}, std::byte{'u'},
                     std::byte{'s'}, std::byte{'h'}});
            }
            stop_state = current_profile->stop;
            stop_state.phase = StopDurablePhase::volume_flushed;
            auto persisted = publish_stop_truth(
                stop_state, current_profile->state,
                current_profile->checkpoint, current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(
                    persisted.error());
            }
        }

        // The quiesce, the write freeze and the flush have run; all three are
        // now behind the plan.  The checkpoint decision is policy-driven, not
        // guessed from the durable epoch -- that counter is per-process and
        // meaningless across a worker restart.
        execution_actions.erase(
            execution_actions.begin(),
            execution_actions.begin() + 3);
        if (current_profile->stop.policy
                == profile::NormalStopPolicy::checkpoint_required) {
            // Always checkpoint.  The P4/P5 boundary decides commit, cleanliness
            // and failure from the worker's own cut and evidence; the service does
            // not predict clean ahead of it.
            auto checkpoint_state = current_profile->checkpoint;
            checkpoint_state.phase = CheckpointDurablePhase::admitted;
            checkpoint_state.before_state =
                current_profile->stop.before_state;
            checkpoint_state.correlation.reset();
            checkpoint_state.disposition.reset();
            checkpoint_state.failure.reset();
            checkpoint_state.reconciliation_failure.reset();
            auto persisted = publish_stop_truth(
                current_profile->stop, current_profile->state,
                checkpoint_state, current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(persisted.error());
            }
            if (execution_actions.empty()
                || execution_actions.front()
                    != ActionKind::checkpoint_if_required) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.stop_plan",
                    L"The normal-stop checkpoint action is not canonical"));
            }
            execution_actions.front() = ActionKind::checkpoint;
        } else if (current_profile->stop.policy
                == profile::NormalStopPolicy::require_clean) {
            // The runtime must already be clean.  "Clean" is the worker's
            // block-level truth, and being unable to ask is not clean.
            auto observation = worker_.require_runtime_observation(
                current_operation.profile_id);
            if (!observation.ok()) {
                return rollback_stop(
                    {std::byte{'r'}, std::byte{'u'}, std::byte{'n'},
                     std::byte{'t'}, std::byte{'i'}, std::byte{'m'},
                     std::byte{'e'}});
            }
            if (observation.value().dirty) {
                auto observed_state = current_profile->state;
                observed_state.disk.persistence =
                    product::PersistenceState::dirty;
                auto observed_checkpoint = current_profile->checkpoint;
                observed_checkpoint.current_dirty_epoch =
                    observation.value().dirty_epoch;
                auto persisted = publish_stop_truth(
                    current_profile->stop, observed_state,
                    observed_checkpoint, current_profile->compaction);
                if (!persisted.ok()) {
                    return Result<OperationRecord>::failure(persisted.error());
                }
                return rollback_stop(
                    {std::byte{'r'}, std::byte{'e'}, std::byte{'q'},
                     std::byte{'u'}, std::byte{'i'}, std::byte{'r'},
                     std::byte{'e'}, std::byte{'-'}, std::byte{'c'},
                     std::byte{'l'}, std::byte{'e'}, std::byte{'a'},
                     std::byte{'n'}});
            }
            auto confirmed_stop = current_profile->stop;
            confirmed_stop.phase = StopDurablePhase::checkpoint_confirmed;
            auto persisted = publish_stop_truth(
                confirmed_stop, current_profile->state,
                current_profile->checkpoint, current_profile->compaction);
            if (!persisted.ok()) {
                return Result<OperationRecord>::failure(persisted.error());
            }
            if (execution_actions.empty()
                || execution_actions.front()
                    != ActionKind::checkpoint_if_required) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.stop_plan",
                    L"The normal-stop checkpoint decision is not canonical"));
            }
            execution_actions.erase(execution_actions.begin());
        } else {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.stop_policy",
                L"The persisted normal-stop policy is invalid"));
        }
    }
    for (const auto action : execution_actions) {
        const bool service_publishes_action =
            action == ActionKind::publish_provisioning_profile
            || action == ActionKind::publish_imported_ready_profile
            || action == ActionKind::publish_ready_profile
            || action == ActionKind::publish_rebind
            || action == ActionKind::repair_profile_from_previous
            || action == ActionKind::forget_metadata;
        auto observed = operations_.load(operation_id);
        if (!observed.ok()) {
            return Result<OperationRecord>::failure(observed.error());
        }
        if (is_terminal(observed.value().state)) return observed;
        if (observed.value().revision != current_operation.revision) {
            if (observed.value().revision != current_operation.revision + 1
                || !observed.value().cancel_requested
                || !observed.value().cancellable) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.observe_operation",
                    L"The durable operation changed unexpectedly"));
            }
            current_operation = observed.move_value();
        }
        if (current_operation.cancel_requested
            && current_operation.cancellable) {
            return finalize_operation(
                current_operation, current_profile, pending_plan,
                CompletionOutcome::transient_failure,
                OperationState::cancelled,
                {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
                 std::byte{'c'}, std::byte{'e'}, std::byte{'l'}});
        }
        const bool stop_removal_boundary = stop_operation
            && action == ActionKind::remove_mount;
        if (stop_removal_boundary && !current_operation.cancellable) {
            if (!current_profile.has_value()
                || current_profile->operation_gate.cancellable
                || current_profile->stop.phase
                    != StopDurablePhase::checkpoint_confirmed) {
                return Result<OperationRecord>::failure(service_error(
                    ServiceCoreError::operation_invalid,
                    L"service_core.stop_removal_boundary",
                    L"The pre-existing noncancellable stop boundary is contradictory"));
            }
            auto stop_state = current_profile->stop;
            stop_state.phase = StopDurablePhase::removal_boundary;
            auto published = profiles_.publish_stop(
                current_profile.value(), current_profile->state,
                current_profile->operation_gate,
                current_profile->checkpoint,
                current_profile->compaction, stop_state);
            if (!published.ok()) {
                return Result<OperationRecord>::failure(published.error());
            }
            current_profile = published.move_value();
        }
        if ((service_publishes_action
             || worker_.begins_noncancellable(action)
             || stop_removal_boundary)
            && current_operation.cancellable) {
            auto proposed = current_operation;
            ++proposed.revision;
            proposed.cancellable = false;
            proposed.phase = "noncancellable";
            proposed.updated_unix_ms = (std::max)(
                proposed.updated_unix_ms, clock_.unix_time_ms());
            auto written = operations_.write(
                current_operation.revision, proposed);
            if (!written.ok()) {
                auto latest = operations_.load(operation_id);
                if (!latest.ok()
                    || latest.value().revision != proposed.revision) {
                    return Result<OperationRecord>::failure(written.error());
                }
                if (latest.value().cancel_requested
                    && latest.value().cancellable) {
                    return finalize_operation(
                        latest.value(),
                        current_profile,
                        pending_plan,
                        CompletionOutcome::transient_failure,
                        OperationState::cancelled,
                        {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
                         std::byte{'c'}, std::byte{'e'}, std::byte{'l'}});
                }
                if (latest.value().cancellable
                    || latest.value().cancel_requested
                    || latest.value().phase != "noncancellable") {
                    return Result<OperationRecord>::failure(written.error());
                }
                current_operation = latest.move_value();
            } else {
                current_operation = std::move(proposed);
            }
            if (current_profile.has_value()) {
                const auto noncancellable = mark_operation_noncancellable(
                    current_profile->operation_gate, operation_id);
                if (!noncancellable.accepted()) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.noncancellable",
                        L"The operation gate rejected its noncancellable phase"));
                }
                auto stop_state = current_profile->stop;
                if (stop_removal_boundary) {
                    if (stop_state.phase
                        != StopDurablePhase::checkpoint_confirmed) {
                        return Result<OperationRecord>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.stop_removal_boundary",
                            L"Stop removal began without confirmed persistence truth"));
                    }
                    stop_state.phase = StopDurablePhase::removal_boundary;
                }
                const bool repair_publication =
                    action == ActionKind::repair_profile_from_previous
                    && pending_execution->recovery_intent.has_value()
                    && pending_execution->recovery_intent->kind
                        == RecoveryOperationKind::repair_profile;
                if (repair_publication) {
                    current_profile->operation_gate = noncancellable.after;
                } else {
                    auto published = stop_removal_boundary
                        ? profiles_.publish_stop(
                            current_profile.value(), current_profile->state,
                            noncancellable.after,
                            current_profile->checkpoint,
                            current_profile->compaction, stop_state)
                        : profiles_.publish_runtime(
                            current_profile.value(), current_profile->state,
                            noncancellable.after);
                    if (!published.ok()) {
                        return Result<OperationRecord>::failure(
                            published.error());
                    }
                    current_profile = published.move_value();
                }
            }
        }
        if (pending_execution->lifecycle_intent.has_value()) {
            if (!revalidating_durable_bundle
                && (action == ActionKind::create_and_verify_bundle
                    || action == ActionKind::inspect_bundle_read_only)) {
                auto proposed_reservation = lifecycle_reservation.value();
                proposed_reservation.phase =
                    action == ActionKind::create_and_verify_bundle
                    ? LifecycleReservationPhase::bundle_action_started
                    : LifecycleReservationPhase::inspection_action_started;
                auto persisted = persist_reservation_transition(
                    *registrations_, lifecycle_reservation.value(),
                    std::move(proposed_reservation));
                if (!persisted.ok()) {
                    worker_succeeded = false;
                    registration_truth_ambiguous = true;
                    completion = CompletionOutcome::commit_indeterminate;
                    terminal_result = {
                        std::byte{'e'}, std::byte{'v'}, std::byte{'i'},
                        std::byte{'d'}, std::byte{'e'}, std::byte{'n'},
                        std::byte{'c'}, std::byte{'e'}};
                    break;
                }
                lifecycle_reservation = persisted.move_value();
            }
            auto phase = current_operation;
            ++phase.revision;
            phase.phase = lifecycle_action_phase(action);
            phase.updated_unix_ms = (std::max)(
                phase.updated_unix_ms, clock_.unix_time_ms());
            auto written = operations_.write(
                current_operation.revision, phase);
            if (!written.ok()) {
                return Result<OperationRecord>::failure(written.error());
            }
            current_operation = std::move(phase);
        }
        WorkerActionResult worker_result{
            CompletionOutcome::succeeded_clean,
            {}};
        if (!service_publishes_action) {
            if (!current_profile.has_value()
                && !pending_execution->lifecycle_intent.has_value()) {
                auto refreshed = profiles_.load(
                    current_operation.profile_id);
                if (!refreshed.ok()) {
                    return Result<OperationRecord>::failure(
                        refreshed.error());
                }
                current_profile = refreshed.move_value();
            }
            const WorkerActionRequest worker_request{
                current_operation.request_id,
                current_operation.operation_id,
                current_operation.profile_id,
                pending_plan.command,
                action,
                pending_execution->lifecycle_intent,
                current_profile.has_value()
                    ? std::optional<profile::ProfileRecord>(
                        current_profile->record)
                    : std::nullopt,
                pending_execution->recovery_intent};
            Result<WorkerActionResult> result =
                action == ActionKind::checkpoint
                    && (pending_plan.command == ManagementCommand::checkpoint
                        || pending_plan.command
                            == ManagementCommand::compact
                        || normal_stop
                        // A provisioning profile has no committed Windows
                        // volume generation until this first checkpoint.  It
                        // must therefore take the same prepare/commit/abort
                        // boundary as every other real checkpoint; routing it
                        // through execute() would fail closed as unrealized.
                        || provisioning_start)
                ? (recovering_checkpoint_from_m3
                    ? worker_.reconcile_checkpoint(
                        worker_request,
                        current_profile->checkpoint.correlation.value())
                    : worker_.execute_checkpoint(
                    worker_request,
                    [&](const CheckpointCommitBoundaryEvidence& evidence)
                        -> Result<CheckpointCommitBoundaryDecision> {
                        if (!current_profile.has_value()
                            || !valid_checkpoint_boundary(
                                evidence, worker_request,
                                current_profile.value())) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.checkpoint_boundary",
                                    L"The worker checkpoint correlation is invalid"));
                        }
                        auto observed_boundary = operations_.load(
                            operation_id);
                        if (!observed_boundary.ok()) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                observed_boundary.error());
                        }
                        if (observed_boundary.value().revision
                                == current_operation.revision + 1
                            && observed_boundary.value().cancel_requested
                            && observed_boundary.value().cancellable
                            && !is_terminal(
                                observed_boundary.value().state)) {
                            current_operation =
                                observed_boundary.move_value();
                            checkpoint_cancelled_before_boundary = true;
                            cancelled_checkpoint_correlation = evidence;
                            return Result<
                                CheckpointCommitBoundaryDecision>::success(
                                {false,
                                 current_operation.revision,
                                 current_profile->record.revision});
                        }
                        if (observed_boundary.value().revision
                                != current_operation.revision
                            || is_terminal(observed_boundary.value().state)) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.checkpoint_boundary",
                                    L"The durable operation changed before the checkpoint boundary"));
                        }
                        if (observed_boundary.value().cancel_requested
                            && observed_boundary.value().cancellable) {
                            current_operation =
                                observed_boundary.move_value();
                            checkpoint_cancelled_before_boundary = true;
                            cancelled_checkpoint_correlation = evidence;
                            return Result<
                                CheckpointCommitBoundaryDecision>::success(
                                {false,
                                 current_operation.revision,
                                 current_profile->record.revision});
                        }
                        auto boundary_operation =
                            observed_boundary.value();
                        ++boundary_operation.revision;
                        boundary_operation.cancellable = false;
                        boundary_operation.phase =
                            "checkpoint-commit-boundary";
                        boundary_operation.updated_unix_ms = (std::max)(
                            boundary_operation.updated_unix_ms,
                            clock_.unix_time_ms());
                        auto boundary_written = operations_.write(
                            observed_boundary.value().revision,
                            boundary_operation);
                        if (!boundary_written.ok()) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                boundary_written.error());
                        }
                        const auto noncancellable =
                            mark_operation_noncancellable(
                                current_profile->operation_gate,
                                operation_id);
                        if (!noncancellable.accepted()) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.checkpoint_boundary",
                                    L"The checkpoint gate rejected its commit boundary"));
                        }
                        auto checkpoint_state =
                            current_profile->checkpoint;
                        checkpoint_state.phase =
                            CheckpointDurablePhase::commit_boundary;
                        checkpoint_state.correlation = evidence;
                        checkpoint_state.disposition.reset();
                        checkpoint_state.failure.reset();
                        checkpoint_state.reconciliation_failure.reset();
                        auto boundary_profile =
                            profiles_.publish_checkpoint(
                                current_profile.value(),
                                current_profile->state,
                                noncancellable.after,
                                checkpoint_state);
                        if (!boundary_profile.ok()) {
                            return Result<
                                CheckpointCommitBoundaryDecision>::failure(
                                boundary_profile.error());
                        }
                        current_operation =
                            std::move(boundary_operation);
                        current_profile =
                            boundary_profile.move_value();
                        return Result<
                            CheckpointCommitBoundaryDecision>::success(
                            {true,
                             current_operation.revision,
                             current_profile->record.revision});
                    }))
                : action == ActionKind::compact
                        && pending_plan.command == ManagementCommand::compact
                ? (recovering_compaction_from_m3
                    ? worker_.reconcile_compaction(
                        worker_request,
                        current_profile->compaction.correlation.value())
                    : worker_.execute_compaction(
                    worker_request,
                    [&](const CompactionMutationBoundaryEvidence& evidence)
                        -> Result<CompactionMutationBoundaryDecision> {
                        if (!current_profile.has_value()
                            || !valid_compaction_boundary(
                                evidence, worker_request,
                                current_profile.value())) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_boundary",
                                    L"The worker compaction correlation or space proof is invalid"));
                        }
                        auto observed_boundary = operations_.load(
                            operation_id);
                        if (!observed_boundary.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                observed_boundary.error());
                        }
                        if (observed_boundary.value().revision
                                == current_operation.revision + 1
                            && observed_boundary.value().cancel_requested
                            && observed_boundary.value().cancellable
                            && !is_terminal(
                                observed_boundary.value().state)) {
                            current_operation =
                                observed_boundary.move_value();
                            compaction_cancelled_before_boundary = true;
                            return Result<
                                CompactionMutationBoundaryDecision>::success(
                                {false,
                                 current_operation.revision,
                                 current_profile->record.revision});
                        }
                        if (observed_boundary.value().revision
                                != current_operation.revision
                            || is_terminal(observed_boundary.value().state)) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_boundary",
                                    L"The durable operation changed before the compaction boundary"));
                        }
                        if (observed_boundary.value().cancel_requested
                            && observed_boundary.value().cancellable) {
                            current_operation =
                                observed_boundary.move_value();
                            compaction_cancelled_before_boundary = true;
                            return Result<
                                CompactionMutationBoundaryDecision>::success(
                                {false,
                                 current_operation.revision,
                                 current_profile->record.revision});
                        }
                        auto boundary_operation =
                            observed_boundary.value();
                        ++boundary_operation.revision;
                        boundary_operation.cancellable = false;
                        boundary_operation.phase =
                            "compaction-mutation-boundary";
                        boundary_operation.updated_unix_ms = (std::max)(
                            boundary_operation.updated_unix_ms,
                            clock_.unix_time_ms());
                        auto boundary_written = operations_.write(
                            observed_boundary.value().revision,
                            boundary_operation);
                        if (!boundary_written.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                boundary_written.error());
                        }
                        const auto noncancellable =
                            mark_operation_noncancellable(
                                current_profile->operation_gate,
                                operation_id);
                        if (!noncancellable.accepted()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_boundary",
                                    L"The compaction gate rejected its mutation boundary"));
                        }
                        auto compaction_state =
                            current_profile->compaction;
                        compaction_state.phase =
                            CompactionDurablePhase::mutation_boundary;
                        compaction_state.correlation = evidence;
                        compaction_state.disposition.reset();
                        compaction_state.metrics.reset();
                        compaction_state.failure.reset();
                        compaction_state.reconciliation_failure.reset();
                        compaction_state.preflight_failure.reset();
                        auto boundary_profile =
                            profiles_.publish_compaction(
                                current_profile.value(),
                                current_profile->state,
                                noncancellable.after,
                                current_profile->checkpoint,
                                compaction_state);
                        if (!boundary_profile.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                boundary_profile.error());
                        }
                        current_operation =
                            std::move(boundary_operation);
                        current_profile =
                            boundary_profile.move_value();
                        return Result<
                            CompactionMutationBoundaryDecision>::success(
                            {true,
                             current_operation.revision,
                             current_profile->record.revision});
                    },
                    [&](const CompactionMutationBoundaryEvidence& evidence)
                        -> Result<CompactionMutationBoundaryDecision> {
                        if (!current_profile.has_value()
                            || !current_profile->compaction.correlation
                                    .has_value()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_cut",
                                    L"The compaction cut has no durable mutation-boundary owner"));
                        }
                        auto expected =
                            current_profile->compaction.correlation.value();
                        if (expected.cut_dirty_epoch_known
                            || !evidence.cut_dirty_epoch_known) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_cut",
                                    L"The compaction cut epoch is missing, stale, or duplicated"));
                        }
                        expected.cut_dirty_epoch = evidence.cut_dirty_epoch;
                        expected.cut_dirty_epoch_known = true;
                        if (evidence != expected) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_cut",
                                    L"The compaction cut changed its durable correlation"));
                        }
                        auto observed_cut = operations_.load(operation_id);
                        if (!observed_cut.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                observed_cut.error());
                        }
                        if (observed_cut.value().revision
                                != current_operation.revision
                            || observed_cut.value().cancellable
                            || observed_cut.value().cancel_requested
                            || is_terminal(observed_cut.value().state)) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_cut",
                                    L"The durable operation changed before cut publication"));
                        }
                        auto cut_operation = observed_cut.value();
                        ++cut_operation.revision;
                        cut_operation.phase = "compaction-consistency-cut";
                        cut_operation.updated_unix_ms = (std::max)(
                            cut_operation.updated_unix_ms,
                            clock_.unix_time_ms());
                        auto cut_written = operations_.write(
                            observed_cut.value().revision, cut_operation);
                        if (!cut_written.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                cut_written.error());
                        }
                        auto compaction_state =
                            current_profile->compaction;
                        compaction_state.correlation = evidence;
                        auto cut_profile = profiles_.publish_compaction(
                            current_profile.value(), current_profile->state,
                            current_profile->operation_gate,
                            current_profile->checkpoint, compaction_state);
                        if (!cut_profile.ok()) {
                            return Result<
                                CompactionMutationBoundaryDecision>::failure(
                                cut_profile.error());
                        }
                        current_operation = std::move(cut_operation);
                        current_profile = cut_profile.move_value();
                        return Result<
                            CompactionMutationBoundaryDecision>::success(
                            {true,
                             current_operation.revision,
                             current_profile->record.revision});
                    }))
                : worker_.execute(worker_request);
            if (!result.ok()) {
                // **The one place the reason is recorded.**  Every branch below turns this
                // failure into a durable *disposition* and a marker token, and none of them
                // carries the cause; by the time the operation is terminal the error is
                // gone.  So it is written here, where it still exists, naming the action
                // and the step that refused.
                {
                    const auto& failed = result.error();
                    auto operation_name = ipc::utf16_to_utf8(failed.operation);
                    auto message = ipc::utf16_to_utf8(failed.message);
                    std::string reason =
                        "action=" + std::string(lifecycle_action_phase(action))
                        + " code=" + std::to_string(failed.code)
                        + " native=" + std::to_string(failed.native_code)
                        + " step="
                        + (operation_name.ok() ? operation_name.value() : "?")
                        + " detail="
                        + (message.ok() ? message.value() : "?");
                    logger_.write(ServiceLogEvent{
                        ServiceLogKind::operation_failed,
                        current_operation.request_id,
                        current_operation.operation_id,
                        current_operation.profile_id,
                        current_operation.command_scope,
                        current_operation.phase,
                        std::move(reason)});
                }
                if (stop_operation && current_profile.has_value()
                    && current_profile->stop.phase
                        >= StopDurablePhase::removal_boundary) {
                    return park_stop_reconciliation();
                }
                if (action == ActionKind::checkpoint
                    && current_profile.has_value()
                    && current_profile->checkpoint.correlation.has_value()) {
                    const auto indeterminate = complete_command(
                        pending_plan,
                        CompletionOutcome::commit_indeterminate);
                    if (!indeterminate.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.resume_checkpoint",
                                L"The missing M3 proof cannot enter attention state"));
                    }
                    auto checkpoint_state =
                        current_profile->checkpoint;
                    checkpoint_state.phase =
                        CheckpointDurablePhase::resolving;
                    checkpoint_state.reconciliation_failure =
                        durable_checkpoint_reconciliation_failure(
                            result.error(), checkpoint_state.disposition,
                            "worker checkpoint failed after durable P4 boundary");
                    auto unresolved_state = indeterminate.after;
                    auto stop_state = current_profile->stop;
                    if (normal_stop) {
                        unresolved_state = current_profile->state;
                        unresolved_state.disk.activity =
                            product::ActivityState::resolving_commit;
                        unresolved_state.disk.persistence =
                            product::PersistenceState::commit_indeterminate;
                        unresolved_state.disk.health =
                            product::HealthState::needs_attention;
                        stop_state.phase = StopDurablePhase::resolving;
                    }
                    auto unresolved = normal_stop
                        ? profiles_.publish_stop(
                            current_profile.value(), unresolved_state,
                            current_profile->operation_gate,
                            checkpoint_state, current_profile->compaction,
                            stop_state)
                        : profiles_.publish_checkpoint(
                            current_profile.value(), unresolved_state,
                            current_profile->operation_gate,
                            checkpoint_state);
                    if (!unresolved.ok()) {
                        return Result<OperationRecord>::failure(
                            unresolved.error());
                    }
                    current_profile = unresolved.move_value();
                    return normal_stop
                        ? park_stop_reconciliation()
                        : park_checkpoint_reconciliation();
                }
                if (action == ActionKind::compact
                    && current_profile.has_value()
                    && current_profile->compaction.correlation.has_value()) {
                    const auto indeterminate = complete_command(
                        pending_plan,
                        CompletionOutcome::commit_indeterminate);
                    if (!indeterminate.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.resume_compaction",
                                L"The missing M3 compaction proof cannot enter attention state"));
                    }
                    auto compaction_state =
                        current_profile->compaction;
                    compaction_state.phase =
                        CompactionDurablePhase::resolving;
                    compaction_state.reconciliation_failure =
                        durable_compaction_reconciliation_failure(
                            result.error(), compaction_state.disposition,
                            "worker compaction failed after durable mutation boundary");
                    auto unresolved = profiles_.publish_compaction(
                        current_profile.value(),
                        indeterminate.after,
                        current_profile->operation_gate,
                        current_profile->checkpoint,
                        compaction_state);
                    if (!unresolved.ok()) {
                        return Result<OperationRecord>::failure(
                            unresolved.error());
                    }
                    current_profile = unresolved.move_value();
                    return park_compaction_reconciliation();
                }
                worker_succeeded = false;
                completion = CompletionOutcome::transient_failure;
                terminal_result = {
                    std::byte{'w'}, std::byte{'o'}, std::byte{'r'},
                    std::byte{'k'}, std::byte{'e'}, std::byte{'r'}};
                break;
            }
            worker_result = result.move_value();
            if (action == ActionKind::compact
                && pending_plan.command == ManagementCommand::compact) {
                if (worker_result.compaction_preflight.has_value()) {
                    const auto& preflight =
                        worker_result.compaction_preflight.value();
                    if (!current_profile.has_value()
                        || current_profile->compaction.correlation.has_value()
                        || worker_result.compaction.has_value()
                        || !valid_compaction_preflight_failure(
                            preflight, worker_request,
                            current_profile.value())) {
                        return Result<OperationRecord>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.compaction_preflight",
                            L"The worker returned contradictory low-space proof"));
                    }
                    completion = preflight.future_checkpoint_possible
                        ? CompletionOutcome::compaction_deferred
                        : CompletionOutcome::persistence_blocked;
                    const auto deferred = complete_command(
                        pending_plan, completion);
                    if (!deferred.applied()) {
                        return Result<OperationRecord>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.compaction_preflight",
                            L"The low-space compaction transition is invalid"));
                    }
                    auto deferred_compaction =
                        current_profile->compaction;
                    deferred_compaction.phase =
                        CompactionDurablePhase::stable;
                    deferred_compaction.correlation.reset();
                    deferred_compaction.disposition =
                        CompactionCommitDisposition::not_committed;
                    deferred_compaction.metrics.reset();
                    deferred_compaction.failure.reset();
                    deferred_compaction.reconciliation_failure.reset();
                    deferred_compaction.preflight_failure = preflight;
                    auto deferred_profile =
                        profiles_.publish_compaction(
                            current_profile.value(),
                            deferred.after,
                            current_profile->operation_gate,
                            current_profile->checkpoint,
                            deferred_compaction);
                    if (!deferred_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            deferred_profile.error());
                    }
                    current_profile = deferred_profile.move_value();
                    worker_succeeded = false;
                    terminal_result =
                        compaction_preflight_terminal_result(preflight);
                    break;
                }
                if (compaction_cancelled_before_boundary) {
                    auto cancelled_compaction =
                        current_profile->compaction;
                    cancelled_compaction.phase =
                        CompactionDurablePhase::stable;
                    cancelled_compaction.before_state.reset();
                    cancelled_compaction.correlation.reset();
                    cancelled_compaction.disposition =
                        CompactionCommitDisposition::not_committed;
                    cancelled_compaction.metrics.reset();
                    cancelled_compaction.failure.reset();
                    cancelled_compaction.reconciliation_failure.reset();
                    cancelled_compaction.preflight_failure.reset();
                    auto cancelled_profile =
                        profiles_.publish_compaction(
                            current_profile.value(),
                            current_profile->state,
                            current_profile->operation_gate,
                            current_profile->checkpoint,
                            cancelled_compaction);
                    if (!cancelled_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            cancelled_profile.error());
                    }
                    return finalize_operation(
                        current_operation,
                        cancelled_profile.value(),
                        pending_plan,
                        CompletionOutcome::transient_failure,
                        OperationState::cancelled,
                        {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
                         std::byte{'c'}, std::byte{'e'}, std::byte{'l'}});
                }
                if (!current_profile.has_value()
                    || !current_profile->compaction.correlation.has_value()
                    || !worker_result.compaction.has_value()
                    || !valid_compaction_evidence(
                        worker_result.compaction.value(),
                        current_profile->compaction.correlation.value())) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.compaction_evidence",
                        L"The worker returned missing or contradictory compaction evidence"));
                }
                auto evidence = worker_result.compaction.value();
                auto compaction_state = current_profile->compaction;
                compaction_state.phase =
                    CompactionDurablePhase::evidence_recorded;
                compaction_state.disposition = evidence.disposition;
                compaction_state.metrics = evidence.metrics;
                compaction_state.failure = evidence.error.has_value()
                    ? std::optional<CompactionFailureEvidence>(
                        durable_compaction_failure(evidence))
                    : std::nullopt;
                compaction_state.reconciliation_failure.reset();
                compaction_state.preflight_failure.reset();
                auto evidence_profile = profiles_.publish_compaction(
                    current_profile.value(),
                    current_profile->state,
                    current_profile->operation_gate,
                    current_profile->checkpoint,
                    compaction_state);
                if (!evidence_profile.ok()) {
                    return Result<OperationRecord>::failure(
                        evidence_profile.error());
                }
                current_profile = evidence_profile.move_value();

                if (evidence.disposition
                        == CompactionCommitDisposition::
                            committed_needs_reconciliation
                    || evidence.disposition
                        == CompactionCommitDisposition::
                            commit_indeterminate) {
                    const auto resolving_completion =
                        evidence.disposition
                                == CompactionCommitDisposition::
                                    commit_indeterminate
                            ? CompletionOutcome::commit_indeterminate
                            : CompletionOutcome::
                                committed_needs_reconciliation_dirty;
                    const auto resolving = complete_command(
                        pending_plan, resolving_completion);
                    if (!resolving.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.compaction_resolving",
                                L"The compaction disposition cannot enter reconciliation"));
                    }
                    compaction_state = current_profile->compaction;
                    compaction_state.phase =
                        CompactionDurablePhase::resolving;
                    auto resolving_profile =
                        profiles_.publish_compaction(
                            current_profile.value(),
                            resolving.after,
                            current_profile->operation_gate,
                            current_profile->checkpoint,
                            compaction_state);
                    if (!resolving_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            resolving_profile.error());
                    }
                    current_profile = resolving_profile.move_value();

                    auto resolved = worker_.reconcile_compaction(
                        worker_request, evidence.correlation);
                    std::optional<Error> reconciliation_error;
                    if (!resolved.ok()) {
                        reconciliation_error = resolved.error();
                    } else if (!resolved.value().compaction.has_value()
                        || !valid_compaction_evidence(
                            resolved.value().compaction.value(),
                            evidence.correlation)) {
                        reconciliation_error = service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.compaction_reconciliation",
                            L"The worker returned missing or contradictory compaction reconciliation evidence");
                    }
                    if (reconciliation_error.has_value()) {
                        compaction_state = current_profile->compaction;
                        compaction_state.reconciliation_failure =
                            durable_compaction_reconciliation_failure(
                                reconciliation_error.value(),
                                compaction_state.disposition,
                                "worker compaction reconciliation produced no acceptable proof");
                        auto failed_reconciliation =
                            profiles_.publish_compaction(
                                current_profile.value(),
                                current_profile->state,
                                current_profile->operation_gate,
                                current_profile->checkpoint,
                                compaction_state);
                        if (!failed_reconciliation.ok()) {
                            return Result<OperationRecord>::failure(
                                failed_reconciliation.error());
                        }
                        current_profile =
                            failed_reconciliation.move_value();
                        return park_compaction_reconciliation();
                    }
                    evidence = resolved.value().compaction.value();
                    compaction_state = current_profile->compaction;
                    compaction_state.phase =
                        CompactionDurablePhase::evidence_recorded;
                    compaction_state.disposition = evidence.disposition;
                    compaction_state.metrics = evidence.metrics;
                    compaction_state.failure = evidence.error.has_value()
                        ? std::optional<CompactionFailureEvidence>(
                            durable_compaction_failure(evidence))
                        : std::nullopt;
                    compaction_state.reconciliation_failure.reset();
                    compaction_state.preflight_failure.reset();
                    auto resolved_profile =
                        profiles_.publish_compaction(
                            current_profile.value(),
                            current_profile->state,
                            current_profile->operation_gate,
                            current_profile->checkpoint,
                            compaction_state);
                    if (!resolved_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            resolved_profile.error());
                    }
                    current_profile = resolved_profile.move_value();
                    if (evidence.disposition
                            == CompactionCommitDisposition::
                                committed_needs_reconciliation
                        || evidence.disposition
                            == CompactionCommitDisposition::
                                commit_indeterminate) {
                        compaction_state = current_profile->compaction;
                        compaction_state.phase =
                            CompactionDurablePhase::resolving;
                        auto unresolved_profile =
                            profiles_.publish_compaction(
                                current_profile.value(),
                                current_profile->state,
                                current_profile->operation_gate,
                                current_profile->checkpoint,
                                compaction_state);
                        if (!unresolved_profile.ok()) {
                            return Result<OperationRecord>::failure(
                                unresolved_profile.error());
                        }
                        current_profile = unresolved_profile.move_value();
                        return park_compaction_reconciliation();
                    }
                }

                if (evidence.disposition
                    == CompactionCommitDisposition::not_committed) {
                    completion = is_persistent_checkpoint_failure(
                                     evidence.error->code)
                        ? CompletionOutcome::persistence_blocked
                        : CompletionOutcome::transient_failure;
                    const auto not_committed = complete_command(
                        pending_plan, completion);
                    if (!not_committed.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.compaction_not_committed",
                                L"The NotCommitted compaction transition is invalid"));
                    }
                    compaction_state = current_profile->compaction;
                    compaction_state.phase =
                        CompactionDurablePhase::stable;
                    auto not_committed_profile =
                        profiles_.publish_compaction(
                            current_profile.value(),
                            not_committed.after,
                            current_profile->operation_gate,
                            current_profile->checkpoint,
                            compaction_state);
                    if (!not_committed_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            not_committed_profile.error());
                    }
                    current_profile =
                        not_committed_profile.move_value();
                    worker_succeeded = false;
                    terminal_result = compaction_terminal_result(
                        evidence.correlation,
                        evidence.disposition,
                        current_profile->checkpoint.confirmed_generation,
                        evidence.observed_dirty_epoch,
                        evidence.metrics,
                        current_profile->compaction);
                    break;
                }

                if (evidence.disposition
                    != CompactionCommitDisposition::committed) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.compaction_disposition",
                        L"The reconciled compaction disposition is invalid"));
                }

                compaction_state = current_profile->compaction;
                compaction_state.phase =
                    CompactionDurablePhase::
                        committed_pending_epoch_confirmation;
                auto pending_epoch = profiles_.publish_compaction(
                    current_profile.value(),
                    current_profile->state,
                    current_profile->operation_gate,
                    current_profile->checkpoint,
                    compaction_state);
                if (!pending_epoch.ok()) {
                    return Result<OperationRecord>::failure(
                        pending_epoch.error());
                }
                current_profile = pending_epoch.move_value();

                auto epoch_publication = worker_.publish_compaction_epoch(
                    worker_request,
                    evidence.correlation,
                    [&](const CompactionEpochObservation& observation)
                        -> Result<void> {
                        if (observation.correlation
                                != evidence.correlation
                            || observation.live_dirty_epoch
                                < evidence.correlation.cut_dirty_epoch) {
                            return Result<void>::failure(service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.compaction_epoch",
                                L"The worker compaction epoch regressed or changed identity"));
                        }
                        const auto clean = observation.live_dirty_epoch
                            == evidence.correlation.cut_dirty_epoch;
                        const auto completed = complete_command(
                            pending_plan,
                            clean
                                ? CompletionOutcome::succeeded_clean
                                : CompletionOutcome::succeeded_dirty);
                        if (!completed.applied()) {
                            return Result<void>::failure(service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.compaction_epoch",
                                L"The final compaction state is invalid"));
                        }
                        auto final_checkpoint =
                            current_profile->checkpoint;
                        final_checkpoint.confirmed_generation =
                            evidence.generation;
                        final_checkpoint.current_dirty_epoch =
                            observation.live_dirty_epoch;
                        auto final_compaction =
                            current_profile->compaction;
                        final_compaction.phase =
                            CompactionDurablePhase::stable;
                        final_compaction.disposition =
                            CompactionCommitDisposition::committed;
                        final_compaction.metrics = evidence.metrics;
                        final_compaction.failure.reset();
                        final_compaction.reconciliation_failure.reset();
                        final_compaction.preflight_failure.reset();
                        if (final_compaction
                                .last_published_compaction_operation_id
                            != std::optional<product::Uuid>(operation_id)) {
                            if (final_compaction
                                    .successful_compactions_total
                                == (std::numeric_limits<std::uint64_t>::max)()) {
                                return Result<void>::failure(service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.compaction_counter",
                                    L"The durable compaction counter overflowed"));
                            }
                            ++final_compaction.successful_compactions_total;
                            final_compaction
                                .last_published_compaction_operation_id =
                                operation_id;
                        }
                        final_compaction.checkpoints_since_compaction = 0;
                        auto final_profile =
                            profiles_.publish_compaction(
                                current_profile.value(),
                                completed.after,
                                current_profile->operation_gate,
                                final_checkpoint,
                                final_compaction);
                        if (!final_profile.ok()) {
                            return Result<void>::failure(
                                final_profile.error());
                        }
                        current_profile = final_profile.move_value();
                        completion = clean
                            ? CompletionOutcome::succeeded_clean
                            : CompletionOutcome::succeeded_dirty;
                        terminal_result = compaction_terminal_result(
                            evidence.correlation,
                            evidence.disposition,
                            evidence.generation,
                            observation.live_dirty_epoch,
                            evidence.metrics,
                            current_profile->compaction);
                        return Result<void>::success();
                    });
                if (!epoch_publication.ok()) {
                    return Result<OperationRecord>::failure(
                        epoch_publication.error());
                }
                worker_result.completion = completion;
                worker_result.terminal_result = terminal_result;
            }
            if (action == ActionKind::checkpoint
                && (pending_plan.command == ManagementCommand::checkpoint
                    || pending_plan.command == ManagementCommand::compact
                    || normal_stop
                    // The first provisioning checkpoint has the same durable
                    // P4/P5 evidence and epoch-publication obligations as a
                    // later checkpoint.  Leaving it out would commit a
                    // worker candidate without recording the matching service
                    // truth before publish-ready.
                    || provisioning_start)) {
                if (checkpoint_cancelled_before_boundary) {
                    if (pending_plan.command == ManagementCommand::compact) {
                        auto cancelled_compaction =
                            current_profile->compaction;
                        cancelled_compaction.phase =
                            CompactionDurablePhase::stable;
                        cancelled_compaction
                            .prerequisite_checkpoint_performed = true;
                        cancelled_compaction
                            .prerequisite_checkpoint_generation =
                            cancelled_checkpoint_correlation
                                ->prior_generation;
                        cancelled_compaction
                            .prerequisite_checkpoint_disposition =
                            CheckpointCommitDisposition::not_committed;
                        auto cancelled_profile =
                            profiles_.publish_compaction(
                                current_profile.value(),
                                current_profile->state,
                                current_profile->operation_gate,
                                current_profile->checkpoint,
                                cancelled_compaction);
                        if (!cancelled_profile.ok()) {
                            return Result<OperationRecord>::failure(
                                cancelled_profile.error());
                        }
                        current_profile =
                            cancelled_profile.move_value();
                    }
                    return finalize_operation(
                        current_operation,
                        current_profile,
                        pending_plan,
                        CompletionOutcome::transient_failure,
                        OperationState::cancelled,
                        checkpoint_terminal_result(
                            cancelled_checkpoint_correlation.value(),
                            CheckpointCommitDisposition::not_committed,
                            cancelled_checkpoint_correlation
                                ->prior_generation,
                            cancelled_checkpoint_correlation
                                ->cut_dirty_epoch));
                }
                if (!current_profile.has_value()
                    || !current_profile->checkpoint.correlation.has_value()) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.checkpoint_evidence",
                        L"The P4 checkpoint correlation was not durable"));
                }
                if (!worker_result.checkpoint.has_value()
                    || !valid_checkpoint_evidence(
                        worker_result.checkpoint.value(),
                        current_profile->checkpoint.correlation.value())) {
                    const auto indeterminate = complete_command(
                        pending_plan,
                        CompletionOutcome::commit_indeterminate);
                    if (!indeterminate.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.checkpoint_evidence",
                                L"Contradictory M3 evidence cannot enter attention state"));
                    }
                    const auto evidence_error = service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.checkpoint_evidence",
                        L"The worker returned missing or contradictory M3 checkpoint evidence");
                    auto checkpoint_state = current_profile->checkpoint;
                    checkpoint_state.phase =
                        CheckpointDurablePhase::resolving;
                    checkpoint_state.reconciliation_failure =
                        durable_checkpoint_reconciliation_failure(
                            evidence_error,
                            checkpoint_state.disposition,
                            "worker checkpoint returned no acceptable M3 proof");
                    auto unresolved = profiles_.publish_checkpoint(
                        current_profile.value(), indeterminate.after,
                        current_profile->operation_gate,
                        checkpoint_state);
                    if (!unresolved.ok()) {
                        return Result<OperationRecord>::failure(
                            unresolved.error());
                    }
                    current_profile = unresolved.move_value();
                    return park_checkpoint_reconciliation();
                }
                auto evidence = worker_result.checkpoint.value();
                auto checkpoint_state = current_profile->checkpoint;
                checkpoint_state.phase =
                    CheckpointDurablePhase::evidence_recorded;
                checkpoint_state.disposition = evidence.disposition;
                // The evidence records this checkpoint's own session epoch, not a
                // max with the durable value.  The durable value may belong to a
                // previous worker process and be numerically larger; carrying it
                // forward would keep a per-process counter masquerading as a
                // profile-wide clock.
                checkpoint_state.current_dirty_epoch =
                    evidence.observed_dirty_epoch;
                checkpoint_state.failure = evidence.error.has_value()
                    ? std::optional<CheckpointFailureEvidence>(
                        durable_checkpoint_failure(evidence))
                    : std::nullopt;
                checkpoint_state.reconciliation_failure.reset();
                auto evidence_profile = profiles_.publish_checkpoint(
                    current_profile.value(), current_profile->state,
                    current_profile->operation_gate, checkpoint_state);
                if (!evidence_profile.ok()) {
                    return Result<OperationRecord>::failure(
                        evidence_profile.error());
                }
                current_profile = evidence_profile.move_value();

                auto evidence_operation = current_operation;
                ++evidence_operation.revision;
                evidence_operation.phase = "checkpoint-evidence";
                evidence_operation.updated_unix_ms = (std::max)(
                    evidence_operation.updated_unix_ms,
                    clock_.unix_time_ms());
                auto evidence_written = operations_.write(
                    current_operation.revision, evidence_operation);
                if (!evidence_written.ok()) {
                    return Result<OperationRecord>::failure(
                        evidence_written.error());
                }
                current_operation = std::move(evidence_operation);

                if (evidence.disposition
                        == CheckpointCommitDisposition::
                            committed_needs_reconciliation
                    || evidence.disposition
                        == CheckpointCommitDisposition::
                            commit_indeterminate) {
                    const auto resolving_completion =
                        evidence.disposition
                                == CheckpointCommitDisposition::
                                    commit_indeterminate
                            ? CompletionOutcome::commit_indeterminate
                            : CompletionOutcome::
                                committed_needs_reconciliation_dirty;
                    const auto resolving = complete_command(
                        pending_plan, resolving_completion);
                    if (!resolving.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.checkpoint_resolving",
                                L"The checkpoint disposition cannot enter reconciliation"));
                    }
                    checkpoint_state = current_profile->checkpoint;
                    checkpoint_state.phase =
                        CheckpointDurablePhase::resolving;
                    auto resolving_profile =
                        profiles_.publish_checkpoint(
                            current_profile.value(),
                            resolving.after,
                            current_profile->operation_gate,
                            checkpoint_state);
                    if (!resolving_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            resolving_profile.error());
                    }
                    current_profile = resolving_profile.move_value();

                    auto resolving_operation = current_operation;
                    ++resolving_operation.revision;
                    resolving_operation.phase = "checkpoint-resolving";
                    resolving_operation.updated_unix_ms = (std::max)(
                        resolving_operation.updated_unix_ms,
                        clock_.unix_time_ms());
                    auto resolving_written = operations_.write(
                        current_operation.revision,
                        resolving_operation);
                    if (!resolving_written.ok()) {
                        return Result<OperationRecord>::failure(
                            resolving_written.error());
                    }
                    current_operation = std::move(resolving_operation);

                    auto resolved = worker_.reconcile_checkpoint(
                        worker_request, evidence.correlation);
                    std::optional<Error> reconciliation_error;
                    if (!resolved.ok()) {
                        reconciliation_error = resolved.error();
                    } else if (!resolved.value().checkpoint.has_value()
                        || !valid_checkpoint_evidence(
                            resolved.value().checkpoint.value(),
                            evidence.correlation,
                            true)) {
                        reconciliation_error = service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.checkpoint_reconciliation",
                            L"The worker returned missing or contradictory M3 reconciliation evidence");
                    }
                    if (reconciliation_error.has_value()) {
                        checkpoint_state = current_profile->checkpoint;
                        checkpoint_state.reconciliation_failure =
                            durable_checkpoint_reconciliation_failure(
                                reconciliation_error.value(),
                                checkpoint_state.disposition,
                                "worker checkpoint reconciliation produced no acceptable proof");
                        auto failed_reconciliation =
                            profiles_.publish_checkpoint(
                                current_profile.value(),
                                current_profile->state,
                                current_profile->operation_gate,
                                checkpoint_state);
                        if (!failed_reconciliation.ok()) {
                            return Result<OperationRecord>::failure(
                                failed_reconciliation.error());
                        }
                        current_profile =
                            failed_reconciliation.move_value();
                        return park_checkpoint_reconciliation();
                    }
                    evidence = resolved.value().checkpoint.value();
                    checkpoint_state = current_profile->checkpoint;
                    checkpoint_state.phase =
                        CheckpointDurablePhase::evidence_recorded;
                    checkpoint_state.disposition = evidence.disposition;
                    checkpoint_state.current_dirty_epoch =
                        evidence.observed_dirty_epoch;
                    checkpoint_state.failure = evidence.error.has_value()
                        ? std::optional<CheckpointFailureEvidence>(
                            durable_checkpoint_failure(evidence))
                        : std::nullopt;
                    checkpoint_state.reconciliation_failure.reset();
                    auto resolved_profile =
                        profiles_.publish_checkpoint(
                            current_profile.value(),
                            current_profile->state,
                            current_profile->operation_gate,
                            checkpoint_state);
                    if (!resolved_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            resolved_profile.error());
                    }
                    current_profile = resolved_profile.move_value();
                    if (evidence.disposition
                            == CheckpointCommitDisposition::
                                committed_needs_reconciliation
                        || evidence.disposition
                            == CheckpointCommitDisposition::
                                commit_indeterminate) {
                        const auto unresolved_completion =
                            evidence.disposition
                                    == CheckpointCommitDisposition::
                                        commit_indeterminate
                                ? CompletionOutcome::commit_indeterminate
                                : CompletionOutcome::
                                    committed_needs_reconciliation_dirty;
                        const auto unresolved = complete_command(
                            pending_plan, unresolved_completion);
                        if (!unresolved.applied()) {
                            return Result<OperationRecord>::failure(
                                service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.checkpoint_unresolved",
                                    L"The unresolved checkpoint state is invalid"));
                        }
                        checkpoint_state = current_profile->checkpoint;
                        checkpoint_state.phase =
                            CheckpointDurablePhase::resolving;
                        auto unresolved_profile =
                            profiles_.publish_checkpoint(
                                current_profile.value(),
                                unresolved.after,
                                current_profile->operation_gate,
                                checkpoint_state);
                        if (!unresolved_profile.ok()) {
                            return Result<OperationRecord>::failure(
                                unresolved_profile.error());
                        }
                        current_profile =
                            unresolved_profile.move_value();
                        return park_checkpoint_reconciliation();
                    }
                }

                if (evidence.disposition
                    == CheckpointCommitDisposition::not_committed) {
                    completion = is_persistent_checkpoint_failure(
                                     evidence.error->code)
                        ? CompletionOutcome::persistence_blocked
                        : CompletionOutcome::transient_failure;
                    const auto not_committed = complete_command(
                        pending_plan, completion);
                    if (!not_committed.applied()) {
                        return Result<OperationRecord>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.checkpoint_not_committed",
                                L"The NotCommitted state transition is invalid"));
                    }
                    checkpoint_state = current_profile->checkpoint;
                    checkpoint_state.phase =
                        CheckpointDurablePhase::stable;
                    auto compaction_state =
                        current_profile->compaction;
                    if (pending_plan.command
                        == ManagementCommand::compact) {
                        compaction_state.phase =
                            CompactionDurablePhase::stable;
                        compaction_state
                            .prerequisite_checkpoint_performed = true;
                        compaction_state
                            .prerequisite_checkpoint_generation =
                            current_profile->checkpoint
                                .confirmed_generation;
                        compaction_state
                            .prerequisite_checkpoint_disposition =
                            CheckpointCommitDisposition::not_committed;
                    }
                    auto not_committed_state = not_committed.after;
                    if (normal_stop) {
                        not_committed_state = current_profile->state;
                        not_committed_state.disk.persistence =
                            completion == CompletionOutcome::persistence_blocked
                            ? product::PersistenceState::persistence_blocked
                            : product::PersistenceState::dirty;
                        if (completion
                            == CompletionOutcome::persistence_blocked) {
                            not_committed_state.disk.health =
                                product::HealthState::degraded;
                        }
                    }
                    auto not_committed_profile =
                        pending_plan.command == ManagementCommand::compact
                        ? profiles_.publish_compaction(
                            current_profile.value(),
                            not_committed_state,
                            current_profile->operation_gate,
                            checkpoint_state,
                            compaction_state)
                        : normal_stop
                        ? profiles_.publish_stop(
                            current_profile.value(),
                            not_committed_state,
                            current_profile->operation_gate,
                            checkpoint_state,
                            current_profile->compaction,
                            current_profile->stop)
                        : profiles_.publish_checkpoint(
                            current_profile.value(),
                            not_committed_state,
                            current_profile->operation_gate,
                            checkpoint_state);
                    if (!not_committed_profile.ok()) {
                        return Result<OperationRecord>::failure(
                            not_committed_profile.error());
                    }
                    current_profile =
                        not_committed_profile.move_value();
                    worker_succeeded = false;
                    terminal_result = checkpoint_terminal_result(
                        evidence.correlation,
                        evidence.disposition,
                        current_profile->checkpoint.confirmed_generation,
                        evidence.observed_dirty_epoch,
                        &current_profile->checkpoint.failure.value());
                    break;
                }

                if (evidence.disposition
                    != CheckpointCommitDisposition::committed) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.checkpoint_disposition",
                        L"The reconciled checkpoint disposition is invalid"));
                }

                checkpoint_state = current_profile->checkpoint;
                checkpoint_state.phase =
                    CheckpointDurablePhase::
                        committed_pending_epoch_confirmation;
                auto pending_epoch = normal_stop
                    ? profiles_.publish_stop(
                        current_profile.value(), current_profile->state,
                        current_profile->operation_gate, checkpoint_state,
                        current_profile->compaction, current_profile->stop)
                    : profiles_.publish_checkpoint(
                        current_profile.value(), current_profile->state,
                        current_profile->operation_gate, checkpoint_state);
                if (!pending_epoch.ok()) {
                    return Result<OperationRecord>::failure(
                        pending_epoch.error());
                }
                current_profile = pending_epoch.move_value();

                auto epoch_publication = worker_.publish_checkpoint_epoch(
                    worker_request,
                    evidence.correlation,
                    [&](const CheckpointEpochObservation& observation)
                        -> Result<void> {
                        if (observation.correlation
                                != evidence.correlation
                            || observation.live_dirty_epoch
                                < evidence.correlation.cut_dirty_epoch) {
                            return Result<void>::failure(service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.checkpoint_epoch",
                                L"The worker dirty epoch regressed or changed identity"));
                        }
                        // No comparison against the durable `current_dirty_epoch`
                        // is made here: that value may come from a previous
                        // worker process and be numerically larger, and the epoch
                        // is only meaningful within one session.  The live epoch
                        // was already checked against this checkpoint's own cut
                        // above, which is the comparison that has an order.
                        const auto clean =
                            observation.live_dirty_epoch
                            == evidence.correlation.cut_dirty_epoch;
                        const auto completed = complete_command(
                            pending_plan,
                            clean
                                ? CompletionOutcome::succeeded_clean
                                : CompletionOutcome::succeeded_dirty);
                        if (!completed.applied()) {
                            return Result<void>::failure(service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.checkpoint_epoch",
                                L"The final checkpoint state is invalid"));
                        }
                        auto final_checkpoint =
                            current_profile->checkpoint;
                        final_checkpoint.confirmed_generation =
                            evidence.generation;
                        final_checkpoint.current_dirty_epoch =
                            observation.live_dirty_epoch;
                        final_checkpoint.phase =
                            CheckpointDurablePhase::stable;
                        final_checkpoint.disposition =
                            CheckpointCommitDisposition::committed;
                        final_checkpoint.failure.reset();
                        final_checkpoint.reconciliation_failure.reset();
                        // The first checkpoint admits a provisioning `before_state`
                        // (it must, or the commit boundary is refused), but that
                        // state is no longer valid once the profile publishes
                        // itself ready: the durable-state validator requires a
                        // `before_state` to carry the same lifecycle as the state
                        // it belongs to, and `provisioning` does not equal
                        // `ready`.  The committed generation and disposition are
                        // the durable truth; the pre-start state is dropped.
                        // A normal checkpoint keeps its `before_state` -- its
                        // lifecycle already matches the ready profile it serves.
                        if (provisioning_start) {
                            final_checkpoint.before_state.reset();
                        }
                        auto final_compaction =
                            current_profile->compaction;
                        if (final_compaction
                                .last_counted_checkpoint_operation_id
                            != std::optional<product::Uuid>(operation_id)) {
                            if (final_compaction
                                    .checkpoints_since_compaction
                                == (std::numeric_limits<std::uint64_t>::max)()) {
                                return Result<void>::failure(service_error(
                                    ServiceCoreError::operation_invalid,
                                    L"service_core.checkpoint_counter",
                                    L"The durable checkpoint counter overflowed"));
                            }
                            ++final_compaction.checkpoints_since_compaction;
                            final_compaction
                                .last_counted_checkpoint_operation_id =
                                operation_id;
                        }
                        if (pending_plan.command
                            == ManagementCommand::compact) {
                            final_compaction.phase =
                                CompactionDurablePhase::checkpoint_confirmed;
                            final_compaction
                                .prerequisite_checkpoint_performed = true;
                            final_compaction
                                .prerequisite_checkpoint_generation =
                                evidence.generation;
                            final_compaction
                                .prerequisite_checkpoint_disposition =
                                CheckpointCommitDisposition::committed;
                        }
                        auto completed_state = completed.after;
                        // The first checkpoint establishes durable volume
                        // truth while the profile record is still
                        // provisioning.  publish_ready_profile is the only
                        // atomic lifecycle transition to ready, so publishing
                        // the completed ready state here would create a
                        // record/state contradiction the protected store must
                        // reject.
                        if (provisioning_start) {
                            completed_state = current_profile->state;
                        }
                        if (pending_plan.command
                            == ManagementCommand::compact) {
                            completed_state.disk.activity =
                                product::ActivityState::compacting;
                        }
                        auto final_stop = current_profile->stop;
                        if (normal_stop) {
                            completed_state = current_profile->state;
                            completed_state.disk.persistence = clean
                                ? product::PersistenceState::clean
                                : product::PersistenceState::dirty;
                            if (clean) {
                                final_stop.phase =
                                    StopDurablePhase::checkpoint_confirmed;
                            } else {
                                final_stop.phase = StopDurablePhase::resolving;
                                completed_state.disk.health =
                                    product::HealthState::needs_attention;
                            }
                        }
                        auto final_profile = normal_stop
                            ? profiles_.publish_stop(
                                current_profile.value(), completed_state,
                                current_profile->operation_gate,
                                final_checkpoint, final_compaction, final_stop)
                            // Every committed checkpoint carries durable
                            // compaction truth: execute_operation advances
                            // checkpoints_since_compaction and fences it with
                            // last_counted_checkpoint_operation_id above, and
                            // publish_compaction is the only store entry point
                            // that persists a proposed compaction state.
                            // Routing a plain checkpoint through
                            // publish_checkpoint would silently drop both.
                            : profiles_.publish_compaction(
                                current_profile.value(), completed_state,
                                current_profile->operation_gate,
                                final_checkpoint, final_compaction);
                        if (!final_profile.ok()) {
                            return Result<void>::failure(
                                final_profile.error());
                        }
                        current_profile = final_profile.move_value();
                        completion = clean
                            ? CompletionOutcome::succeeded_clean
                            : (normal_stop
                                ? CompletionOutcome::commit_indeterminate
                                : CompletionOutcome::succeeded_dirty);
                        terminal_result = checkpoint_terminal_result(
                            evidence.correlation,
                            evidence.disposition,
                            evidence.generation,
                            observation.live_dirty_epoch);
                        return Result<void>::success();
                    });
                if (!epoch_publication.ok()) {
                    return Result<OperationRecord>::failure(
                        epoch_publication.error());
                }
                worker_result.completion = completion;
                worker_result.terminal_result = terminal_result;
            }
        } else if (action == ActionKind::repair_profile_from_previous
                   && pending_execution->recovery_intent.has_value()
                   && pending_execution->recovery_intent->kind
                       == RecoveryOperationKind::repair_profile) {
            auto expected_previous = current_profile.value();
            expected_previous.state = pending_plan.before;
            expected_previous.operation_gate = idle_operation_gate();
            auto repaired = profiles_.repair_from_previous(
                expected_previous, current_profile->state,
                current_profile->operation_gate);
            if (!repaired.ok()) {
                return finalize_operation(
                    current_operation, std::nullopt, pending_plan,
                    CompletionOutcome::transient_failure,
                    OperationState::failed,
                    {std::byte{'r'}, std::byte{'e'}, std::byte{'p'},
                     std::byte{'a'}, std::byte{'i'}, std::byte{'r'}});
            }
            current_profile = repaired.move_value();
        } else if (action == ActionKind::publish_rebind
                   && pending_execution->recovery_intent.has_value()
                   && pending_execution->recovery_intent->kind
                       == RecoveryOperationKind::rebind) {
            if (!current_profile.has_value()
                || !rebind_bundle_evidence.has_value()
                || !rebind_record.has_value()) {
                worker_succeeded = false;
                worker_result.completion =
                    CompletionOutcome::needs_attention_failure;
                worker_result.terminal_result = {
                    std::byte{'r'}, std::byte{'e'}, std::byte{'b'},
                    std::byte{'i'}, std::byte{'n'}, std::byte{'d'}};
            } else {
                auto rebound = profiles_.publish_rebind(
                    current_profile.value(), rebind_record.value(),
                    rebind_bundle_evidence.value(), current_profile->state,
                    current_profile->operation_gate);
                if (!rebound.ok()) {
                    worker_succeeded = false;
                    worker_result.completion =
                        CompletionOutcome::transient_failure;
                    worker_result.terminal_result = {
                        std::byte{'c'}, std::byte{'a'}, std::byte{'s'}};
                } else {
                    current_profile = rebound.move_value();
                }
            }
        } else if (action == ActionKind::forget_metadata
                   && pending_execution->recovery_intent.has_value()
                   && pending_execution->recovery_intent->kind
                       == RecoveryOperationKind::forget) {
            if (!current_profile.has_value()
                || !pending_execution->recovery_intent->confirmation_id
                        .has_value()
                || pending_execution->recovery_intent->confirmation_id.value()
                    != current_operation.profile_id) {
                worker_result.completion =
                    CompletionOutcome::needs_attention_failure;
                worker_result.terminal_result = {
                    std::byte{'c'}, std::byte{'o'}, std::byte{'n'},
                    std::byte{'f'}, std::byte{'i'}, std::byte{'r'},
                    std::byte{'m'}};
            } else {
                auto tombstone = profiles_.find_forget_tombstone(
                    current_operation.operation_id);
                if (!tombstone.ok()) {
                    return Result<OperationRecord>::failure(
                        tombstone.error());
                }
                if (!tombstone.value().has_value()) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.forget_tombstone",
                        L"The admitted forget operation lost its durable tombstone"));
                } else {
                    const auto& durable_tombstone =
                        tombstone.value().value();
                    const WorkerActionRequest proof_request{
                        current_operation.request_id,
                        current_operation.operation_id,
                        current_operation.profile_id,
                        pending_plan.command,
                        action,
                        pending_execution->lifecycle_intent,
                        current_profile->record,
                        pending_execution->recovery_intent};
                    auto proof = worker_.prove_forget_safe(proof_request);
                    const bool exact_proof = proof.ok()
                        && proof.value().profile_id
                            == durable_tombstone.profile_id
                        && proof.value().image_id
                            == durable_tombstone.image_id
                        && proof.value().profile_revision
                            == durable_tombstone.profile_revision
                        && proof.value().worker_absent
                        && proof.value().writer_lock_absent
                        && proof.value().matching_device_absent
                        && proof.value().bundle_present
                        && proof.value().retained_bundle_identity
                            == durable_tombstone.retained_bundle_identity;
                    if (!exact_proof) {
                        worker_result.completion =
                            CompletionOutcome::needs_attention_failure;
                        worker_result.terminal_result = {
                            std::byte{'u'}, std::byte{'n'}, std::byte{'s'},
                            std::byte{'a'}, std::byte{'f'}, std::byte{'e'}};
                    } else {
                        const ForgetMetadataRemovalRequest removal_request{
                            current_operation.operation_id,
                            current_operation.request_id,
                            current_operation.profile_id,
                            durable_tombstone.image_id,
                            current_profile->record.revision,
                            current_profile->state,
                            current_profile->operation_gate};
                        auto removed = profiles_.remove_forget_metadata(
                            removal_request, proof.value());
                        if (!removed.ok()
                            || removed.value().operation_id
                                != durable_tombstone.operation_id
                            || removed.value().request_id
                                != durable_tombstone.request_id
                            || removed.value().profile_id
                                != durable_tombstone.profile_id
                            || removed.value().image_id
                                != durable_tombstone.image_id
                            || removed.value().profile_revision
                                != durable_tombstone.profile_revision
                            || removed.value().confirmed_generation
                                != durable_tombstone.confirmed_generation
                            || removed.value().retained_bundle_path
                                != durable_tombstone.retained_bundle_path
                            || removed.value().authorization_owner_sid
                                != durable_tombstone.authorization_owner_sid
                            || removed.value().retained_bundle_identity
                                != durable_tombstone.retained_bundle_identity
                            || removed.value().before_state
                                != durable_tombstone.before_state
                            || removed.value().phase
                                != ForgetTombstonePhase::metadata_removed) {
                            worker_result.completion =
                                CompletionOutcome::transient_failure;
                            worker_result.terminal_result = {
                                std::byte{'r'}, std::byte{'e'},
                                std::byte{'m'}, std::byte{'o'},
                                std::byte{'v'}, std::byte{'e'}};
                        } else {
                            worker_result.terminal_result =
                                forget_terminal_result(removed.value());
                            current_profile.reset();
                        }
                    }
                }
            }
        }
        completion = worker_result.completion;
        terminal_result = worker_result.terminal_result;
        if (!is_successful_completion(completion)) {
            worker_succeeded = false;
            break;
        }
        if (action == ActionKind::verify_rebind_bundle
            && pending_execution->recovery_intent.has_value()
            && pending_execution->recovery_intent->kind
                == RecoveryOperationKind::rebind) {
            if (!current_profile.has_value()
                || !worker_result.lifecycle.bundle.has_value()
                || !valid_rebind_bundle_evidence(
                    pending_execution->recovery_intent.value(),
                    current_profile.value(),
                    worker_result.lifecycle.bundle.value())
                || current_profile->record.revision
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                worker_succeeded = false;
                completion = CompletionOutcome::transient_failure;
                terminal_result = {
                    std::byte{'i'}, std::byte{'d'}, std::byte{'e'},
                    std::byte{'n'}, std::byte{'t'}, std::byte{'i'},
                    std::byte{'t'}, std::byte{'y'}};
                break;
            }
            rebind_bundle_evidence = worker_result.lifecycle.bundle;
            auto proposed = current_profile->record;
            ++proposed.revision;
            proposed.profile.bundle_path =
                pending_execution->recovery_intent->bundle_path;
            proposed.profile.manifest_name =
                rebind_bundle_evidence->manifest_name;
            rebind_record = std::move(proposed);
        }
        if (stop_operation) {
            std::optional<StopDurablePhase> expected_stop_phase;
            std::optional<StopDurablePhase> completed_stop_phase;
            switch (action) {
            case ActionKind::remove_mount:
                expected_stop_phase = StopDurablePhase::removal_boundary;
                completed_stop_phase = StopDurablePhase::mount_removed;
                break;
            case ActionKind::remove_exact_device:
                expected_stop_phase = StopDurablePhase::mount_removed;
                completed_stop_phase = StopDurablePhase::device_removed;
                break;
            case ActionKind::stop_runtime_and_release_lock:
                expected_stop_phase = StopDurablePhase::device_removed;
                completed_stop_phase = StopDurablePhase::runtime_stopped;
                break;
            default:
                break;
            }
            if (expected_stop_phase.has_value()) {
                if (!current_profile.has_value()
                    || current_profile->stop.phase
                        != expected_stop_phase.value()) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.stop_phase",
                        L"The durable stop phase does not match the completed action"));
                }
                auto stop_state = current_profile->stop;
                stop_state.phase = completed_stop_phase.value();
                auto published = profiles_.publish_stop(
                    current_profile.value(), current_profile->state,
                    current_profile->operation_gate,
                    current_profile->checkpoint,
                    current_profile->compaction, stop_state);
                if (!published.ok()) {
                    return Result<OperationRecord>::failure(
                        published.error());
                }
                current_profile = published.move_value();
            }
        }
        if (pending_execution->lifecycle_intent.has_value()) {
            const auto& intent = pending_execution->lifecycle_intent.value();
            if (action == ActionKind::create_and_verify_bundle
                || action == ActionKind::inspect_bundle_read_only) {
                if (!worker_result.lifecycle.bundle.has_value()) {
                    worker_succeeded = false;
                    completion = CompletionOutcome::needs_attention_failure;
                    terminal_result = {
                        std::byte{'b'}, std::byte{'u'}, std::byte{'n'},
                        std::byte{'d'}, std::byte{'l'}, std::byte{'e'}};
                    break;
                }
                if (revalidating_durable_bundle) {
                    const auto& durable =
                        lifecycle_reservation->bundle.value();
                    const auto& observed_bundle =
                        worker_result.lifecycle.bundle.value();
                    const bool volume_matches =
                        intent.kind == LifecycleOperationKind::create
                        || (lifecycle_reservation->volume.has_value()
                            && worker_result.lifecycle.volume.has_value()
                            && lifecycle_reservation->volume.value()
                                == worker_result.lifecycle.volume.value());
                    if (observed_bundle.source
                            != BundleEvidenceSource::read_only_inspection
                        || !same_bundle_binding(durable, observed_bundle)
                        || !volume_matches) {
                        worker_succeeded = false;
                        completion =
                            CompletionOutcome::needs_attention_failure;
                        terminal_result = {
                            std::byte{'i'}, std::byte{'d'},
                            std::byte{'e'}, std::byte{'n'},
                            std::byte{'t'}, std::byte{'i'},
                            std::byte{'t'}, std::byte{'y'}};
                        break;
                    }
                    bundle_evidence = durable;
                    volume_evidence = lifecycle_reservation->volume;
                } else {
                    if (!valid_bundle_evidence(
                            intent,
                            worker_result.lifecycle.bundle.value())
                        || (intent.kind == LifecycleOperationKind::import
                            && !worker_result.lifecycle.volume.has_value())) {
                        worker_succeeded = false;
                        completion =
                            CompletionOutcome::needs_attention_failure;
                        terminal_result = {
                            std::byte{'i'}, std::byte{'d'},
                            std::byte{'e'}, std::byte{'n'},
                            std::byte{'t'}, std::byte{'i'},
                            std::byte{'t'}, std::byte{'y'}};
                        break;
                    }
                    bundle_evidence = worker_result.lifecycle.bundle;
                    volume_evidence = worker_result.lifecycle.volume;
                    auto proposed_reservation =
                        lifecycle_reservation.value();
                    proposed_reservation.phase =
                        intent.kind == LifecycleOperationKind::create
                        ? LifecycleReservationPhase::bundle_verified
                        : LifecycleReservationPhase::inspection_verified;
                    proposed_reservation.bundle = bundle_evidence;
                    proposed_reservation.volume = volume_evidence;
                    auto persisted = persist_reservation_transition(
                        *registrations_, lifecycle_reservation.value(),
                        std::move(proposed_reservation));
                    if (!persisted.ok()) {
                        worker_succeeded = false;
                        registration_truth_ambiguous = true;
                        completion = CompletionOutcome::commit_indeterminate;
                        terminal_result = {
                            std::byte{'e'}, std::byte{'v'},
                            std::byte{'i'}, std::byte{'d'},
                            std::byte{'e'}, std::byte{'n'},
                            std::byte{'c'}, std::byte{'e'}};
                        break;
                    }
                    lifecycle_reservation = persisted.move_value();
                }
            } else if (action == ActionKind::publish_provisioning_profile
                       || action
                            == ActionKind::publish_imported_ready_profile) {
                if (!bundle_evidence.has_value() || registrations_ == nullptr) {
                    worker_succeeded = false;
                    completion = CompletionOutcome::needs_attention_failure;
                    break;
                }
                const auto lifecycle = action
                        == ActionKind::publish_provisioning_profile
                    ? profile::ProfileLifecycle::provisioning
                    : profile::ProfileLifecycle::ready;
                auto record = make_registration_record(
                    intent, bundle_evidence.value(), volume_evidence,
                    lifecycle);
                if (!record.ok()) {
                    worker_succeeded = false;
                    completion = CompletionOutcome::needs_attention_failure;
                    terminal_result = {
                        std::byte{'p'}, std::byte{'r'}, std::byte{'o'},
                        std::byte{'f'}, std::byte{'i'}, std::byte{'l'},
                        std::byte{'e'}};
                    break;
                }
                const auto gate = plan_gate_access(
                    idle_operation_gate(), pending_plan, operation_id);
                if (!gate.accepted()) {
                    return Result<OperationRecord>::failure(service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.registration_publish_gate",
                        L"The new profile gate could not be materialized"));
                }
                auto publication_gate = gate.after;
                if (!current_operation.cancellable) {
                    const auto noncancellable = mark_operation_noncancellable(
                        publication_gate, operation_id);
                    if (!noncancellable.accepted()) {
                        return Result<OperationRecord>::failure(service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.registration_publish_gate",
                            L"The new profile gate rejected the publication boundary"));
                    }
                    publication_gate = noncancellable.after;
                }
                const ManagedProfileSnapshot intended{
                    record.value(),
                    pending_plan.admitted,
                    publication_gate};
                auto published = volume_evidence.has_value()
                    ? registrations_->publish_registration(
                        intent,
                        record.value(),
                        bundle_evidence.value(),
                        volume_evidence,
                        pending_plan.admitted,
                        publication_gate)
                    : registrations_->publish_registration(
                        intent,
                        record.value(),
                        bundle_evidence.value(),
                        pending_plan.admitted,
                        publication_gate);
                if (!published.ok()) {
                    worker_succeeded = false;
                    completion = CompletionOutcome::transient_failure;
                    terminal_result = {
                        std::byte{'r'}, std::byte{'e'}, std::byte{'g'},
                        std::byte{'i'}, std::byte{'s'}, std::byte{'t'},
                        std::byte{'e'}, std::byte{'r'}};
                    break;
                }
                auto applied = apply_registration_mutation(
                    *registrations_, operation_id, intended,
                    published.move_value());
                if (!applied.ok()) {
                    return Result<OperationRecord>::failure(applied.error());
                }
                if (applied.value().disposition
                        != RegistrationApplicationDisposition::applied) {
                    worker_succeeded = false;
                    registration_truth_ambiguous =
                        applied.value().disposition
                        == RegistrationApplicationDisposition::ambiguous;
                    completion = applied.value().disposition
                            == RegistrationApplicationDisposition::ambiguous
                        ? CompletionOutcome::commit_indeterminate
                        : CompletionOutcome::needs_attention_failure;
                    terminal_result = {
                        std::byte{'r'}, std::byte{'e'}, std::byte{'g'},
                        std::byte{'i'}, std::byte{'s'}, std::byte{'t'},
                        std::byte{'e'}, std::byte{'r'}};
                    break;
                }
                current_profile = applied.value().snapshot;
                auto proposed_reservation = lifecycle_reservation.value();
                proposed_reservation.phase =
                    LifecycleReservationPhase::profile_published;
                auto persisted = persist_reservation_transition(
                    *registrations_, lifecycle_reservation.value(),
                    std::move(proposed_reservation));
                if (!persisted.ok()) {
                    worker_succeeded = false;
                    registration_truth_ambiguous = true;
                    completion = CompletionOutcome::commit_indeterminate;
                    terminal_result = {
                        std::byte{'p'}, std::byte{'h'}, std::byte{'a'},
                        std::byte{'s'}, std::byte{'e'}};
                    break;
                }
                lifecycle_reservation = persisted.move_value();
                // A `disk create` without `--no-start` provisions and then
                // checkpoints inside one plan, so the profile published here is
                // the state its own first checkpoint commits from.  This is the
                // moment that state starts existing, so it is where the
                // checkpoint can first be admitted.
                auto admitted = admit_first_checkpoint();
                if (!admitted.ok()) {
                    worker_succeeded = false;
                    completion = CompletionOutcome::transient_failure;
                    terminal_result = {
                        std::byte{'p'}, std::byte{'r'}, std::byte{'o'},
                        std::byte{'f'}, std::byte{'i'}, std::byte{'l'},
                        std::byte{'e'}};
                    break;
                }
            }
        }
        if (action == ActionKind::record_volume_identity) {
            if (!current_profile.has_value()
                || !worker_result.lifecycle.volume.has_value()
                || registrations_ == nullptr) {
                worker_succeeded = false;
                completion = CompletionOutcome::needs_attention_failure;
                break;
            }
            volume_evidence = worker_result.lifecycle.volume;
            auto proposed = current_profile->record;
            ++proposed.revision;
            proposed.profile.gpt_disk_guid = volume_evidence->gpt_disk_id;
            proposed.profile.partition_guid = volume_evidence->partition_id;
            proposed.profile.volume_guid = volume_evidence->volume_id;
            const ManagedProfileSnapshot intended{
                proposed,
                current_profile->state,
                current_profile->operation_gate,
                current_profile->checkpoint,
                current_profile->compaction,
                current_profile->stop};
            auto updated = registrations_->update_registration(
                current_profile.value(), proposed,
                current_profile->state,
                current_profile->operation_gate);
            if (!updated.ok()) {
                worker_succeeded = false;
                completion = CompletionOutcome::needs_attention_failure;
                terminal_result = {
                    std::byte{'i'}, std::byte{'d'}, std::byte{'e'},
                    std::byte{'n'}, std::byte{'t'}, std::byte{'i'},
                    std::byte{'t'}, std::byte{'y'}};
                break;
            }
            auto applied = apply_registration_mutation(
                *registrations_, operation_id, intended,
                updated.move_value());
            if (!applied.ok()) {
                return Result<OperationRecord>::failure(applied.error());
            }
            if (applied.value().disposition
                    != RegistrationApplicationDisposition::applied) {
                worker_succeeded = false;
                registration_truth_ambiguous =
                    applied.value().disposition
                    == RegistrationApplicationDisposition::ambiguous;
                completion = applied.value().disposition
                        == RegistrationApplicationDisposition::ambiguous
                    ? CompletionOutcome::commit_indeterminate
                    : CompletionOutcome::needs_attention_failure;
                terminal_result = {
                    std::byte{'i'}, std::byte{'d'}, std::byte{'e'},
                    std::byte{'n'}, std::byte{'t'}, std::byte{'i'},
                    std::byte{'t'}, std::byte{'y'}};
                break;
            }
            current_profile = applied.value().snapshot;
            if (lifecycle_reservation.has_value()) {
                auto proposed_reservation = lifecycle_reservation.value();
                proposed_reservation.phase =
                    LifecycleReservationPhase::volume_identity_recorded;
                proposed_reservation.volume = volume_evidence;
                auto persisted = persist_reservation_transition(
                    *registrations_, lifecycle_reservation.value(),
                    std::move(proposed_reservation));
                if (!persisted.ok()) {
                    worker_succeeded = false;
                    registration_truth_ambiguous = true;
                    completion = CompletionOutcome::commit_indeterminate;
                    terminal_result = {
                        std::byte{'p'}, std::byte{'h'}, std::byte{'a'},
                        std::byte{'s'}, std::byte{'e'}};
                    break;
                }
                lifecycle_reservation = persisted.move_value();
            }
        } else if (action == ActionKind::checkpoint
                   && (pending_plan.command
                            == ManagementCommand::create_and_start
                       || (pending_plan.command == ManagementCommand::start
                           && pending_plan.before.lifecycle
                               == profile::ProfileLifecycle::provisioning))) {
            if (!worker_result.lifecycle.first_checkpoint_acknowledged) {
                worker_succeeded = false;
                completion = CompletionOutcome::needs_attention_failure;
                terminal_result = {
                    std::byte{'c'}, std::byte{'h'}, std::byte{'e'},
                    std::byte{'c'}, std::byte{'k'}};
                break;
            }
            first_checkpoint_acknowledged = true;
        } else if (action == ActionKind::publish_ready_profile) {
            if (!first_checkpoint_acknowledged
                || !current_profile.has_value()
                || recorded_volume_identity_count(
                    current_profile->record.profile) != 3
                || registrations_ == nullptr) {
                worker_succeeded = false;
                completion = CompletionOutcome::needs_attention_failure;
                break;
            }
            auto proposed = current_profile->record;
            ++proposed.revision;
            proposed.profile.lifecycle = profile::ProfileLifecycle::ready;
            auto state = current_profile->state;
            state.lifecycle = profile::ProfileLifecycle::ready;
            const ManagedProfileSnapshot intended{
                proposed,
                state,
                current_profile->operation_gate,
                current_profile->checkpoint,
                current_profile->compaction,
                current_profile->stop};
            auto updated = registrations_->update_registration(
                current_profile.value(), proposed, state,
                current_profile->operation_gate);
            if (!updated.ok()) {
                worker_succeeded = false;
                completion = CompletionOutcome::needs_attention_failure;
                terminal_result = {
                    std::byte{'r'}, std::byte{'e'}, std::byte{'a'},
                    std::byte{'d'}, std::byte{'y'}};
                break;
            }
            auto applied = apply_registration_mutation(
                *registrations_, operation_id, intended,
                updated.move_value());
            if (!applied.ok()) {
                return Result<OperationRecord>::failure(applied.error());
            }
            if (applied.value().disposition
                    != RegistrationApplicationDisposition::applied) {
                worker_succeeded = false;
                registration_truth_ambiguous =
                    applied.value().disposition
                    == RegistrationApplicationDisposition::ambiguous;
                completion = applied.value().disposition
                        == RegistrationApplicationDisposition::ambiguous
                    ? CompletionOutcome::commit_indeterminate
                    : CompletionOutcome::needs_attention_failure;
                terminal_result = {
                    std::byte{'r'}, std::byte{'e'}, std::byte{'a'},
                    std::byte{'d'}, std::byte{'y'}};
                break;
            }
            current_profile = applied.value().snapshot;
            if (lifecycle_reservation.has_value()) {
                auto proposed_reservation = lifecycle_reservation.value();
                proposed_reservation.phase =
                    LifecycleReservationPhase::ready_published;
                auto persisted = persist_reservation_transition(
                    *registrations_, lifecycle_reservation.value(),
                    std::move(proposed_reservation));
                if (!persisted.ok()) {
                    worker_succeeded = false;
                    registration_truth_ambiguous = true;
                    completion = CompletionOutcome::commit_indeterminate;
                    terminal_result = {
                        std::byte{'p'}, std::byte{'h'}, std::byte{'a'},
                        std::byte{'s'}, std::byte{'e'}};
                    break;
                }
                lifecycle_reservation = persisted.move_value();
            }
        }
    }
    if (stop_operation && !idempotent_noop) {
        if (!worker_succeeded) {
            if (current_profile->stop.phase
                    >= StopDurablePhase::removal_boundary
                || current_profile->stop.phase
                    == StopDurablePhase::resolving) {
                return park_stop_reconciliation();
            }
            auto resumed = worker_.execute(WorkerActionRequest{
                current_operation.request_id,
                current_operation.operation_id,
                current_operation.profile_id,
                pending_plan.command,
                ActionKind::resume_mutations,
                std::nullopt,
                current_profile->record});
            if (!resumed.ok()
                || !is_successful_completion(resumed.value().completion)) {
                auto unresolved_stop = current_profile->stop;
                unresolved_stop.phase = StopDurablePhase::resolving;
                auto unresolved_state = current_profile->state;
                unresolved_state.disk.health =
                    product::HealthState::needs_attention;
                auto unresolved = profiles_.publish_stop(
                    current_profile.value(), unresolved_state,
                    current_profile->operation_gate,
                    current_profile->checkpoint,
                    current_profile->compaction, unresolved_stop);
                if (!unresolved.ok()) {
                    return Result<OperationRecord>::failure(
                        unresolved.error());
                }
                current_profile = unresolved.move_value();
                return park_stop_reconciliation();
            }
            auto rollback_state = current_profile->stop.before_state.value();
            rollback_state.disk.persistence =
                current_profile->state.disk.persistence;
            rollback_state.disk.health = current_profile->state.disk.health;
            auto rollback_checkpoint = current_profile->checkpoint;
            if (rollback_checkpoint.phase == CheckpointDurablePhase::admitted
                && !rollback_checkpoint.correlation.has_value()) {
                rollback_checkpoint.phase = CheckpointDurablePhase::stable;
                rollback_checkpoint.before_state.reset();
                rollback_checkpoint.disposition.reset();
                rollback_checkpoint.failure.reset();
                rollback_checkpoint.reconciliation_failure.reset();
            }
            auto rolled_back = profiles_.publish_stop(
                current_profile.value(), rollback_state,
                current_profile->operation_gate, rollback_checkpoint,
                current_profile->compaction, StopDurableState{});
            if (!rolled_back.ok()) {
                return Result<OperationRecord>::failure(
                    rolled_back.error());
            }
            current_profile = rolled_back.move_value();
        } else if (current_profile->stop.phase
                   != StopDurablePhase::runtime_stopped) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.stop_completion",
                L"The normal stop completed without full teardown proof"));
        }
        if (force_stop && worker_succeeded) {
            terminal_result = force_stop_terminal_result(
                current_profile->record, current_profile->stop);
        }
    }
    auto final_observation = operations_.load(operation_id);
    if (!final_observation.ok()) {
        return Result<OperationRecord>::failure(final_observation.error());
    }
    if (is_terminal(final_observation.value().state)) {
        return final_observation;
    }
    if (final_observation.value().revision != current_operation.revision) {
        if (final_observation.value().revision
                != current_operation.revision + 1
            || !final_observation.value().cancel_requested
            || !final_observation.value().cancellable) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.final_observation",
                L"The durable operation changed before completion"));
        }
        return finalize_operation(
            final_observation.value(),
            current_profile,
            pending_plan,
            CompletionOutcome::transient_failure,
            OperationState::cancelled,
            {std::byte{'c'}, std::byte{'a'}, std::byte{'n'},
             std::byte{'c'}, std::byte{'e'}, std::byte{'l'}});
    }
    return finalize_operation(
        current_operation,
        current_profile,
        pending_plan,
        completion,
        worker_succeeded ? OperationState::succeeded
                         : OperationState::failed,
        std::move(terminal_result),
        registration_truth_ambiguous);
}

Result<OperationRecord> ServiceCore::terminalize_unowned_admission(
    const OperationRecord& current,
    const wchar_t* operation) {
    if (current.revision != 1
        || current.state != OperationState::queued
        || current.phase != "queued"
        || current.updated_unix_ms != current.created_unix_ms
        || !current.cancellable
        || current.cancel_requested
        || current.terminal_unix_ms.has_value()
        || !current.terminal_result.empty()) {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            operation,
            L"Only a pristine operation-first admission may be terminalized without profile truth"));
    }
    auto terminal = current;
    ++terminal.revision;
    terminal.state = OperationState::failed;
    terminal.phase = "terminal";
    terminal.updated_unix_ms = (std::max)(
        terminal.updated_unix_ms, clock_.unix_time_ms());
    terminal.terminal_unix_ms = terminal.updated_unix_ms;
    terminal.cancellable = false;
    terminal.terminal_result = {
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'O'}, std::byte{'R'}, std::byte{'P'}, std::byte{'H'}};
    append_u32(terminal.terminal_result, 1U);
    auto written = operations_.write(current.revision, terminal);
    if (!written.ok()) {
        return Result<OperationRecord>::failure(written.error());
    }
    logger_.write(ServiceLogEvent{
        ServiceLogKind::operation_terminal,
        terminal.request_id,
        terminal.operation_id,
        terminal.profile_id,
        terminal.command_scope,
        terminal.phase});
    return Result<OperationRecord>::success(std::move(terminal));
}

Result<OperationRecord> ServiceCore::finalize_operation(
    const OperationRecord& current,
    const std::optional<ManagedProfileSnapshot>& current_profile,
    const ActionPlan& plan,
    CompletionOutcome completion,
    OperationState terminal_state,
    std::vector<std::byte> terminal_result,
    bool preserve_profile_truth) {
    const bool idempotent_noop = plan.disposition
        != TransitionDisposition::allowed;
    const bool stop_command = plan.command == ManagementCommand::stop
        || plan.command == ManagementCommand::force_stop;
    auto final_profile = current_profile;
    if (preserve_profile_truth
        && completion != CompletionOutcome::commit_indeterminate
        && completion
            != CompletionOutcome::committed_needs_reconciliation_clean
        && completion
            != CompletionOutcome::committed_needs_reconciliation_dirty) {
        return Result<OperationRecord>::failure(service_error(
            ServiceCoreError::operation_invalid,
            L"service_core.preserve_profile_truth",
            L"Only unresolved durable truth may retain the profile gate"));
    }
    if (!preserve_profile_truth
        && !idempotent_noop && current_profile.has_value()) {
        const auto completed = complete_command(plan, completion);
        if (!completed.applied()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.complete",
                L"The worker outcome is invalid for the management command"));
        }
        auto proposed_state = completed.after;
        if (stop_command
            && terminal_state != OperationState::succeeded
            && current_profile->stop.phase == StopDurablePhase::stable) {
            proposed_state = current_profile->state;
        }
        if (current_profile->record.profile.lifecycle
                == profile::ProfileLifecycle::ready
            && proposed_state.lifecycle
                == profile::ProfileLifecycle::provisioning) {
            proposed_state.lifecycle = profile::ProfileLifecycle::ready;
        }
        const auto final_stop_truth = stop_command
                && terminal_state == OperationState::succeeded
            ? current_profile->stop
            : StopDurableState{};
        auto published = stop_command
            ? profiles_.publish_stop(
                current_profile.value(), proposed_state,
                current_profile->operation_gate,
                current_profile->checkpoint,
                current_profile->compaction, final_stop_truth)
            : profiles_.publish_runtime(
                current_profile.value(), proposed_state,
                current_profile->operation_gate);
        if (!published.ok()) {
            return Result<OperationRecord>::failure(published.error());
        }
        final_profile = published.move_value();
    } else if (!preserve_profile_truth && !idempotent_noop) {
        const auto completed = complete_command(plan, completion);
        if (!completed.applied()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.complete_absent",
                L"The profile-absent worker outcome is invalid"));
        }
    }

    auto terminal = current;
    ++terminal.revision;
    terminal.state = terminal_state;
    terminal.phase = preserve_profile_truth
        ? "terminal-indeterminate" : "terminal";
    terminal.updated_unix_ms = (std::max)(
        terminal.updated_unix_ms, clock_.unix_time_ms());
    terminal.terminal_unix_ms = terminal.updated_unix_ms;
    terminal.cancellable = false;
    if (idempotent_noop) {
        terminal_result = durable_noop_result(plan.disposition);
    }
    terminal.terminal_result = nonempty_terminal_result(
        std::move(terminal_result),
        terminal_state == OperationState::succeeded);
    auto terminal_write = operations_.write(current.revision, terminal);
    if (!terminal_write.ok()) {
        return Result<OperationRecord>::failure(terminal_write.error());
    }

    if (!preserve_profile_truth
        && !idempotent_noop && final_profile.has_value()) {
        const auto released = release_operation_gate(
            final_profile->operation_gate, terminal.operation_id);
        if (!released.accepted()) {
            return Result<OperationRecord>::failure(service_error(
                ServiceCoreError::operation_invalid,
                L"service_core.release_gate",
                L"The profile operation gate could not be released"));
        }
        auto published = stop_command
            ? profiles_.publish_stop(
                final_profile.value(), final_profile->state, released.after,
                final_profile->checkpoint, final_profile->compaction,
                StopDurableState{})
            : profiles_.publish_runtime(
                final_profile.value(), final_profile->state, released.after);
        if (!published.ok()) {
            return Result<OperationRecord>::failure(published.error());
        }
    }

    logger_.write(ServiceLogEvent{
        ServiceLogKind::operation_terminal,
        terminal.request_id,
        terminal.operation_id,
        terminal.profile_id,
        terminal.command_scope,
        terminal.phase});
    {
        std::scoped_lock lock(pending_mutex_);
        const auto pending = std::find_if(
            pending_.begin(), pending_.end(), [&](const auto& candidate) {
                return candidate.operation_id == terminal.operation_id;
            });
        if (pending != pending_.end()) pending_.erase(pending);
    }
    return Result<OperationRecord>::success(std::move(terminal));
}

Result<std::vector<OperationRecord>> ServiceCore::recover_after_restart() {
    {
        std::scoped_lock lock(pending_mutex_);
        pending_.clear();
    }
    auto records = operations_.reconcile_after_restart(clock_.unix_time_ms());
    if (!records.ok()) return records;
    for (auto& record : records.value()) {
        if (is_terminal(record.state)
            && record.phase != "terminal-indeterminate") {
            auto profile = profiles_.load(record.profile_id);
            if (!profile.ok()) {
                const bool lifecycle_without_profile =
                    record.command_scope == "disk.create"
                    || record.command_scope == "disk.import";
                if (!lifecycle_without_profile
                    && !is_profile_not_found(profile.error())) {
                    return Result<std::vector<OperationRecord>>::failure(
                        profile.error());
                }
                bool expected_absent_forget = false;
                if (record.command_scope == "disk.forget"
                    && record.state == OperationState::succeeded) {
                    auto tombstone = profiles_.find_forget_tombstone(
                        record.operation_id);
                    if (!tombstone.ok()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            tombstone.error());
                    }
                    expected_absent_forget = tombstone.value().has_value()
                        && tombstone.value()->phase
                            == ForgetTombstonePhase::metadata_removed
                        && tombstone.value()->operation_id
                            == record.operation_id
                        && tombstone.value()->request_id == record.request_id
                        && tombstone.value()->profile_id == record.profile_id
                        && !tombstone.value()->authorization_owner_sid.empty()
                        && tombstone.value()->retained_bundle_identity.has_value()
                        && nonzero_file_identity(
                            tombstone.value()->retained_bundle_identity.value());
                }
                if (record.command_scope != "disk.create"
                    && record.command_scope != "disk.import"
                    && !expected_absent_forget) {
                    return Result<std::vector<OperationRecord>>::failure(
                        profile.error());
                }
            } else if (profile.value().operation_gate.active_operation_id
                       == std::optional<product::Uuid>(
                           record.operation_id)) {
                const auto released = release_operation_gate(
                    profile.value().operation_gate, record.operation_id);
                if (!released.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_release",
                            L"A terminal operation owns an invalid gate"));
                }
                auto published = record.command_scope == "disk.stop"
                    ? profiles_.publish_stop(
                        profile.value(), profile.value().state,
                        released.after, profile.value().checkpoint,
                        profile.value().compaction, StopDurableState{})
                    : profiles_.publish_runtime(
                        profile.value(), profile.value().state,
                        released.after);
                if (!published.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        published.error());
                }
            }
        } else if (!is_terminal(record.state)
                   && record.command_scope == "disk.stop") {
            auto loaded_profile = profiles_.load(record.profile_id);
            if (!loaded_profile.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    loaded_profile.error());
            }
            auto snapshot = loaded_profile.move_value();
            if (snapshot.stop == StopDurableState{}
                && snapshot.operation_gate.active_operation_id
                    != std::optional<product::Uuid>(record.operation_id)) {
                auto terminalized = terminalize_unowned_admission(
                    record, L"service_core.recover_stop_orphan");
                if (!terminalized.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        terminalized.error());
                }
                record = terminalized.move_value();
                continue;
            }
            if (!snapshot.stop.operation_id.has_value()
                || snapshot.stop.operation_id.value()
                    != record.operation_id
                || !snapshot.stop.before_state.has_value()
                || snapshot.record.profile.profile_uuid
                    != record.profile_id
                || snapshot.stop.phase == StopDurablePhase::stable
                || !known_normal_stop_policy(snapshot.stop.policy)
                || snapshot.stop.volatile_data_loss_acknowledged
                    != snapshot.stop.force) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_stop",
                        L"The durable stop reservation is missing or contradictory"));
            }
            const auto command = snapshot.stop.force
                ? ManagementCommand::force_stop
                : ManagementCommand::stop;
            auto payload_matches = operation_payload_matches(
                record,
                control::Command(control::DiskStopCommand{
                    snapshot.stop.target,
                    snapshot.stop.force,
                    snapshot.stop.force}));
            if (!payload_matches.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    payload_matches.error());
            }
            if (!payload_matches.value()) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_stop",
                        L"The durable stop reservation no longer matches the admitted payload"));
            }
            auto plan = plan_command(
                snapshot.stop.before_state.value(), command);
            if (!plan.accepted()
                || plan.disposition != TransitionDisposition::allowed) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::state_rejected,
                        L"service_core.recover_stop",
                        L"The durable stop operation cannot be replanned"));
            }
            const bool owns_gate =
                snapshot.operation_gate.active_operation_id
                    == std::optional<product::Uuid>(record.operation_id)
                && snapshot.operation_gate.active_command == command;
            const bool forward_only =
                snapshot.stop.phase >= StopDurablePhase::removal_boundary
                || snapshot.stop.phase == StopDurablePhase::resolving;
            if (!owns_gate
                || (forward_only
                    && (record.cancellable
                        || snapshot.operation_gate.cancellable))) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_stop",
                        L"The durable stop gate lost its exact ownership or cancellation boundary"));
            }
            if (record.state == OperationState::queued
                || record.state == OperationState::reconciling) {
                auto admitting = record;
                ++admitting.revision;
                admitting.state = OperationState::running;
                admitting.phase = "admitting";
                admitting.updated_unix_ms = (std::max)(
                    admitting.updated_unix_ms, clock_.unix_time_ms());
                auto written = operations_.write(
                    record.revision, admitting);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(admitting);
            } else {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_stop",
                        L"The reconciled stop operation has an invalid state"));
            }
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                record.operation_id,
                record.request_id,
                std::move(plan),
                std::nullopt,
                true});
        } else if (!is_terminal(record.state)
                   && (record.command_scope == "disk.rebind"
                       || record.command_scope
                           == "disk.repair-profile")) {
            auto reservation = profiles_.find_recovery_command(
                record.operation_id);
            if (!reservation.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    reservation.error());
            }
            if (!reservation.value().has_value()) {
                auto terminalized = terminalize_unowned_admission(
                    record, L"service_core.recover_recovery_orphan");
                if (!terminalized.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        terminalized.error());
                }
                record = terminalized.move_value();
                continue;
            }
            const auto expected_kind = record.command_scope == "disk.rebind"
                ? RecoveryOperationKind::rebind
                : RecoveryOperationKind::repair_profile;
            const auto expected_command = expected_kind
                    == RecoveryOperationKind::rebind
                ? ManagementCommand::rebind
                : ManagementCommand::repair_profile;
            const auto& durable = reservation.value().value();
            if (durable.operation_id != record.operation_id
                || durable.request_id != record.request_id
                || durable.intent.kind != expected_kind
                || durable.intent.profile_id != record.profile_id
                || durable.expected_profile.profile.profile_uuid
                    != record.profile_id
                || durable.before_state.lifecycle
                    != std::optional<profile::ProfileLifecycle>(
                        durable.expected_profile.profile.lifecycle)
                || (expected_kind == RecoveryOperationKind::rebind
                    && (!durable.intent.expected_image_id.has_value()
                        || durable.intent.bundle_path.empty()
                        || durable.intent.from_previous))
                || (expected_kind == RecoveryOperationKind::repair_profile
                    && (!durable.intent.from_previous
                        || durable.intent.expected_image_id.has_value()
                        || !durable.intent.bundle_path.empty()))) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_recovery_command",
                        L"The durable recovery reservation does not match the operation"));
            }
            const auto durable_command = expected_kind
                    == RecoveryOperationKind::rebind
                ? control::Command(control::DiskRebindCommand{
                    durable.intent.profile_id,
                    durable.intent.bundle_path,
                    durable.intent.expected_image_id.value()})
                : control::Command(control::DiskRepairProfileCommand{
                    durable.intent.profile_id,
                    durable.intent.from_previous});
            auto payload_matches = operation_payload_matches(
                record, durable_command);
            if (!payload_matches.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    payload_matches.error());
            }
            if (!payload_matches.value()) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_recovery_command",
                        L"The durable recovery reservation no longer matches the admitted payload"));
            }
            auto plan = plan_command(
                durable.before_state, expected_command);
            if (!plan.accepted()
                || plan.disposition != TransitionDisposition::allowed) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::state_rejected,
                        L"service_core.recover_recovery_command",
                        L"The durable recovery command cannot be replanned"));
            }
            const auto completed_profile_state = complete_command(
                plan, CompletionOutcome::succeeded_clean);
            if (!completed_profile_state.applied()) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_recovery_command",
                        L"The recovery command has no canonical success state"));
            }
            auto current = profiles_.load(record.profile_id);
            bool product_truth_published = false;
            if (current.ok()
                && durable.expected_profile.revision
                    != (std::numeric_limits<std::uint64_t>::max)()
                && current.value().record.revision
                    == durable.expected_profile.revision + 1
                && current.value().state == completed_profile_state.after
                && current.value().operation_gate.active_operation_id
                    == std::optional<product::Uuid>(record.operation_id)
                && current.value().operation_gate.active_command
                    == expected_command
                && !current.value().operation_gate.cancellable) {
                if (expected_kind == RecoveryOperationKind::repair_profile) {
                    product_truth_published =
                        current.value().record.profile
                            == durable.expected_profile.profile;
                } else {
                    auto normalized = current.value().record.profile;
                    const bool intended_binding =
                        normalized.bundle_path == durable.intent.bundle_path
                        && durable.intent.expected_image_id.has_value()
                        && normalized.image_uuid
                            == durable.intent.expected_image_id.value();
                    normalized.bundle_path =
                        durable.expected_profile.profile.bundle_path;
                    normalized.manifest_name =
                        durable.expected_profile.profile.manifest_name;
                    product_truth_published = intended_binding
                        && normalized == durable.expected_profile.profile;
                }
            }
            if (product_truth_published) {
                auto finalized = finalize_operation(
                    record, current.value(), plan,
                    CompletionOutcome::succeeded_clean,
                    OperationState::succeeded,
                    {std::byte{'r'}, std::byte{'e'}, std::byte{'c'},
                     std::byte{'o'}, std::byte{'v'}, std::byte{'e'},
                     std::byte{'r'}, std::byte{'e'}, std::byte{'d'}});
                if (!finalized.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        finalized.error());
                }
                record = finalized.move_value();
                logger_.write(ServiceLogEvent{
                    ServiceLogKind::restart_reconcile,
                    record.request_id,
                    record.operation_id,
                    record.profile_id,
                    record.command_scope,
                    "restart-terminalized"});
                continue;
            }
            auto loaded_profile = expected_kind
                    == RecoveryOperationKind::repair_profile
                ? profiles_.load_repair_candidate(record.profile_id)
                : std::move(current);
            if (!loaded_profile.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    loaded_profile.error());
            }
            auto snapshot = loaded_profile.move_value();
            if (snapshot.record != durable.expected_profile) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_recovery_command",
                        L"The recovery target changed after reservation"));
            }
            if (expected_kind == RecoveryOperationKind::rebind) {
                auto gate = snapshot.operation_gate;
                if (gate.active_operation_id
                    != std::optional<product::Uuid>(record.operation_id)) {
                    const auto restored = plan_gate_access(
                        gate, plan, record.operation_id);
                    if (!restored.accepted()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.recover_rebind_gate",
                                L"The rebind operation cannot restore its gate"));
                    }
                    auto published = profiles_.publish_runtime(
                        snapshot, plan.admitted, restored.after);
                    if (!published.ok()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            published.error());
                    }
                    snapshot = published.move_value();
                } else if (gate.active_command != ManagementCommand::rebind
                           || snapshot.state != plan.admitted) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_rebind_gate",
                            L"The durable rebind gate is contradictory"));
                }
            }
            if (record.state == OperationState::queued
                || record.state == OperationState::reconciling) {
                auto admitting = record;
                ++admitting.revision;
                admitting.state = OperationState::running;
                admitting.phase = "admitting";
                admitting.updated_unix_ms = (std::max)(
                    admitting.updated_unix_ms, clock_.unix_time_ms());
                auto written = operations_.write(
                    record.revision, admitting);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(admitting);
            } else {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_recovery_command",
                        L"The reconciled recovery operation has an invalid state"));
            }
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                record.operation_id,
                record.request_id,
                std::move(plan),
                std::nullopt,
                true,
                durable.intent});
        } else if (!is_terminal(record.state)
                   && record.command_scope == "disk.forget") {
            auto loaded_tombstone = profiles_.find_forget_tombstone(
                record.operation_id);
            if (!loaded_tombstone.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    loaded_tombstone.error());
            }
            if (!loaded_tombstone.value().has_value()) {
                auto terminalized = terminalize_unowned_admission(
                    record, L"service_core.recover_forget_orphan");
                if (!terminalized.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        terminalized.error());
                }
                record = terminalized.move_value();
                continue;
            }
            const auto tombstone = loaded_tombstone.value().value();
            if (tombstone.operation_id != record.operation_id
                || tombstone.request_id != record.request_id
                || tombstone.profile_id != record.profile_id
                || tombstone.before_state.lifecycle == std::nullopt
                || tombstone.authorization_owner_sid.empty()
                || !tombstone.retained_bundle_identity.has_value()
                || !nonzero_file_identity(
                    tombstone.retained_bundle_identity.value())) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget",
                        L"The durable forget tombstone does not match the operation"));
            }
            auto payload_matches = operation_payload_matches(
                record,
                control::Command(control::DiskForgetCommand{
                    tombstone.profile_id,
                    tombstone.profile_id}));
            if (!payload_matches.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    payload_matches.error());
            }
            if (!payload_matches.value()) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget",
                        L"The durable forget tombstone no longer matches the admitted payload"));
            }
            auto plan = plan_command(
                tombstone.before_state, ManagementCommand::forget);
            if (!plan.accepted()
                || plan.disposition != TransitionDisposition::allowed) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::state_rejected,
                        L"service_core.recover_forget",
                        L"The durable forget operation cannot be replanned"));
            }
            if (tombstone.phase
                == ForgetTombstonePhase::metadata_removed) {
                auto unexpectedly_present = profiles_.load(
                    record.profile_id);
                if (unexpectedly_present.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_forget",
                            L"Forget metadata is marked removed but the profile is still present"));
                }
                if (!is_profile_not_found(unexpectedly_present.error())) {
                    return Result<std::vector<OperationRecord>>::failure(
                        unexpectedly_present.error());
                }
                auto finalized = finalize_operation(
                    record, std::nullopt, plan,
                    CompletionOutcome::succeeded_clean,
                    OperationState::succeeded,
                    forget_terminal_result(tombstone));
                if (!finalized.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        finalized.error());
                }
                record = finalized.move_value();
                logger_.write(ServiceLogEvent{
                    ServiceLogKind::restart_reconcile,
                    record.request_id,
                    record.operation_id,
                    record.profile_id,
                    record.command_scope,
                    "restart-terminalized"});
                continue;
            }
            if (tombstone.phase != ForgetTombstonePhase::reserved) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget",
                        L"The durable forget phase is unknown"));
            }
            auto loaded_profile = profiles_.load(record.profile_id);
            if (!loaded_profile.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    loaded_profile.error());
            }
            auto snapshot = loaded_profile.move_value();
            if (snapshot.record.profile.profile_uuid
                    != tombstone.profile_id
                || snapshot.record.profile.image_uuid
                    != tombstone.image_id
                || snapshot.record.revision
                    != tombstone.profile_revision
                || snapshot.record.profile.bundle_path
                    != tombstone.retained_bundle_path
                || snapshot.record.profile.owner_sid
                    != tombstone.authorization_owner_sid
                || snapshot.checkpoint.confirmed_generation
                    != tombstone.confirmed_generation
                || snapshot.state != plan.admitted) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget",
                        L"The forget target changed after its tombstone was reserved"));
            }
            auto gate = snapshot.operation_gate;
            if (gate.active_operation_id
                != std::optional<product::Uuid>(record.operation_id)) {
                auto restored = plan_gate_access(
                    gate, plan, record.operation_id);
                if (!restored.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_forget_gate",
                            L"The forget operation cannot restore its gate"));
                }
                gate = restored.after;
            } else if (gate.active_command != ManagementCommand::forget) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget_gate",
                        L"Another command owns the forget gate"));
            }
            if (!record.cancellable && gate.cancellable) {
                auto noncancellable = mark_operation_noncancellable(
                    gate, record.operation_id);
                if (!noncancellable.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_forget_gate",
                            L"The forget cancellation boundary cannot be restored"));
                }
                gate = noncancellable.after;
            } else if (record.cancellable && !gate.cancellable) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget_gate",
                        L"The forget cancellation decision is contradictory"));
            }
            if (gate != snapshot.operation_gate) {
                auto published = profiles_.publish_runtime(
                    snapshot, snapshot.state, gate);
                if (!published.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        published.error());
                }
            }
            if (record.state == OperationState::queued
                || record.state == OperationState::reconciling) {
                auto admitting = record;
                ++admitting.revision;
                admitting.state = OperationState::running;
                admitting.phase = "admitting";
                admitting.updated_unix_ms = (std::max)(
                    admitting.updated_unix_ms, clock_.unix_time_ms());
                auto written = operations_.write(
                    record.revision, admitting);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(admitting);
            } else {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_forget",
                        L"The reconciled forget operation has an invalid state"));
            }
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                record.operation_id,
                record.request_id,
                std::move(plan),
                std::nullopt,
                true,
                RecoveryOperationIntent{
                    RecoveryOperationKind::forget,
                    tombstone.profile_id,
                    {},
                    std::nullopt,
                    tombstone.profile_id,
                    false}});
        } else if (!is_terminal(record.state)
                   && record.command_scope == "disk.checkpoint") {
            auto checkpoint_profile = profiles_.load(record.profile_id);
            if (!checkpoint_profile.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    checkpoint_profile.error());
            }
            auto snapshot = checkpoint_profile.move_value();
            bool owns_gate =
                snapshot.operation_gate.active_operation_id
                    == std::optional<product::Uuid>(record.operation_id)
                && snapshot.operation_gate.active_command
                    == ManagementCommand::checkpoint;
            const bool post_boundary =
                snapshot.checkpoint.phase
                    != CheckpointDurablePhase::admitted
                || snapshot.checkpoint.correlation.has_value();
            const bool operation_first_state_matches = [&] {
                if (!snapshot.checkpoint.before_state.has_value()) {
                    return false;
                }
                const auto plan = plan_command(
                    snapshot.checkpoint.before_state.value(),
                    ManagementCommand::checkpoint);
                return plan.accepted()
                    && plan.disposition == TransitionDisposition::allowed
                    && snapshot.state == plan.admitted;
            }();
            // The operation record is persisted first at P4. Only this exact
            // shape proves that the profile CAS was the missing half; every
            // other non-cancellable pre-boundary record fails closed.
            const bool operation_first_boundary =
                !post_boundary
                && !record.cancellable
                && !record.cancel_requested
                && record.state == OperationState::reconciling
                && record.phase == "checkpoint-commit-boundary"
                && owns_gate
                && snapshot.operation_gate.cancellable
                && operation_first_state_matches
                && snapshot.checkpoint.before_state.has_value()
                && snapshot.record.profile.profile_uuid == record.profile_id
                && !is_zero_uuid(snapshot.record.profile.image_uuid)
                && snapshot.checkpoint.phase
                    == CheckpointDurablePhase::admitted
                && !snapshot.checkpoint.correlation.has_value();
            const bool unowned_orphan =
                !owns_gate
                && !snapshot.checkpoint.before_state.has_value();
            if (!unowned_orphan && post_boundary) {
                if (!snapshot.checkpoint.correlation.has_value()
                    || record.cancellable
                    || (owns_gate
                        && snapshot.operation_gate.cancellable)) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_checkpoint_gate",
                            L"Post-boundary checkpoint ownership is incomplete"));
                }
                const auto& correlation =
                    snapshot.checkpoint.correlation.value();
                if (correlation.product_operation_id
                        != record.operation_id
                    || correlation.profile_id != record.profile_id
                    || correlation.image_id
                        != snapshot.record.profile.image_uuid) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_checkpoint_gate",
                            L"Post-boundary checkpoint correlation changed identity"));
                }
            } else if (!unowned_orphan
                       && !record.cancellable
                       && !operation_first_boundary) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.restore_checkpoint_gate",
                        L"Pre-boundary checkpoint lost its cancellation decision"));
            }
            if (!owns_gate
                && snapshot.checkpoint.before_state.has_value()) {
                auto restore_plan = plan_command(
                    snapshot.checkpoint.before_state.value(),
                    ManagementCommand::checkpoint);
                if (!restore_plan.accepted()
                    || restore_plan.disposition
                        != TransitionDisposition::allowed) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::state_rejected,
                            L"service_core.restore_checkpoint_gate",
                            L"The durable checkpoint cannot restore its gate"));
                }
                auto restored_gate = plan_gate_access(
                    snapshot.operation_gate, restore_plan,
                    record.operation_id);
                if (!restored_gate.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_checkpoint_gate",
                            L"Another operation owns the checkpoint profile"));
                }
                if (post_boundary) {
                    restored_gate = mark_operation_noncancellable(
                        restored_gate.after, record.operation_id);
                    if (!restored_gate.accepted()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.restore_checkpoint_gate",
                                L"The restored checkpoint gate cannot preserve P4"));
                    }
                }
                auto restored_profile = profiles_.publish_checkpoint(
                    snapshot, snapshot.state, restored_gate.after,
                    snapshot.checkpoint);
                if (!restored_profile.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        restored_profile.error());
                }
                snapshot = restored_profile.move_value();
                owns_gate = true;
            }
            if (owns_gate && operation_first_boundary) {
                const auto repaired_gate = mark_operation_noncancellable(
                    snapshot.operation_gate, record.operation_id);
                if (!repaired_gate.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_checkpoint_gate",
                            L"The operation-first checkpoint gate cannot be repaired"));
                }
                auto repaired_profile = profiles_.publish_checkpoint(
                    snapshot, snapshot.state, repaired_gate.after,
                    snapshot.checkpoint);
                if (!repaired_profile.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        repaired_profile.error());
                }
                snapshot = repaired_profile.move_value();
            }
            if (!owns_gate) {
                auto orphan = record;
                ++orphan.revision;
                orphan.state = OperationState::failed;
                orphan.phase = "terminal";
                orphan.updated_unix_ms = (std::max)(
                    orphan.updated_unix_ms, clock_.unix_time_ms());
                orphan.terminal_unix_ms = orphan.updated_unix_ms;
                orphan.cancellable = false;
                orphan.terminal_result = {
                    std::byte{'o'}, std::byte{'r'}, std::byte{'p'},
                    std::byte{'h'}, std::byte{'a'}, std::byte{'n'}};
                auto written = operations_.write(record.revision, orphan);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(orphan);
            } else {
                if (!snapshot.checkpoint.before_state.has_value()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_checkpoint",
                            L"The checkpoint gate has no durable pre-operation state"));
                }
                auto plan = plan_command(
                    snapshot.checkpoint.before_state.value(),
                    ManagementCommand::checkpoint);
                if (!plan.accepted()
                    || plan.disposition
                        != TransitionDisposition::allowed) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::state_rejected,
                            L"service_core.recover_checkpoint",
                            L"The durable checkpoint cannot be replanned"));
                }
                if (record.state == OperationState::queued
                    || record.state == OperationState::reconciling) {
                    auto admitting = record;
                    ++admitting.revision;
                    admitting.state = OperationState::running;
                    admitting.phase = "admitting";
                    admitting.updated_unix_ms = (std::max)(
                        admitting.updated_unix_ms,
                        clock_.unix_time_ms());
                    auto written = operations_.write(
                        record.revision, admitting);
                    if (!written.ok()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            written.error());
                    }
                    record = std::move(admitting);
                } else {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_checkpoint",
                            L"The reconciled checkpoint operation has an invalid state"));
                }
                std::scoped_lock lock(pending_mutex_);
                pending_.push_back(PendingExecution{
                    record.operation_id,
                    record.request_id,
                    std::move(plan),
                    std::nullopt,
                    true});
            }
        } else if (!is_terminal(record.state)
                   && record.command_scope == "disk.compact") {
            auto loaded_profile = profiles_.load(record.profile_id);
            if (!loaded_profile.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    loaded_profile.error());
            }
            auto snapshot = loaded_profile.move_value();
            bool owns_gate =
                snapshot.operation_gate.active_operation_id
                    == std::optional<product::Uuid>(record.operation_id)
                && snapshot.operation_gate.active_command
                    == ManagementCommand::compact;
            const bool compaction_post_boundary =
                snapshot.compaction.phase
                        == CompactionDurablePhase::mutation_boundary
                || snapshot.compaction.phase
                        == CompactionDurablePhase::evidence_recorded
                || snapshot.compaction.phase
                        == CompactionDurablePhase::resolving
                || snapshot.compaction.phase
                        == CompactionDurablePhase::
                            committed_pending_epoch_confirmation
                || snapshot.compaction.phase
                        == CompactionDurablePhase::product_truth_published
                || snapshot.compaction.correlation.has_value();
            const bool checkpoint_post_boundary =
                ((snapshot.compaction.phase
                          == CompactionDurablePhase::checkpointing
                      && (snapshot.checkpoint.phase
                              != CheckpointDurablePhase::admitted
                          || snapshot.checkpoint.correlation.has_value()))
                 || snapshot.compaction.phase
                        == CompactionDurablePhase::checkpoint_confirmed
                 || (snapshot.compaction.phase
                            == CompactionDurablePhase::stable
                     && snapshot.compaction
                            .prerequisite_checkpoint_performed))
                && snapshot.checkpoint.correlation.has_value();
            const bool post_boundary = compaction_post_boundary
                || checkpoint_post_boundary;
            const bool operation_first_state_matches = [&] {
                if (!snapshot.compaction.before_state.has_value()) {
                    return false;
                }
                const auto plan = plan_command(
                    snapshot.compaction.before_state.value(),
                    ManagementCommand::compact);
                return plan.accepted()
                    && plan.disposition == TransitionDisposition::allowed
                    && snapshot.state == plan.admitted;
            }();
            // The operation record is persisted first at both mutation
            // boundaries. A crash before the matching profile CAS therefore
            // leaves a deliberately non-cancellable operation with a still
            // pre-boundary profile. Replaying read-only preflight is safe; M3
            // mutation remains impossible until the profile half is durable.
            const bool operation_first_boundary =
                !post_boundary
                && !record.cancellable
                && !record.cancel_requested
                && record.state == OperationState::reconciling
                && owns_gate
                && snapshot.operation_gate.cancellable
                && operation_first_state_matches
                && snapshot.compaction.before_state.has_value()
                && snapshot.record.profile.profile_uuid == record.profile_id
                && !is_zero_uuid(snapshot.record.profile.image_uuid)
                && !snapshot.compaction.correlation.has_value()
                && !snapshot.checkpoint.correlation.has_value()
                && ((record.phase == "checkpoint-commit-boundary"
                        && snapshot.compaction.phase
                            == CompactionDurablePhase::checkpointing
                        && snapshot.checkpoint.phase
                            == CheckpointDurablePhase::admitted)
                    || (record.phase == "compaction-mutation-boundary"
                        && snapshot.compaction.phase
                            == CompactionDurablePhase::admitted));
            if (post_boundary) {
                const bool compaction_identity_changed =
                    compaction_post_boundary
                    && (!snapshot.compaction.correlation.has_value()
                        || snapshot.compaction.correlation
                                ->product_operation_id
                            != record.operation_id
                        || snapshot.compaction.correlation->profile_id
                            != record.profile_id
                        || snapshot.compaction.correlation->image_id
                            != snapshot.record.profile.image_uuid);
                const bool checkpoint_identity_changed =
                    checkpoint_post_boundary
                    && (!snapshot.checkpoint.correlation.has_value()
                        || snapshot.checkpoint.correlation
                                ->product_operation_id
                            != record.operation_id
                        || snapshot.checkpoint.correlation->profile_id
                            != record.profile_id
                        || snapshot.checkpoint.correlation->image_id
                            != snapshot.record.profile.image_uuid);
                if (record.cancellable
                    || compaction_identity_changed
                    || checkpoint_identity_changed) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_compaction_gate",
                            L"Post-boundary compaction ownership or correlation is incomplete"));
                }
            } else if (!record.cancellable && !operation_first_boundary) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.restore_compaction_gate",
                        L"Pre-boundary compaction lost its cancellation decision"));
            }
            if (!owns_gate && snapshot.compaction.before_state.has_value()) {
                auto restore_plan = plan_command(
                    snapshot.compaction.before_state.value(),
                    ManagementCommand::compact);
                if (!restore_plan.accepted()
                    || restore_plan.disposition
                        != TransitionDisposition::allowed) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::state_rejected,
                            L"service_core.restore_compaction_gate",
                            L"The durable compaction cannot restore its gate"));
                }
                auto restored_gate = plan_gate_access(
                    snapshot.operation_gate, restore_plan,
                    record.operation_id);
                if (!restored_gate.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_compaction_gate",
                            L"Another operation owns the compaction profile"));
                }
                if (post_boundary || operation_first_boundary) {
                    restored_gate = mark_operation_noncancellable(
                        restored_gate.after, record.operation_id);
                    if (!restored_gate.accepted()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            service_error(
                                ServiceCoreError::operation_invalid,
                                L"service_core.restore_compaction_gate",
                                L"The restored compaction gate cannot preserve its mutation boundary"));
                    }
                }
                auto restored_profile = profiles_.publish_compaction(
                    snapshot,
                    snapshot.state,
                    restored_gate.after,
                    snapshot.checkpoint,
                    snapshot.compaction);
                if (!restored_profile.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        restored_profile.error());
                }
                snapshot = restored_profile.move_value();
                owns_gate = true;
            }
            if (owns_gate && operation_first_boundary
                && snapshot.operation_gate.cancellable) {
                const auto repaired_gate = mark_operation_noncancellable(
                    snapshot.operation_gate, record.operation_id);
                if (!repaired_gate.accepted()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.restore_compaction_gate",
                            L"The operation-first boundary gate cannot be repaired"));
                }
                auto repaired_profile = profiles_.publish_compaction(
                    snapshot,
                    snapshot.state,
                    repaired_gate.after,
                    snapshot.checkpoint,
                    snapshot.compaction);
                if (!repaired_profile.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        repaired_profile.error());
                }
                snapshot = repaired_profile.move_value();
            }
            if (!owns_gate) {
                auto orphan = record;
                ++orphan.revision;
                orphan.state = OperationState::failed;
                orphan.phase = "terminal";
                orphan.updated_unix_ms = (std::max)(
                    orphan.updated_unix_ms, clock_.unix_time_ms());
                orphan.terminal_unix_ms = orphan.updated_unix_ms;
                orphan.cancellable = false;
                orphan.terminal_result = {
                    std::byte{'o'}, std::byte{'r'}, std::byte{'p'},
                    std::byte{'h'}, std::byte{'a'}, std::byte{'n'}};
                auto written = operations_.write(record.revision, orphan);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(orphan);
            } else {
                if (!snapshot.compaction.before_state.has_value()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_compaction",
                            L"The compaction gate has no durable pre-operation state"));
                }
                auto plan = plan_command(
                    snapshot.compaction.before_state.value(),
                    ManagementCommand::compact);
                if (!plan.accepted()
                    || plan.disposition
                        != TransitionDisposition::allowed) {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::state_rejected,
                            L"service_core.recover_compaction",
                            L"The durable compaction cannot be replanned"));
                }
                if (record.state == OperationState::queued
                    || record.state == OperationState::reconciling) {
                    auto admitting = record;
                    ++admitting.revision;
                    admitting.state = OperationState::running;
                    admitting.phase = "admitting";
                    admitting.updated_unix_ms = (std::max)(
                        admitting.updated_unix_ms,
                        clock_.unix_time_ms());
                    auto written = operations_.write(
                        record.revision, admitting);
                    if (!written.ok()) {
                        return Result<std::vector<OperationRecord>>::failure(
                            written.error());
                    }
                    record = std::move(admitting);
                } else {
                    return Result<std::vector<OperationRecord>>::failure(
                        service_error(
                            ServiceCoreError::operation_invalid,
                            L"service_core.recover_compaction",
                            L"The reconciled compaction operation has an invalid state"));
                }
                std::scoped_lock lock(pending_mutex_);
                pending_.push_back(PendingExecution{
                    record.operation_id,
                    record.request_id,
                    std::move(plan),
                    std::nullopt,
                    true});
            }
        } else if (!is_terminal(record.state)
                   && (record.command_scope == "disk.create"
                       || record.command_scope == "disk.import")) {
            if (registrations_ == nullptr) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::dependency_failure,
                        L"service_core.recover_registration",
                        L"Registration persistence is unavailable"));
            }
            auto reservation = registrations_->load_reservation(
                record.operation_id);
            if (!reservation.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    reservation.error());
            }
            if (!reservation_matches_operation(
                    reservation.value(), record)) {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_registration",
                        L"The lifecycle reservation does not match the operation"));
            }
            auto plan = plan_lifecycle_reservation(reservation.value());
            if (!plan.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    plan.error());
            }
            if (record.state == OperationState::queued
                || record.state == OperationState::reconciling) {
                auto admitting = record;
                ++admitting.revision;
                admitting.state = OperationState::running;
                admitting.phase = "admitting";
                admitting.updated_unix_ms = (std::max)(
                    admitting.updated_unix_ms, clock_.unix_time_ms());
                auto written = operations_.write(
                    record.revision, admitting);
                if (!written.ok()) {
                    return Result<std::vector<OperationRecord>>::failure(
                        written.error());
                }
                record = std::move(admitting);
            } else {
                return Result<std::vector<OperationRecord>>::failure(
                    service_error(
                        ServiceCoreError::operation_invalid,
                        L"service_core.recover_registration",
                        L"The reconciled lifecycle operation has an invalid state"));
            }
            std::scoped_lock lock(pending_mutex_);
            pending_.push_back(PendingExecution{
                record.operation_id,
                record.request_id,
                plan.move_value(),
                reservation.value().intent,
                true});
        }
        logger_.write(ServiceLogEvent{
            ServiceLogKind::restart_reconcile,
            record.request_id,
            record.operation_id,
            record.profile_id,
            record.command_scope,
            "restart-reconcile"});
    }
    return records;
}

} // namespace myrd::management
