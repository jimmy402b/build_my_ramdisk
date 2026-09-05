#pragma once

#include "src/common/result.h"
#include "src/control/authorization.h"
#include "src/control/command.h"
#include "src/management/operation_store.h"
#include "src/management/state_machine.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::management {

enum class ServiceCoreError : std::uint32_t {
    invalid_request = 0x5601,
    unsupported_command,
    authorization_denied,
    profile_not_found,
    state_rejected,
    operation_invalid,
    wait_protocol_invalid,
    dependency_failure,
};

enum class ClientWaitState : std::uint8_t { active = 0, cancelled };

struct ServiceCommandRequest final {
    std::optional<product::Uuid> request_id;
    control::Command command;
    ipc::CallerIdentity caller;
    bool no_wait{};
    bool dry_run{};
    std::optional<std::uint64_t> wait_timeout_ms;
    ClientWaitState client_wait{ClientWaitState::active};
};

enum class CheckpointCommitDisposition : std::uint8_t {
    not_committed = 0,
    commit_indeterminate,
    committed,
    committed_needs_reconciliation,
};

enum class CheckpointDurablePhase : std::uint8_t {
    stable = 0,
    admitted,
    commit_boundary,
    evidence_recorded,
    committed_pending_epoch_confirmation,
    resolving,
};

struct CheckpointCommitBoundaryEvidence final {
    product::Uuid product_operation_id;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint32_t worker_pid{};
    std::uint64_t worker_process_creation_time{};
    std::uint64_t worker_session_generation{};
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t cut_dirty_epoch{};
    std::optional<std::uint64_t> native_m3_operation_id;

    bool operator==(const CheckpointCommitBoundaryEvidence&) const noexcept =
        default;
};

struct CheckpointPersistenceErrorContext final {
    std::string operation;
    std::uint32_t code{};
    std::uint8_t phase{};
    std::uint32_t native_code{};
    std::uint64_t native_operation_id{};
    std::array<std::byte, 16> image_uuid{};
    std::uint64_t generation{};
    CheckpointCommitDisposition disposition{
        CheckpointCommitDisposition::not_committed};
    std::string safe_context;

    bool operator==(const CheckpointPersistenceErrorContext&) const noexcept =
        default;
};

struct CheckpointFailureEvidence final {
    ErrorDomain domain{ErrorDomain::persistence};
    std::uint32_t code{};
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code{};
    CheckpointPersistenceErrorContext context;

    bool operator==(const CheckpointFailureEvidence&) const noexcept = default;
};

struct CheckpointReconciliationFailureEvidence final {
    ErrorDomain domain{ErrorDomain::internal};
    std::uint32_t code{};
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code{};
    std::optional<CheckpointCommitDisposition> unresolved_disposition;
    std::string safe_context;

    bool operator==(
        const CheckpointReconciliationFailureEvidence&) const noexcept =
        default;
};

struct CheckpointDurableState final {
    std::uint64_t confirmed_generation{};
    std::uint64_t current_dirty_epoch{};
    CheckpointDurablePhase phase{CheckpointDurablePhase::stable};
    std::optional<ProfileState> before_state;
    std::optional<CheckpointCommitBoundaryEvidence> correlation;
    std::optional<CheckpointCommitDisposition> disposition;
    std::optional<CheckpointFailureEvidence> failure;
    std::optional<CheckpointReconciliationFailureEvidence>
        reconciliation_failure;

    bool operator==(const CheckpointDurableState&) const noexcept = default;
};

enum class CompactionCommitDisposition : std::uint8_t {
    not_committed = 0,
    commit_indeterminate,
    committed,
    committed_needs_reconciliation,
};

enum class CompactionDurablePhase : std::uint8_t {
    stable = 0,
    admitted,
    checkpointing,
    checkpoint_confirmed,
    preflight_verified,
    mutation_boundary,
    evidence_recorded,
    committed_pending_epoch_confirmation,
    resolving,
    product_truth_published,
};

