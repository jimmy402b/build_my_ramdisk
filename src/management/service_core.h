#pragma once

#include "src/common/result.h"
#include "src/common/sha256.h"
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

// Moved to the contracts library in T3-01: these codes travel to clients, which have to map
// them to exit codes and sentences.  Re-exported for every existing call site.
using product::ServiceCoreError;

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
    // Appended so every existing aggregate initializer keeps its meaning, and
    // because the recovery correlation cannot be rebuilt without them.  A
    // recovery proof has to judge whether *this* candidate reached the bundle,
    // which means naming the manifest slot it would occupy and the digest of the
    // bytes it would contain -- neither of which the fields above carry.
    //
    // They are also the reason the durable record's schema minor was bumped: a
    // correlation written before that carries neither, and a zero digest must
    // never compare as a match.  The reader leaves them at their defaults for
    // those records and the recovery path refuses on them, so an old record
    // loads and *cannot* be mistaken for a proved one.
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};

    // Whether this correlation can support a recovery proof at all.  A zero
    // digest is the absent value, not a digest that happens to be zero: the
    // caller must treat it as unprovable rather than compare it.
    [[nodiscard]] bool recovery_recoverable() const noexcept {
        return candidate_bytes_digest != Sha256Digest{};
    }

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

// Where a snapshot's ProfileState came from.  A durable service_state.myrs
// record is an observed fact.  The synthesized default that fills in for a
// profile which has never been registered is not: it is a placeholder that
// happens to read as stopped/idle/clean/healthy at generation zero.
enum class ProfileStateProvenance : std::uint8_t {
    durable = 0,
    synthesized,
};

// What the management authority can observe about the worker currently bound
// to a profile.  Deliberately a management-side type: myrd_management_core is
// forbidden from linking the host IPC layer, so the Windows service supplies
// the adapter that maps a host::WorkerRuntimeStatus onto this.  Every field is
// a fact the service holds, not one derived from the durable profile files.
struct WorkerRuntimeObservation final {
    // Whether an authenticated worker for this profile is alive right now.
    // When false, the remaining fields carry no meaning.
    bool worker_alive{};
    std::uint64_t committed_generation{};
    bool dirty{};
    std::uint64_t dirty_epoch{};
    std::optional<product::Uuid> active_operation_id{};
    std::uint32_t active_operation_phase{};
    // **Memory visibility (M5 搂7).**  RAM this profile's runtime holds, and the budget
    // it was opened with.  Both are read from the worker's own storage gauges; zero
    // means zero, and they are meaningless when `worker_alive` is false.
    //
    // Appended so every existing aggregate initializer keeps its meaning.
    std::uint64_t resident_bytes{};
    std::uint64_t budget_bytes{};
    // Bytes this runtime holds that have not reached the journal.  The checkpoint
    // policy's dirty threshold is a byte count, so the observation has to carry one.
    std::uint64_t dirty_bytes{};
    std::uint64_t volume_serial{};

    bool operator==(const WorkerRuntimeObservation&) const noexcept = default;
};

struct ManagedProfileSnapshot final {
    profile::ProfileRecord record;
    ProfileState state;
    OperationGateState operation_gate;
    CheckpointDurableState checkpoint;
    CompactionDurableState compaction;
    StopDurableState stop;
    // Appended with a default so every existing aggregate initialization keeps
    // its meaning: a snapshot built from real durable state is `durable`.
    ProfileStateProvenance state_provenance{ProfileStateProvenance::durable};
    // The live worker facts the read path observed for this profile.  Absent
    // means no observation was taken or none could be taken -- which is the
    // case for every snapshot the store loads from disk, and for every caller
    // that never consults the worker.  Present means the observation is
    // authoritative, including when it reports worker_alive == false.
    std::optional<WorkerRuntimeObservation> runtime_observation;

