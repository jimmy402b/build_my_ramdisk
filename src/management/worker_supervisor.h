#pragma once

#include "src/common/result.h"
#include "src/product/product_state.h"
#include "src/product/product_version.h"
#include "src/product/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace myrd::management {

class IServiceClock;

inline constexpr std::uint64_t kWorkerHeartbeatIntervalMs = 2'000;
inline constexpr std::uint64_t kWorkerDegradedAfterMs = 10'000;
inline constexpr std::uint64_t kWorkerNeedsAttentionAfterMs = 30'000;
inline constexpr std::uint32_t kScmFailureResetSeconds = 24U * 60U * 60U;
inline constexpr std::array<std::uint32_t, 3> kScmRestartDelaysSeconds{
    5U, 15U, 60U};
inline constexpr std::uint64_t kWorkerRestartWindowMs = 10U * 60U * 1'000U;
inline constexpr std::size_t kMaximumAutomaticWorkerRestarts = 3;

enum class WorkerSupervisorError : std::uint32_t {
    invalid_identity = 0x5A01,
    duplicate_attachment,
    process_identity_mismatch,
    authentication_failed,
    stale_runtime_snapshot,
    heartbeat_rejected,
    restart_history_invalid,
    recovery_proof_invalid,
};

struct WorkerRuntimeIdentity final {
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint32_t worker_pid{};
    std::uint64_t process_creation_time{};
    std::uint64_t session_generation{};
    std::uint64_t profile_revision{};
    std::uint32_t product_major{};
    std::uint32_t product_minor{};
    std::uint32_t product_patch{};

    bool operator==(const WorkerRuntimeIdentity&) const noexcept = default;
};

bool is_valid(const WorkerRuntimeIdentity& identity) noexcept;

struct WorkerDeviceIdentity final {
    std::uint32_t physical_drive_number{};
    std::uint64_t length_bytes{};
    std::uint32_t port_number{};
    std::uint8_t path_id{};
    std::uint8_t target_id{};
    std::uint8_t lun{};
    product::Uuid gpt_disk_id;
    product::Uuid partition_id;
    product::Uuid volume_id;

    bool operator==(const WorkerDeviceIdentity&) const noexcept = default;
};

struct WorkerRuntimeSnapshot final {
    WorkerRuntimeIdentity identity;
    WorkerDeviceIdentity device;
    std::uint64_t heartbeat_sequence{};
    std::uint64_t snapshot_sequence{};
    std::uint64_t dirty_epoch{};
    bool dirty{};
    bool checkpoint_policy_enabled{};
    std::uint64_t checkpoint_policy_revision{};
    std::uint64_t checkpoint_deadline_unix_ms{};
    std::optional<product::Uuid> active_operation_id;
    std::uint32_t active_operation_phase{};
    bool active_operation_noncancellable{};

    bool operator==(const WorkerRuntimeSnapshot&) const noexcept = default;
};

class IWorkerConnection {
public:
    virtual ~IWorkerConnection() noexcept = default;
};

enum class WorkerReattachmentDisposition : std::uint8_t {
    rotated_successor = 0,
    recovered_current,
};

struct WorkerConnectionOpen final {
    std::unique_ptr<IWorkerConnection> connection;
    WorkerRuntimeSnapshot snapshot;
    WorkerReattachmentDisposition disposition{
        WorkerReattachmentDisposition::rotated_successor};

    WorkerConnectionOpen(
        std::unique_ptr<IWorkerConnection> connection_value,
        WorkerRuntimeSnapshot snapshot_value,
        WorkerReattachmentDisposition disposition_value) noexcept;
    WorkerConnectionOpen(WorkerConnectionOpen&&) noexcept = default;
    WorkerConnectionOpen& operator=(WorkerConnectionOpen&&) noexcept = default;
    WorkerConnectionOpen(const WorkerConnectionOpen&) = delete;
    WorkerConnectionOpen& operator=(const WorkerConnectionOpen&) = delete;
};