struct CompactionMutationBoundaryEvidence final {
    product::Uuid product_operation_id;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint32_t worker_pid{};
    std::uint64_t worker_process_creation_time{};
    std::uint64_t worker_session_generation{};
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t cut_dirty_epoch{};
    std::uint64_t source_base_bytes{};
    std::uint64_t source_journal_bytes{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    std::optional<std::uint64_t> native_m3_operation_id;
    bool cut_dirty_epoch_known{};

    bool operator==(const CompactionMutationBoundaryEvidence&) const noexcept =
        default;
};

struct CompactionMetrics final {
    std::uint64_t source_generation{};
    std::uint64_t committed_generation{};
    std::uint64_t source_base_bytes{};
    std::uint64_t source_journal_bytes{};
    std::uint64_t candidate_base_bytes{};
    std::uint64_t candidate_journal_bytes{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    std::uint64_t live_extent_count{};
    std::uint64_t candidate_payloads{};
    std::uint64_t snapshot_records{};
    std::uint64_t data_records{};
    std::uint64_t zero_records{};
    std::uint64_t discard_records{};
    std::uint64_t index_nodes{};
    std::uint64_t newer_dirty_preserved{};
    std::uint64_t retained_candidate_count{};
    std::uint64_t deleted_candidate_count{};

    bool operator==(const CompactionMetrics&) const noexcept = default;
};

struct CompactionPersistenceErrorContext final {
    std::string operation;
    std::uint32_t code{};
    std::uint8_t phase{};
    std::uint32_t native_code{};
    std::uint64_t native_operation_id{};
    std::array<std::byte, 16> image_uuid{};
    std::uint64_t generation{};
    CompactionCommitDisposition disposition{
        CompactionCommitDisposition::not_committed};
    std::string safe_context;

    bool operator==(const CompactionPersistenceErrorContext&) const noexcept =
        default;
};

struct CompactionFailureEvidence final {
    ErrorDomain domain{ErrorDomain::persistence};
    std::uint32_t code{};
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code{};
    CompactionPersistenceErrorContext context;

    bool operator==(const CompactionFailureEvidence&) const noexcept = default;
};

struct CompactionReconciliationFailureEvidence final {
    ErrorDomain domain{ErrorDomain::internal};
    std::uint32_t code{};
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code{};
    std::optional<CompactionCommitDisposition> unresolved_disposition;
    std::string safe_context;

    bool operator==(
        const CompactionReconciliationFailureEvidence&) const noexcept =
        default;
};

struct CompactionPreflightFailureEvidence final {
    product::Uuid product_operation_id;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint64_t source_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t required_peak_free_bytes{};
    std::uint64_t available_free_bytes{};
    bool future_checkpoint_possible{};

    bool operator==(
        const CompactionPreflightFailureEvidence&) const noexcept = default;
};

struct CompactionDurableState final {
    CompactionDurablePhase phase{CompactionDurablePhase::stable};
    std::optional<ProfileState> before_state;
    std::optional<CompactionMutationBoundaryEvidence> correlation;
    std::optional<CompactionCommitDisposition> disposition;
    std::optional<CompactionMetrics> metrics;
    std::optional<CompactionFailureEvidence> failure;
    std::optional<CompactionReconciliationFailureEvidence>
        reconciliation_failure;
    std::optional<CompactionPreflightFailureEvidence> preflight_failure;
    bool prerequisite_checkpoint_performed{};
    std::uint64_t prerequisite_checkpoint_generation{};
    std::optional<CheckpointCommitDisposition>
        prerequisite_checkpoint_disposition;
    std::uint64_t successful_compactions_total{};
    std::uint64_t checkpoints_since_compaction{};
    std::optional<product::Uuid> last_counted_checkpoint_operation_id;
    std::optional<product::Uuid> last_published_compaction_operation_id;

    bool operator==(const CompactionDurableState&) const noexcept = default;
};

enum class StopDurablePhase : std::uint8_t {
    stable = 0,
    admitted,
    quiesced,
    volume_flushed,
    checkpoint_confirmed,
    removal_boundary,
    mount_removed,
    device_removed,
    runtime_stopped,
    resolving,
};

struct StopDurableState final {
    StopDurablePhase phase{StopDurablePhase::stable};
    std::optional<product::Uuid> operation_id;
    std::optional<ProfileState> before_state;
    std::wstring target;
    profile::NormalStopPolicy policy{
        profile::NormalStopPolicy::checkpoint_required};
    bool force{};
    bool volatile_data_loss_acknowledged{};
    bool volatile_data_loss_possible{};
    std::uint64_t confirmed_generation_before{};
    std::uint64_t dirty_epoch_before{};

