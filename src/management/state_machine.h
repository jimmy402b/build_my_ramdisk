#pragma once

#include "src/product/product_state.h"
#include "src/profile/profile_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace myrd::management {

enum class ManagementCommand : std::uint8_t {
    create_and_start = 0,
    create_stopped,
    import_profile,
    start,
    stop,
    force_stop,
    checkpoint,
    compact,
    config_set_live,
    config_set_stopped,
    repair_profile,
    rebind,
    forget,
    resolve_commit,
    status_read,
    config_read,
    diagnostics_read,
    operation_read,
    operation_cancel,
};

inline constexpr std::size_t kManagementCommandCount = 19;

enum class CommandAccess : std::uint8_t {
    shared_read = 0,
    exclusive_mutation,
    active_operation_control,
};

enum class PresenceRequirement : std::uint8_t { any = 0, absent, present };
enum class LifecycleRequirement : std::uint8_t { any = 0, ready };
enum class RuntimeRequirement : std::uint8_t {
    any = 0,
    stopped,
    running,
    stopped_or_running,
    stopped_running_or_orphaned,
};
enum class ActivityRequirement : std::uint8_t {
    any = 0,
    idle,
    idle_or_resolving_commit,
};
enum class PersistenceRequirement : std::uint8_t {
    any = 0,
    not_commit_indeterminate,
    compactable,
    commit_indeterminate,
};
enum class HealthRequirement : std::uint8_t {
    any = 0,
    operational,
    nonfatal,
    needs_attention,
};
enum class AdmissionEffect : std::uint8_t {
    preserve = 0,
    create_starting_provisioning,
    create_stopped_provisioning,
    import_stopped_ready,
    start_by_lifecycle,
    begin_stopping,
    checkpoint_if_needed,
    checkpoint_then_compact,
    begin_recovery,
    remove_profile,
    begin_commit_resolution,
};

struct TransitionContract final {
    ManagementCommand command;
    CommandAccess access;
    PresenceRequirement presence;
    LifecycleRequirement lifecycle;
    RuntimeRequirement runtime;
    ActivityRequirement activity;
    PersistenceRequirement persistence;
    HealthRequirement health;
    AdmissionEffect admission;

    bool operator==(const TransitionContract&) const noexcept = default;
};

std::span<const TransitionContract> transition_contracts() noexcept;

struct ProfileState final {
    // No lifecycle means the canonical profile is absent. A present profile
    // always has either the Provisioning or Ready lifecycle.
    std::optional<profile::ProfileLifecycle> lifecycle;
    product::DiskState disk;

    bool operator==(const ProfileState& other) const noexcept {
        return lifecycle == other.lifecycle
            && disk.runtime == other.disk.runtime
            && disk.activity == other.disk.activity
            && disk.persistence == other.disk.persistence
            && disk.health == other.disk.health;
    }
};

// Read-only service-facing projection.  ProfileState remains the sole
// durable authority; this enum is deliberately derived and contains no
// additional lifecycle state.
enum class AggregateStatus : std::uint8_t {
    stopped_configured = 0,
    running,
    blocked_needs_attention,
    failed,
};

AggregateStatus aggregate_status(const ProfileState& state) noexcept;
std::string_view to_string(AggregateStatus status) noexcept;

enum class TransitionDisposition : std::uint8_t {
    allowed = 0,
    already_running,
    already_stopped,
    no_changes,
    invalid_state,
    profile_not_found,
    profile_already_exists,
    state_conflict,
    persistence_blocked,
    commit_indeterminate,
    needs_attention,
    fatal,
};

enum class ActionKind : std::uint8_t {
    none = 0,
    create_and_verify_bundle,
    publish_provisioning_profile,
    inspect_bundle_read_only,
    publish_imported_ready_profile,
    create_runtime_and_worker,
    recover_bundle,
    start_proxy,
    create_exact_device,
    locate_exact_physical_drive,
    prove_new_uninitialized,
    provision_gpt_ntfs,
    record_volume_identity,
    flush_exact_volume,
    checkpoint,
    checkpoint_if_required,
    publish_ready_profile,
    verify_existing_volume_identity,
    assign_exact_mount,
    readiness_io,
    remove_mount,
    remove_exact_device,
    stop_runtime_and_release_lock,
    compact,
    publish_live_config,
    publish_stopped_config,
    repair_profile_from_previous,
    verify_rebind_bundle,
    publish_rebind,
    forget_metadata,
    resolve_commit,
    read_status,
    read_config,
    collect_diagnostics,
    read_operation,
    request_operation_cancel,
    // Appended to preserve the stable numeric values already used by the
    // private worker protocol and durable model fixtures.
    quiesce_mutations,
    resume_mutations,
};