class IWorkerProcessIdentityPin {
public:
    virtual ~IWorkerProcessIdentityPin() noexcept = default;
    virtual std::uint32_t process_id() const noexcept = 0;
    virtual std::uint64_t process_creation_time() const noexcept = 0;
    virtual bool is_running() const noexcept = 0;
    // Production pins can provide an exact bounded wait. Model pins that do
    // not own an OS wait capability retain conservative immediate behavior.
    virtual Result<bool> wait_for_exit(std::uint32_t) noexcept {
        return Result<bool>::success(!is_running());
    }
};

class IWorkerProcessIdentityQuery {
public:
    virtual ~IWorkerProcessIdentityQuery() noexcept = default;
    // Production returns an owning process-handle capability. The capability
    // remains alive for the complete authenticated attachment so PID reuse
    // cannot occur between a timestamp query and manager-slot acquisition.
    virtual Result<std::unique_ptr<IWorkerProcessIdentityPin>> pin_process(
        std::uint32_t process_id) = 0;
};

class IAuthenticatedWorkerConnector {
public:
    virtual ~IAuthenticatedWorkerConnector() noexcept = default;
    virtual Result<WorkerConnectionOpen> reattach_and_rotate(
        const WorkerRuntimeIdentity& expected,
        std::uint64_t next_profile_revision,
        const product::Uuid& service_instance_id) = 0;
};

class IReattachmentCheckpointScheduler {
public:
    virtual ~IReattachmentCheckpointScheduler() noexcept = default;
    enum class Disposition : std::uint8_t {
        queued = 0,
        already_pending,
        blocked,
    };

    struct Intent final {
        product::Uuid profile_id;
        product::Uuid image_id;
        std::uint64_t worker_session_generation{};
        std::uint64_t snapshot_sequence{};
        std::uint64_t dirty_epoch{};
        std::uint64_t checkpoint_policy_revision{};
        std::uint64_t checkpoint_deadline_unix_ms{};

        bool operator==(const Intent&) const noexcept = default;
        bool equivalent_checkpoint_truth(
            const Intent& other) const noexcept;
    };

    // This queues an idempotent policy intent; it never executes persistence
    // inline. Task 25 owns operation-gate arbitration and policy execution.
    virtual Disposition enqueue_immediate_checkpoint_intent(
        const Intent& intent) noexcept = 0;
};

struct WorkerSupervisionSnapshot final {
    product::HealthState health{product::HealthState::healthy};
    bool attached{};
    bool heartbeat_due{};
    bool reconnect_required{};
    bool automatic_kill_allowed{};
    bool checkpoint_intent_pending{};
    std::uint64_t last_valid_heartbeat_ms{};
    std::uint64_t next_heartbeat_due_ms{};
    std::optional<WorkerRuntimeSnapshot> runtime;

    bool operator==(const WorkerSupervisionSnapshot&) const noexcept = default;
};

class WorkerSupervisor final {
public:
    WorkerSupervisor(
        IServiceClock& clock,
        IWorkerProcessIdentityQuery& processes,
        IAuthenticatedWorkerConnector& connector,
        IReattachmentCheckpointScheduler& checkpoints) noexcept;
    ~WorkerSupervisor() noexcept;

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    Result<WorkerRuntimeSnapshot> reattach(
        const WorkerRuntimeIdentity& expected,
        std::uint64_t next_profile_revision,
        const product::Uuid& service_instance_id);
    Result<void> accept_heartbeat(const WorkerRuntimeSnapshot& heartbeat);
    std::optional<IReattachmentCheckpointScheduler::Disposition>
        retry_pending_checkpoint_intent() noexcept;
    WorkerSupervisionSnapshot observe() const noexcept;
    void disconnect() noexcept;

private:
    void update_checkpoint_intent_locked(
        const WorkerRuntimeSnapshot& snapshot,
        std::uint64_t now_unix_ms) noexcept;