    bool operator==(const StopDurableState&) const noexcept = default;
};

struct BundleFileIdentity final {
    std::uint64_t volume_serial{};
    std::array<std::byte, 16> file_id{};
    bool operator==(const BundleFileIdentity&) const noexcept = default;
};

struct ForgetSafetyEvidence final {
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint64_t profile_revision{};
    bool worker_absent{};
    bool writer_lock_absent{};
    bool matching_device_absent{};
    bool bundle_present{};
    std::optional<BundleFileIdentity> retained_bundle_identity;

    bool operator==(const ForgetSafetyEvidence&) const noexcept = default;
};

enum class ForgetTombstonePhase : std::uint8_t {
    reserved = 0,
    removal_started,
    metadata_removed,
};

struct ForgetTombstone final {
    product::Uuid operation_id;
    product::Uuid request_id;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint64_t profile_revision{};
    std::uint64_t confirmed_generation{};
    std::wstring retained_bundle_path;
    std::wstring authorization_owner_sid;
    std::optional<BundleFileIdentity> retained_bundle_identity;
    ProfileState before_state;
    ForgetTombstonePhase phase{ForgetTombstonePhase::reserved};

    bool operator==(const ForgetTombstone&) const noexcept = default;
};

// This capability intentionally contains no filesystem path or profile
// payload. Implementations may compare and remove only the exact metadata
// identities represented here; bundle retention is proved separately.
struct ForgetMetadataRemovalRequest final {
    product::Uuid operation_id;
    product::Uuid request_id;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint64_t profile_revision{};
    ProfileState expected_state;
    OperationGateState expected_gate;

    bool operator==(const ForgetMetadataRemovalRequest&) const noexcept =
        default;
};

struct ManagedProfileSnapshot final {
    profile::ProfileRecord record;
    ProfileState state;
    OperationGateState operation_gate;
    CheckpointDurableState checkpoint;
    CompactionDurableState compaction;
    StopDurableState stop;

    bool operator==(const ManagedProfileSnapshot&) const noexcept = default;
};

inline constexpr std::uint32_t kProductLogicalSectorBytes = 512;
inline constexpr std::uint32_t kProductPhysicalSectorBytes = 4096;
inline constexpr std::uint64_t kProductPayloadBlockBytes = 2ULL << 20;
inline constexpr std::uint64_t kAutomaticSnapshotCowLimitBytes =
    65ULL * profile::kProfileMemoryAlignmentBytes;

struct PhysicalMemorySnapshot final {
    std::uint64_t total_bytes{};
    std::uint64_t available_bytes{};
};

enum class LifecycleOperationKind : std::uint8_t {
    create = 0,
    import,
};

struct LifecycleOperationIntent final {
    LifecycleOperationKind kind{LifecycleOperationKind::create};
    product::Uuid profile_id;
    std::optional<product::Uuid> expected_image_id;
    std::wstring owner_sid;
    std::wstring display_name;
    std::wstring bundle_path;
    std::wstring mount_target;
    std::uint64_t requested_capacity_bytes{};
    bool auto_start{};
    control::MemoryMode memory_mode{control::MemoryMode::automatic};
    std::optional<std::uint64_t> memory_hard_limit_bytes;
    std::optional<product::CheckpointInterval> checkpoint_interval;
    std::optional<control::OptionalSize> dirty_threshold;
    PhysicalMemorySnapshot physical_memory;
    std::uint64_t created_unix_ms{};
};

enum class RecoveryOperationKind : std::uint8_t {
    repair_profile = 0,
    rebind,
    forget,
};

struct RecoveryOperationIntent final {
    RecoveryOperationKind kind{RecoveryOperationKind::repair_profile};
    product::Uuid profile_id;
    std::wstring bundle_path;
    std::optional<product::Uuid> expected_image_id;
    std::optional<product::Uuid> confirmation_id;
    bool from_previous{};