    // Equality is durable content identity, which is what callers compare to
    // decide whether a snapshot is still current and whether a publication
    // landed.  Both trailing fields are load-time metadata rather than durable
    // content and are deliberately excluded:
    //   * provenance says where the state was loaded from, not what it is, and
    //     is recomputed on every load;
    //   * the runtime observation is a live fact about a process, not part of
    //     the record, so a snapshot the store loaded from disk and the same
    //     snapshot the read path has just annotated are the same content.
    // The synthesized placeholder and a durable record holding identical values
    // are the same content, so folding provenance in would turn a publication
    // that actually landed into a spurious staleness or indeterminacy failure.
    //
    // The cost of that choice, stated plainly: a caller comparing a snapshot
    // against a durable record can no longer tell "no durable record exists"
    // from "a durable record whose content happens to equal the placeholder".
    // The write-CAS in publish_snapshot_state is the place this bites: on a
    // profile whose service state was removed after a create that published
    // exactly initial_durable_state(record)'s content, the CAS accepts and
    // rewrites at revision 1 instead of rejecting as stale.  That is benign --
    // the file was already gone and the rewritten row says what the caller
    // believed it said -- and revision only orders writes within one file.  It
    // is accepted deliberately because the alternative rejects the M4
    // load-then-publish bootstrap's first successful write.
    bool operator==(const ManagedProfileSnapshot& other) const noexcept {
        return record == other.record
            && state == other.state
            && operation_gate == other.operation_gate
            && checkpoint == other.checkpoint
            && compaction == other.compaction
            && stop == other.stop;
    }
};

// The state a snapshot may be *reported* as.  Two things make a snapshot's own
// state unusable as a report:
//
//   * A synthesized snapshot has no durable service state behind it, so its
//     placeholder values are not observations and must not be presented as a
//     healthy, cleanly stopped disk.
//   * A profile the durable record calls `running` must have a live worker.
//     When the read path obtained an authoritative observation and that
//     observation contradicts the record -- no worker alive for a profile
//     recorded as running, or a live worker for one recorded as stopped -- the
//     record is stale or the worker is unaccounted for, and the honest report
//     is that something needs attention rather than either claim.
//
// Every reporting path that carries a ManagedProfileSnapshot -- the wire
// projection and the CLI renderer alike -- routes through here so they fail
// closed identically.
//
// Only the unambiguous liveness contradiction is folded in.  A generation or
// dirty-epoch disagreement between the record and the observation is NOT
// treated as a conflict: a worker legitimately advances its committed
// generation before the service publishes the matching durable state, so
// failing closed on that window would report a healthy in-flight checkpoint as
// needing attention.  Reconciling that lag is M4's job and its semantics are
// unchanged here.
//
// Scope of the remap, so it is not mistaken for more than it is: it sets
// persistence and health, which is what makes aggregate_status() return
// blocked/needs-attention.  It does NOT remap runtime or activity, and the
// projection still carries confirmed_generation as 0 for a synthesized
// snapshot, because neither RuntimeState nor ActivityState nor the fixed-size
// wire projection has an "unknown" encoding -- inventing one would change
// product state semantics (and the wire format) on a read-only path.  So a
// client that reads aggregate gets the fail-closed verdict, while a client
// that reads runtime alone still sees the recorded value.  Callers must treat
// aggregate as the verdict.
ProfileState reported_profile_state(const ManagedProfileSnapshot& snapshot);

inline constexpr std::uint32_t kProductLogicalSectorBytes = 512;
inline constexpr std::uint32_t kProductPhysicalSectorBytes = 4096;
inline constexpr std::uint64_t kProductPayloadBlockBytes = 2ULL << 20;
// The manifest name every product bundle is created with.  It is here rather
// than on the wire for the same reason the two above are: it is a constant both
// ends already hold, and a second copy travelling with every bundle evidence
// message would be a second thing to keep in step.  (The worker's `start()` does
// name it in the private protocol's own literal, because that layer may not
// include this header.)
inline constexpr std::string_view kProductManifestName = "disk.rdm";
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
    // Declared last so every existing positional initializer keeps meaning what it meant (T4-02).
    std::optional<std::wstring> volume_label;
    // Declared last, like its neighbours, so every existing positional initializer keeps its meaning (T4-03).
    std::optional<std::uint32_t> compaction_checkpoint_threshold;
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
    // The generation the bundle stood at when the worker answered the create or
    // inspection.  It is the durable checkpoint's starting point, and for a
    // stopped create it is the only reading there can be: the worker is closed
    // before the profile is published, so asking it again is asking a process
    // that has already answered and left.
    std::uint64_t bundle_generation{};
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
    // Why a commit is *not* known to have landed, when the store knows.  Appended with a
    // default so every existing aggregate initializer keeps its meaning.  Without it an
    // indeterminate disposition is a verdict with no cause anywhere: measured on the
    // guest as a create ending failed (phase=terminal-indeterminate) with only the word
    // register in the log, and no layer able to say which write failed.
    std::optional<Error> error;
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