    IServiceClock* clock_{};
    IWorkerProcessIdentityQuery* processes_{};
    IAuthenticatedWorkerConnector* connector_{};
    IReattachmentCheckpointScheduler* checkpoints_{};
    mutable std::mutex mutex_;
    std::unique_ptr<IWorkerProcessIdentityPin> process_pin_;
    std::unique_ptr<IWorkerConnection> connection_;
    std::optional<WorkerRuntimeSnapshot> runtime_;
    std::optional<IReattachmentCheckpointScheduler::Intent>
        pending_checkpoint_intent_;
    std::optional<IReattachmentCheckpointScheduler::Intent>
        accepted_checkpoint_intent_;
    std::uint64_t checkpoint_intent_epoch_{};
    std::uint64_t last_valid_heartbeat_ms_{};
};

enum class WorkerFailureKind : std::uint8_t {
    none = 0,
    process_exited,
    pid_reused,
    transport_lost,
    live_stalled,
    observation_indeterminate,
};

struct WorkerFailureObservation final {
    WorkerRuntimeIdentity expected;
    bool process_query_succeeded{};
    bool process_present{};
    bool process_running{};
    std::uint64_t observed_process_creation_time{};
    bool authenticated_transport_alive{};
    std::uint64_t elapsed_since_valid_heartbeat_ms{};
};

struct WorkerFailureClassification final {
    WorkerFailureKind kind{WorkerFailureKind::observation_indeterminate};
    product::HealthState health{product::HealthState::needs_attention};
    bool worker_exit_proven{};
    bool automatic_kill_allowed{};

    bool operator==(const WorkerFailureClassification&) const noexcept =
        default;
};

WorkerFailureClassification classify_worker_failure(
    const WorkerFailureObservation& observation) noexcept;

struct WorkerRestartAttempt final {
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t attempted_at_unix_ms{};
    std::uint64_t checkpoint_generation{};
    std::uint64_t observed_dirty_epoch{};
    bool volatile_data_loss_possible{};

    bool operator==(const WorkerRestartAttempt&) const noexcept = default;
};

struct WorkerRestartHistory final {
    product::Uuid profile_id;
    std::uint64_t revision{};
    std::vector<WorkerRestartAttempt> attempts;

    bool operator==(const WorkerRestartHistory&) const noexcept = default;
};

class IWorkerRestartHistoryStore {
public:
    virtual ~IWorkerRestartHistoryStore() noexcept = default;
    virtual Result<std::optional<WorkerRestartHistory>> load(
        const product::Uuid& profile_id) = 0;
    virtual Result<void> write_next(
        const product::Uuid& profile_id,
        std::optional<std::uint64_t> expected_revision,
        const WorkerRestartHistory& next) = 0;
};

struct WorkerRestartAdmissionRequest final {
    product::Uuid profile_id;
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t checkpoint_generation{};
    std::uint64_t observed_dirty_epoch{};
};

enum class WorkerRestartAdmissionDisposition : std::uint8_t {
    admitted = 0,
    replayed,
    limit_exhausted,
    clock_rollback,
};

struct WorkerRestartAdmission final {
    WorkerRestartAdmissionDisposition disposition{
        WorkerRestartAdmissionDisposition::limit_exhausted};
    std::optional<WorkerRestartAttempt> attempt;
    std::size_t active_attempts{};
};

struct WorkerRestartPreflightRequest final {
    product::Uuid profile_id;
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t observed_dirty_epoch{};
};

enum class WorkerRestartPreflightDisposition : std::uint8_t {
    available = 0,
    replayed,
    limit_exhausted,
    clock_rollback,
};

struct WorkerRestartPreflight final {
    WorkerRestartPreflightDisposition disposition{
        WorkerRestartPreflightDisposition::limit_exhausted};
    std::optional<WorkerRestartAttempt> attempt;
    std::size_t active_attempts{};
};