    bool operator==(const RecoveryOperationIntent&) const noexcept = default;
};

struct RecoveryCommandReservation final {
    product::Uuid operation_id;
    product::Uuid request_id;
    RecoveryOperationIntent intent;
    ProfileState before_state;
    profile::ProfileRecord expected_profile;

    bool operator==(const RecoveryCommandReservation&) const noexcept = default;
};

enum class BundleEvidenceSource : std::uint8_t {
    created_and_verified = 0,
    read_only_inspection,
};

struct BundleRegistrationEvidence final {
    product::Uuid image_id;
    std::string manifest_name;
    std::uint64_t logical_capacity_bytes{};
    std::uint32_t logical_sector_bytes{};
    std::uint32_t physical_sector_bytes{};
    std::uint64_t payload_block_bytes{};
    BundleFileIdentity file_identity;
    BundleEvidenceSource source{BundleEvidenceSource::created_and_verified};
    bool operator==(const BundleRegistrationEvidence&) const noexcept = default;
};

struct VolumeRegistrationEvidence final {
    product::Uuid gpt_disk_id;
    product::Uuid partition_id;
    product::Uuid volume_id;
    bool operator==(const VolumeRegistrationEvidence&) const noexcept = default;
};

struct WorkerLifecycleEvidence final {
    std::optional<BundleRegistrationEvidence> bundle;
    std::optional<VolumeRegistrationEvidence> volume;
    bool first_checkpoint_acknowledged{};
};

enum class LifecycleReservationPhase : std::uint8_t {
    reserved = 0,
    bundle_action_started,
    bundle_verified,
    inspection_action_started,
    inspection_verified,
    profile_published,
    volume_identity_recorded,
    ready_published,
};

struct LifecycleRegistrationReservation final {
    std::uint64_t revision{1};
    product::Uuid operation_id;
    product::Uuid request_id;
    std::array<std::byte, 32> argument_digest{};
    LifecycleOperationIntent intent;
    LifecycleReservationPhase phase{LifecycleReservationPhase::reserved};
    std::optional<BundleRegistrationEvidence> bundle;
    std::optional<VolumeRegistrationEvidence> volume;
};

enum class RegistrationCommitDisposition : std::uint8_t {
    committed = 0,
    not_committed,
    indeterminate,
};

struct RegistrationMutationResult final {
    RegistrationCommitDisposition disposition{
        RegistrationCommitDisposition::not_committed};
    std::optional<ManagedProfileSnapshot> snapshot;
};

enum class RegistrationResolutionDisposition : std::uint8_t {
    absent = 0,
    exact,
    ambiguous,
};

struct RegistrationResolution final {
    RegistrationResolutionDisposition disposition{
        RegistrationResolutionDisposition::ambiguous};
    std::optional<ManagedProfileSnapshot> snapshot;
};

Result<profile::MemoryPolicy> materialize_memory_policy(
    control::MemoryMode mode,
    std::optional<std::uint64_t> requested_hard_limit_bytes,
    std::uint64_t logical_capacity_bytes,
    PhysicalMemorySnapshot physical_memory);

enum class ServiceResponseDisposition : std::uint8_t {
    completed = 0,
    operation_queued,
    dry_run,
    wait_timeout,
    client_wait_cancelled,
};

struct ServiceResponse final {
    product::Uuid request_id;
    std::string command;
    ServiceResponseDisposition disposition{ServiceResponseDisposition::completed};
    std::optional<product::Uuid> operation_id;
    std::optional<OperationRecord> operation;
    std::optional<ManagedProfileSnapshot> profile;
    std::optional<ActionPlan> dry_run_plan;
    std::vector<control::IdempotentWarning> warnings;
};

enum class OperationWaitDisposition : std::uint8_t {
    changed = 0,
    timed_out,
};

struct OperationWaitObservation final {
    OperationWaitDisposition disposition{OperationWaitDisposition::timed_out};
    std::optional<OperationRecord> record;
};

struct WorkerActionRequest final {
    product::Uuid request_id;
    product::Uuid operation_id;
    product::Uuid profile_id;
    ManagementCommand command;
    ActionKind action;
    std::optional<LifecycleOperationIntent> lifecycle_intent;
    std::optional<profile::ProfileRecord> durable_profile;
    std::optional<RecoveryOperationIntent> recovery_intent;
};

struct WorkerCheckpointEvidence final {
    CheckpointCommitBoundaryEvidence correlation;
    CheckpointCommitDisposition disposition{
        CheckpointCommitDisposition::not_committed};
    std::uint64_t generation{};
    std::uint64_t observed_dirty_epoch{};
    std::optional<Error> error;
    std::optional<CheckpointPersistenceErrorContext> error_context;
};

struct WorkerCompactionEvidence final {
    CompactionMutationBoundaryEvidence correlation;
    CompactionCommitDisposition disposition{
        CompactionCommitDisposition::not_committed};
    std::uint64_t generation{};
    std::uint64_t observed_dirty_epoch{};
    CompactionMetrics metrics{};
    std::optional<Error> error;
    std::optional<CompactionPersistenceErrorContext> error_context;
};

using WorkerCompactionPreflightEvidence =
    CompactionPreflightFailureEvidence;

struct WorkerActionResult final {
    CompletionOutcome completion{CompletionOutcome::succeeded_clean};
    std::vector<std::byte> terminal_result;
    WorkerLifecycleEvidence lifecycle;
    std::optional<WorkerCheckpointEvidence> checkpoint;
    std::optional<WorkerCompactionEvidence> compaction{};
    std::optional<WorkerCompactionPreflightEvidence>
        compaction_preflight{};
};

struct CheckpointCommitBoundaryDecision final {
    bool continue_commit{};
    std::uint64_t durable_operation_revision{};
    std::uint64_t durable_profile_revision{};
};

struct CheckpointEpochObservation final {
    CheckpointCommitBoundaryEvidence correlation;
    std::uint64_t live_dirty_epoch{};
};

using CheckpointCommitBoundaryHandler = std::function<
    Result<CheckpointCommitBoundaryDecision>(
        const CheckpointCommitBoundaryEvidence&)>;
using CheckpointEpochPublicationHandler = std::function<
    Result<void>(const CheckpointEpochObservation&)>;

struct CompactionMutationBoundaryDecision final {
    bool continue_compaction{};
    std::uint64_t durable_operation_revision{};
    std::uint64_t durable_profile_revision{};
};

struct CompactionEpochObservation final {
    CompactionMutationBoundaryEvidence correlation;
    std::uint64_t live_dirty_epoch{};
};

using CompactionMutationBoundaryHandler = std::function<
    Result<CompactionMutationBoundaryDecision>(
        const CompactionMutationBoundaryEvidence&)>;
using CompactionCutPublicationHandler =
    CompactionMutationBoundaryHandler;
using CompactionEpochPublicationHandler = std::function<
    Result<void>(const CompactionEpochObservation&)>;

enum class ServiceLogKind : std::uint8_t {
    request_authorized = 0,
    operation_queued,
    operation_running,
    operation_terminal,
    wait_timeout,
    client_wait_cancelled,
    restart_reconcile,
};

struct ServiceLogEvent final {
    ServiceLogKind kind{};
    product::Uuid request_id;
    std::optional<product::Uuid> operation_id;
    std::optional<product::Uuid> profile_id;
    std::string command;
    std::string phase;
};

class IServiceClock {
public:
    virtual ~IServiceClock() noexcept = default;
    virtual std::uint64_t unix_time_ms() const noexcept = 0;
    virtual std::uint64_t monotonic_time_ms() const noexcept = 0;
};

class IServiceProfileStore {
public:
    virtual ~IServiceProfileStore() noexcept = default;
    virtual Result<ManagedProfileSnapshot> resolve(
        std::wstring_view target) = 0;
    virtual Result<ManagedProfileSnapshot> load(
        const product::Uuid& profile_id) = 0;
    virtual Result<ManagedProfileSnapshot> publish_runtime(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate) = 0;
    virtual Result<ManagedProfileSnapshot> publish_checkpoint(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint);
    virtual Result<ManagedProfileSnapshot> publish_compaction(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint,
        const CompactionDurableState& proposed_compaction);
    virtual Result<ManagedProfileSnapshot> publish_stop(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint,
        const CompactionDurableState& proposed_compaction,
        const StopDurableState& proposed_stop);
    // These methods are implemented by the profile/registration composition.
    // Rebind atomically enforces the expected snapshot revision and duplicate
    // bundle identity exclusion. Repair is the only entry that may inspect a
    // corrupt current profile and must delegate to ProfileStore's verified
    // previous-profile publication primitive.
    virtual Result<ManagedProfileSnapshot> publish_rebind(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& verified_bundle,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate);
    virtual Result<ManagedProfileSnapshot> load_repair_candidate(
        const product::Uuid& profile_id);
    virtual Result<ManagedProfileSnapshot> repair_from_previous(
        const ManagedProfileSnapshot& expected_previous,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate);
    virtual Result<RecoveryCommandReservation> reserve_recovery_command(
        const ManagedProfileSnapshot& expected,
        const RecoveryCommandReservation& proposed);
    virtual Result<std::optional<RecoveryCommandReservation>>
    find_recovery_command(
        const product::Uuid& operation_id);
    virtual Result<ForgetTombstone> reserve_forget(
        const ManagedProfileSnapshot& expected,
        const ForgetTombstone& proposed);
    virtual Result<std::optional<ForgetTombstone>> find_forget_tombstone(
        const product::Uuid& operation_id);
    virtual Result<ForgetTombstone> remove_forget_metadata(
        const ForgetMetadataRemovalRequest& request,
        const ForgetSafetyEvidence& proof);
};

class IServiceRegistrationStore {
public:
    virtual ~IServiceRegistrationStore() noexcept = default;
    // Reservations are durable, exact and idempotent by operation ID. They
    // retain the complete command intent even when no public profile exists.
    virtual Result<LifecycleRegistrationReservation> reserve(
        const LifecycleRegistrationReservation& proposed) = 0;
    virtual Result<LifecycleRegistrationReservation> load_reservation(
        const product::Uuid& operation_id) = 0;
    virtual Result<LifecycleRegistrationReservation> update_reservation(
        const LifecycleRegistrationReservation& expected,
        const LifecycleRegistrationReservation& proposed) = 0;
    virtual Result<RegistrationMutationResult> publish_registration(
        const LifecycleOperationIntent& intent,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& bundle,
        const ProfileState& state,
        const OperationGateState& gate) = 0;
    virtual Result<RegistrationMutationResult> update_registration(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const ProfileState& state,
        const OperationGateState& gate) = 0;
    // Resolves commit uncertainty from durable profile/reservation truth.
    // `exact` must return the unique operation-bound snapshot; `absent` proves
    // no profile publication; every other state is `ambiguous`.
    virtual Result<RegistrationResolution> resolve_registration(
        const product::Uuid& operation_id,
        const product::Uuid& profile_id) = 0;
};

class IServiceOperationStore {
public:
    virtual ~IServiceOperationStore() noexcept = default;
    virtual Result<std::optional<OperationAdmission>> find_request(
        const OperationRequest& request) = 0;
    virtual Result<OperationAdmission> admit(
        const OperationRequest& request,
        const product::Uuid& candidate_operation_id) = 0;
    virtual Result<OperationRecord> load(
        const product::Uuid& operation_id) = 0;
    virtual Result<void> write(
        std::uint64_t expected_revision,
        const OperationRecord& proposed) = 0;
    virtual Result<std::vector<OperationRecord>> reconcile_after_restart(
        std::uint64_t now_unix_ms) = 0;
    virtual Result<OperationRecord> request_cancel(
        const product::Uuid& operation_id,
        std::uint64_t now_unix_ms) = 0;
    virtual Result<OperationWaitObservation> wait_for_change(
        const product::Uuid& operation_id,
        std::uint64_t after_revision,
        std::uint64_t monotonic_deadline_ms) = 0;
};

class IWorkerControl {
public:
    virtual ~IWorkerControl() noexcept = default;
    virtual bool begins_noncancellable(ActionKind action) const noexcept = 0;
    virtual Result<WorkerActionResult> execute(
        const WorkerActionRequest& request) = 0;
    virtual Result<WorkerActionResult> execute_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryHandler& boundary);
    virtual Result<WorkerActionResult> reconcile_checkpoint(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation);
    virtual Result<void> publish_checkpoint_epoch(
        const WorkerActionRequest& request,
        const CheckpointCommitBoundaryEvidence& correlation,
        const CheckpointEpochPublicationHandler& publish);
    virtual Result<WorkerActionResult> execute_compaction(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryHandler& boundary,
        const CompactionCutPublicationHandler& cut);
    virtual Result<WorkerActionResult> reconcile_compaction(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryEvidence& correlation);
    virtual Result<void> publish_compaction_epoch(
        const WorkerActionRequest& request,
        const CompactionMutationBoundaryEvidence& correlation,
        const CompactionEpochPublicationHandler& publish);
    virtual Result<std::uint64_t> current_dirty_epoch(
        const product::Uuid& profile_id);
    virtual Result<ForgetSafetyEvidence> prove_forget_safe(
        const WorkerActionRequest& request);
};

class IServiceLogger {
public:
    virtual ~IServiceLogger() noexcept = default;
    virtual void write(const ServiceLogEvent& event) noexcept = 0;
};

class IServiceIdGenerator {
public:
    virtual ~IServiceIdGenerator() noexcept = default;
    virtual Result<product::Uuid> next() = 0;
};

class IPhysicalMemoryQuery {
public:
    virtual ~IPhysicalMemoryQuery() noexcept = default;
    virtual Result<PhysicalMemorySnapshot> query() = 0;
};

class ServiceCore final {
public:
    ServiceCore(
        IServiceClock& clock,
        IServiceProfileStore& profiles,
        IServiceOperationStore& operations,
        IWorkerControl& worker,
        IServiceLogger& logger,
        IServiceIdGenerator& ids,
        IPhysicalMemoryQuery& memory,
        IServiceRegistrationStore* registrations = nullptr) noexcept;