inline constexpr std::size_t kMaximumPlanActions = 16;

struct ActionPlan final {
    ManagementCommand command;
    CommandAccess access;
    TransitionDisposition disposition;
    ProfileState before;
    // Projected state at the state-owning admission point. Planning is pure:
    // the caller must durably publish/adopt this state before executing the
    // fixed action sequence, and rejected/no-op plans preserve `before`.
    ProfileState admitted;
    AdmissionEffect admission;
    std::array<ActionKind, kMaximumPlanActions> actions{};
    std::size_t action_count{};

    bool accepted() const noexcept {
        return disposition == TransitionDisposition::allowed
            || disposition == TransitionDisposition::already_running
            || disposition == TransitionDisposition::already_stopped
            || disposition == TransitionDisposition::no_changes;
    }
    std::span<const ActionKind> action_span() const noexcept {
        if (action_count > actions.size()) return {};
        return std::span<const ActionKind>(actions.data(), action_count);
    }
};

ProfileState absent_profile_state() noexcept;
ProfileState registered_profile_state(
    profile::ProfileLifecycle lifecycle,
    product::DiskState disk) noexcept;
bool is_valid(const ProfileState& state) noexcept;
bool is_valid(const ActionPlan& plan) noexcept;
// Produces a bounded, allocation-free plan without I/O or state mutation.
ActionPlan plan_command(
    const ProfileState& state, ManagementCommand command) noexcept;

// Execution evidence consumed by the command-specific completion reducer.
// Clean/dirty outcomes are persistence evidence, not generic success labels;
// complete_command rejects outcomes that are invalid for the planned command.
enum class CompletionOutcome : std::uint8_t {
    succeeded_clean = 0,
    succeeded_dirty,
    transient_failure,
    needs_attention_failure,
    persistence_blocked,
    committed_needs_reconciliation_clean,
    committed_needs_reconciliation_dirty,
    commit_indeterminate,
    compaction_deferred,
    resolution_unresolved,
};

enum class CompletionDisposition : std::uint8_t {
    applied = 0,
    invalid_plan,
    invalid_outcome,
};

struct CompletionDecision final {
    CompletionDisposition disposition;
    ProfileState after;

    bool applied() const noexcept {
        return disposition == CompletionDisposition::applied;
    }
};

CompletionDecision complete_command(
    const ActionPlan& plan, CompletionOutcome outcome) noexcept;

struct OperationGateState final {
    std::optional<product::Uuid> active_operation_id;
    ManagementCommand active_command{ManagementCommand::status_read};
    bool cancellable{};

    bool operator==(const OperationGateState&) const noexcept = default;
};

enum class GateDisposition : std::uint8_t {
    allowed = 0,
    invalid_request,
    operation_conflict,
    no_active_operation,
    target_mismatch,
    cancellation_too_late,
};

struct GateDecision final {
    GateDisposition disposition;
    OperationGateState before;
    OperationGateState after;

    bool accepted() const noexcept {
        return disposition == GateDisposition::allowed;
    }
};

OperationGateState idle_operation_gate() noexcept;
// These gate functions are pure reducers. The caller owns synchronization and
// durably records operation/cancellation state around the returned decision.
GateDecision plan_gate_access(
    const OperationGateState& gate,
    const ActionPlan& plan,
    const product::Uuid& operation_id) noexcept;
GateDecision mark_operation_noncancellable(
    const OperationGateState& gate,
    const product::Uuid& operation_id) noexcept;
GateDecision release_operation_gate(
    const OperationGateState& gate,
    const product::Uuid& operation_id) noexcept;

} // namespace myrd::management