// Bounded transport projections.  These intentionally omit paths, caller
// identities, and opaque terminal payloads.
struct ServiceOperationProjection final {
    product::Uuid operation_id;
    product::Uuid profile_id;
    OperationState state{OperationState::queued};
    std::string phase;
    std::uint64_t created_unix_ms{};
    std::uint64_t updated_unix_ms{};
    bool cancellable{};
    bool cancel_requested{};
    bool operator==(const ServiceOperationProjection&) const noexcept = default;
};

struct ServiceProfileProjection final {
    product::Uuid profile_id;
    profile::ProfileLifecycle lifecycle{profile::ProfileLifecycle::provisioning};
    product::RuntimeState runtime{product::RuntimeState::stopped};
    product::ActivityState activity{product::ActivityState::idle};
    product::PersistenceState persistence{product::PersistenceState::clean};
    product::HealthState health{product::HealthState::healthy};
    AggregateStatus aggregate{AggregateStatus::stopped_configured};
    std::uint64_t confirmed_generation{};
    // The mount target, in UTF-8, or absent when the profile has none (or when the
    // stored value could not be expressed as UTF-8, which a real Windows path always
    // can).  Appended with a default so every existing aggregate initializer keeps
    // its meaning.
    //
    // It belongs here rather than only on the response because a *list* is useless
    // without it: a user identifies a disk by its drive letter, and the projection is
    // the only shape a client sees.  This is the mount target the user configured --
    // never the bundle path, which is the thing the projection exists to withhold.
    std::optional<std::string> mount_target;
    // **Memory visibility (M5 搂7).**  The two numbers the CLI and the GUI show, and
    // the reason this projection carries them: "memory is under control" is only
    // checkable if a client can see how much RAM the disk holds against its budget.
    // Both come from the worker's own storage gauges through
    // `WorkerRuntimeObservation`; a stopped runtime reports zero for both, which is
    // the truth rather than an omission.
    //
    // Appended with defaults so every existing aggregate initializer keeps its
    // meaning.  No path, no bundle, no host address travels with them.
    std::uint64_t resident_bytes{};
    std::uint64_t budget_bytes{};
    std::uint64_t volume_serial{};
    // Durable compaction counters (T4-03), appended with defaults like their neighbours.
    std::uint64_t successful_compactions{};
    std::uint64_t checkpoints_since_compaction{};
    bool operator==(const ServiceProfileProjection&) const noexcept = default;
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
    std::vector<ManagedProfileSnapshot> profiles;
    // Transport-visible failure projection. Appended to preserve aggregate
    // initialization compatibility with the M4 response contract.
    std::optional<Error> error;
    std::optional<ServiceOperationProjection> operation_projection;
    std::optional<ServiceProfileProjection> profile_projection;
    std::vector<ServiceProfileProjection> profile_projections;
    // Safe mount projection for CLI consumers; never contains bundle paths.
    std::optional<std::string> mount_target;
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
    // Whether the worker still holds writes the commit did not persist.
    //
    // This is the fact the stop decision needs, and it is a *block-level*
    // question, not an ordering one: a successful commit resets the dirty epoch,
    // so comparing the live epoch against the checkpoint's cut epoch was false
    // after every real commit.  `live_dirty_epoch` stays for the durable record;
    // it must not decide cleanliness.
    bool post_commit_dirty{};
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
    // A worker action failed, and this event is where the *reason* is recorded.  Without
    // it an operation that fails says only that it failed: the durable record holds a
    // marker token rather than a cause, the terminal event never carried one, and the
    // client projection cannot invent one.  Measured cost of not having it: a create that
    // failed on the guest could not be diagnosed from the service log, the durable record
    // or the CLI at all.
    operation_failed,
    // A command was refused before it became an operation, and this is where the reason
    // is recorded.  An admitted operation has a durable record and a terminal event; a
    // refused one has neither, so without this the service log would say only that the
    // client was told no.  The first user is the single-disk admission guard.
    admission_refused,
};

struct ServiceLogEvent final {
    ServiceLogKind kind{};
    product::Uuid request_id;
    std::optional<product::Uuid> operation_id;
    std::optional<product::Uuid> profile_id;
    std::string command;
    std::string phase;
    // The reason, for the events that have one.  Appended with a default so every
    // existing aggregate initializer keeps its meaning and its value.
    std::string message;
};