class WorkerRestartLimiter final {
public:
    WorkerRestartLimiter(
        IServiceClock& clock,
        IWorkerRestartHistoryStore& store) noexcept;

    Result<WorkerRestartPreflight> preflight(
        const WorkerRestartPreflightRequest& request);
    Result<WorkerRestartAdmission> admit(
        const WorkerRestartAdmissionRequest& request);

private:
    IServiceClock* clock_{};
    IWorkerRestartHistoryStore* store_{};
};

struct WorkerRestartRecoveryRequest final {
    WorkerRuntimeIdentity expected;
    WorkerDeviceIdentity expected_device;
    product::Uuid recovery_operation_id;
    std::uint64_t confirmed_checkpoint_generation{};
    std::uint64_t observed_dirty_epoch{};
};

struct WorkerCheckpointRecoveryProof final {
    product::Uuid profile_id;
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t profile_revision{};
    std::uint64_t failed_worker_session_generation{};
    std::uint64_t observed_dirty_epoch{};
    std::uint64_t latest_complete_generation{};
    bool interrupted_checkpoint_resolved{};
};

enum class WorkerResidualDeviceRecoveryDisposition : std::uint8_t {
    absent = 0,
    exact_device_removed,
    needs_attention,
};

struct WorkerResidualDeviceRecoveryProof final {
    product::Uuid profile_id;
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t profile_revision{};
    std::uint64_t failed_worker_session_generation{};
    WorkerDeviceIdentity device;
    WorkerResidualDeviceRecoveryDisposition disposition{
        WorkerResidualDeviceRecoveryDisposition::needs_attention};
};

struct WorkerRestartExecutionProof final {
    product::Uuid profile_id;
    product::Uuid image_id;
    product::Uuid recovery_operation_id;
    std::uint64_t checkpoint_generation{};
    WorkerRuntimeIdentity restarted_worker;
};

class IWorkerFailureRecoveryActions {
public:
    virtual ~IWorkerFailureRecoveryActions() noexcept = default;
    virtual Result<WorkerCheckpointRecoveryProof>
        reconcile_interrupted_checkpoint(
            const WorkerRestartRecoveryRequest& request) = 0;
    virtual Result<WorkerResidualDeviceRecoveryProof>
        reconcile_residual_device(
            const WorkerRestartRecoveryRequest& request) = 0;
    // This operation is idempotent by recovery_operation_id. It must reconcile
    // an already-started exact worker before launching and return authenticated
    // runtime identity proof; replay must never launch a second worker.
    virtual Result<WorkerRestartExecutionProof>
        ensure_restarted_from_checkpoint(
        const WorkerRestartRecoveryRequest& request,
        std::uint64_t generation) = 0;
};

enum class WorkerRestartRecoveryDisposition : std::uint8_t {
    waiting_for_live_worker = 0,
    needs_attention,
    restarted,
};

struct WorkerRestartRecoveryResult final {
    WorkerRestartRecoveryDisposition disposition{
        WorkerRestartRecoveryDisposition::needs_attention};
    WorkerFailureClassification failure;
    std::optional<std::uint64_t> restart_generation;
    bool volatile_data_loss_possible{};
};

class WorkerFailureRecoveryCoordinator final {
public:
    WorkerFailureRecoveryCoordinator(
        WorkerRestartLimiter& limiter,
        IWorkerFailureRecoveryActions& actions) noexcept;

    Result<WorkerRestartRecoveryResult> recover(
        const WorkerRestartRecoveryRequest& request,
        const WorkerFailureObservation& observation);

private:
    WorkerRestartLimiter* limiter_{};
    IWorkerFailureRecoveryActions* actions_{};
};

bool scm_failure_count_should_reset(
    std::uint64_t last_failure_unix_ms,
    std::uint64_t now_unix_ms) noexcept;

} // namespace myrd::management