    ServiceCore(const ServiceCore&) = delete;
    ServiceCore& operator=(const ServiceCore&) = delete;

    // Mutating dispatch durably admits work but never runs worker actions on
    // the client request thread. The service scheduler invokes
    // execute_operation, while default CLI waiting is composed from bounded
    // OperationWaitCommand requests; disconnecting that wait cannot cancel
    // server work.
    Result<ServiceResponse> dispatch(const ServiceCommandRequest& request);
    Result<OperationRecord> execute_operation(
        const product::Uuid& operation_id);
    Result<std::vector<OperationRecord>> recover_after_restart();

private:
    struct PendingExecution final {
        product::Uuid operation_id;
        product::Uuid request_id;
        ActionPlan plan;
        std::optional<LifecycleOperationIntent> lifecycle_intent;
        bool restart_recovered{};
        std::optional<RecoveryOperationIntent> recovery_intent;
    };

    Result<OperationRecord> finalize_operation(
        const OperationRecord& current,
        const std::optional<ManagedProfileSnapshot>& current_profile,
        const ActionPlan& plan,
        CompletionOutcome completion,
        OperationState terminal_state,
        std::vector<std::byte> terminal_result,
        bool preserve_profile_truth = false);
    Result<OperationRecord> terminalize_unowned_admission(
        const OperationRecord& current,
        const wchar_t* operation);

    IServiceClock& clock_;
    IServiceProfileStore& profiles_;
    IServiceOperationStore& operations_;
    IWorkerControl& worker_;
    IServiceLogger& logger_;
    IServiceIdGenerator& ids_;
    IPhysicalMemoryQuery& memory_;
    IServiceRegistrationStore* registrations_{};
    std::mutex pending_mutex_;
    std::vector<PendingExecution> pending_;
};

} // namespace myrd::management