class IServiceClock {
public:
    virtual ~IServiceClock() noexcept = default;
    virtual std::uint64_t unix_time_ms() const noexcept = 0;
    virtual std::uint64_t monotonic_time_ms() const noexcept = 0;
};

// The identity the service acts as when its own policy, rather than a client,
// initiates work: maintenance checkpoints, auto-start on boot.  LocalSystem, spelled
// out because the authorization path parses the SID.
//
// One definition, in the management layer, because two copies of "who the service is"
// is one copy too many: the first one that drifts becomes a policy that silently stops
// being authorized.
[[nodiscard]] ipc::CallerIdentity service_caller_identity();

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
    // M5 query/configuration extensions are appended so existing profile-store
    // implementations retain the vtable slots of the M4 interface.
    virtual Result<std::vector<ManagedProfileSnapshot>> list_profiles();
    virtual Result<ManagedProfileSnapshot> update_profile_record(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const ProfileState& state,
        const OperationGateState& gate);
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
    // `bundle_generation` is **the generation the bundle already stands at when it is
    // published**, reported by the worker that created or inspected it.  It is a parameter
    // rather than something the store derives because the service must not read a bundle at
    // all (execution boundary), while the worker has the number for free.
    //
    // It is not cosmetic: a create's first checkpoint asks to advance from the bundle's
    // current generation, and the boundary check compares that against the durable
    // `confirmed_generation`.  Publishing 0 for a bundle that stands at 1 leaves the two
    // sides permanently one apart, so every create fails at exactly that comparison --
    // measured on the guest as `prior=1 confirmed=0 candidate=2` with the other six terms
    // satisfied.
    virtual Result<RegistrationMutationResult> publish_registration(
        const LifecycleOperationIntent& intent,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& bundle,
        const ProfileState& state,
        const OperationGateState& gate,
        std::uint64_t bundle_generation = 0) = 0;
    // Extended M5 publication boundary.  The default preserves the M4
    // interface for existing test/adaptor implementations; production stores
    // may bind volume identity at the same atomic registration boundary.
    virtual Result<RegistrationMutationResult> publish_registration(
        const LifecycleOperationIntent& intent,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& bundle,
        const std::optional<VolumeRegistrationEvidence>& volume,
        const ProfileState& state,
        const OperationGateState& gate,
        std::uint64_t bundle_generation = 0) {
        (void)volume;
        return publish_registration(intent, proposed, bundle, state, gate,
                                    bundle_generation);
    }
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
    // Dismantles a leftover this service proved is its own.  On the interface rather than
    // reached through `execute`, because the sweep calls it directly and does not drive a
    // plan: the profile it acts on is not being managed at all -- its worker is dead and
    // only its device is left.
    //
    // **Defaulted to a refusal rather than left pure.**  An adapter that cannot dismantle a
    // leftover must say so: a virtual with no implementation would force every test fake to
    // grow a method it has no opinion about, and the tempting way to satisfy that is to
    // return success -- which would have a fake report a residue as cleared.  A default that
    // fails is the one a caller cannot mistake for the real thing.
    virtual Result<WorkerActionResult> reclaim_leftover(
        const WorkerActionRequest& request);
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
    // The live dirty epoch for a profile, when it can be read without waiting.
    // `nullopt` means no epoch was observed *now* -- there is no worker this
    // authority can see, or the adapter is mid-command and the channel must not
    // be queued behind.  It is a weaker claim than an observation and it is not
    // a claim that nothing is dirty; callers must not read it as `clean`.
    virtual Result<std::optional<std::uint64_t>> current_dirty_epoch(
        const product::Uuid& profile_id);
    // Writes this profile's durable image to a standalone VHDX at `destination`.
    // The default refuses: an adapter that cannot produce the file must not
    // report one, and a test fake that has no opinion keeps compiling.
    virtual Result<void> export_bundle(
        const profile::ProfileRecord& profile,
        const std::wstring& destination);
    // Observe the worker bound to this profile so the read path can reconcile
    // what the durable record claims against what is actually running.
    // `nullopt` means no observation was taken -- no adapter is wired, or the
    // profile has no worker this authority can see.  A returned value is
    // authoritative, *including* when it reports worker_alive == false; that is
    // the difference between "not observed" and "observed to be gone", and only
    // the latter may contradict a durable claim.
    //
    // The default observes nothing, so every existing implementation keeps
    // compiling and every caller that does not participate in M5 runtime
    // consistency keeps M4's durable-only reporting exactly as it was.
    virtual Result<std::optional<WorkerRuntimeObservation>> observe_runtime(
        const product::Uuid& profile_id);
    // Strictly observe the worker bound to this profile, for mutation safety.
    // Unlike `observe_runtime`, "no observation" is a failure, not `nullopt`:
    // a caller that decides whether to checkpoint or remove must not treat
    // "could not ask" as "the runtime is clean".  Fails when no session is
    // bound, the status transport fails, or the live runtime serves a different
    // profile.  A returned value has `worker_alive == true` and carries the
    // block-level `dirty` truth.
    virtual Result<WorkerRuntimeObservation> require_runtime_observation(
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

// The command path's one authority over durable service state.
//
// Not a concurrent object: `dispatch` and `execute_operation` read and write
// the profile, operation and registration records through this core, and two
// callers at once would interleave those writes.  The requirement is the same
// one ServiceWorkerControl states and for the same reason -- a lock here would
// have to span a worker round-trip -- and it is met the same way, by the
// serialization the production wiring puts at the pipe door rather than by
// anything in this object.
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

    // **Bring back the disks that were up when this service last had them.**
    //
    // A profile whose durable state says `running` and whose `auto_start` is true was
    // serving a client when the machine went away or when the service was restarted --
    // either way there is no worker for it now, because a worker is a child of this
    // process.  Without this the promise "a power cut loses at most one interval" is
    // only half kept: the bytes would be recovered, and the disk would come back
    // stopped, waiting for a human to start it.
    //
    // Admission only, like every other policy-driven action here: the scheduler is the
    // one executor.  A profile that is already running or mid-transition is skipped
    // rather than raced.
    Result<std::vector<product::Uuid>> admit_auto_starts();

    // Take the next piece of durable work, for the one executor the service
    // runs.  The claim is atomic against every other claim, and an operation is
    // handed out at most once -- a claimed entry is never returned again, so two
    // callers cannot be given the same operation to run.  `nullopt` means there
    // is nothing left to run.
    //
    // A claim is taken once per process lifetime and is not released when the
    // execution returns.  That is deliberate: the alternative is a service that
    // re-runs a permanently failing operation every poll interval forever.  An
    // operation a claimed execution left non-terminal is therefore reconciled by
    // `recover_after_restart`, which is M4's own restart path, rather than by a
    // retry loop here.
    //
    // This is the whole of the scheduler's view into the pending queue.  The
    // queue, its lock and the action plans stay private; no caller can reach
    // them, and none of them is widened by this seam.
    Result<std::optional<product::Uuid>> claim_next_pending_operation();

    // Run an operation this core claimed.  The work is the existing
    // `execute_operation` path; what this adds is the guard that only a claim
    // this core handed out may be discharged here, so an executor that skipped
    // the claim cannot become a second runner for an operation another worker
    // may already be executing.
    Result<OperationRecord> execute_claimed_operation(
        const product::Uuid& operation_id);

private:
    struct PendingExecution final {
        product::Uuid operation_id;
        product::Uuid request_id;
        ActionPlan plan;
        std::optional<LifecycleOperationIntent> lifecycle_intent;
        bool restart_recovered{};
        std::optional<RecoveryOperationIntent> recovery_intent;
        // Declared last so every existing aggregate initializer keeps meaning
        // what it meant: an entry built without naming this field is unclaimed,
        // which is the correct answer for every entry admission creates.  The
        // two entries `execute_operation` rebuilds for itself are the exception
        // and set it, because that call is already their executor.
        bool claimed{};
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

    // **The one enforcement point of "M5 is a single-disk product".**
    //
    // Refuses when any profile other than `excluded_profile` is up -- every runtime state
    // except `stopped` counts -- or when another lifecycle/start operation is still
    // pending.  A successful return is the *only* evidence a create or a start needs at
    // admission; there is deliberately no boolean variant, because a caller that could
    // ignore a bool would be the second place the rule is stated.
    //
    // `excluded_profile` exists for idempotent replays: a create replayed with the same
    // request id derives the same profile id and must reach the durable operation record
    // instead of being told a second disk exists.
    Result<void> refuse_if_another_disk_is_up(
        const product::Uuid& excluded_profile,
        const product::Uuid& request_id,
        const std::string& command,
        const wchar_t* step);

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
