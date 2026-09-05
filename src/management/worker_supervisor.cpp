#include "src/management/worker_supervisor.h"

#include "src/management/service_core.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace myrd::management {
namespace {

bool all_zero(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](const std::byte byte) { return byte == std::byte{}; });
}

template <typename T>
Result<T> supervisor_failure(
    const WorkerSupervisorError code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code),
        operation,
        message});
}

std::uint64_t saturating_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    return left > maximum - right ? maximum : left + right;
}

bool exact_successor(
    const WorkerRuntimeIdentity& expected,
    const WorkerRuntimeIdentity& actual,
    const std::uint64_t next_profile_revision) noexcept {
    return actual.profile_id == expected.profile_id
        && actual.image_id == expected.image_id
        && actual.worker_pid == expected.worker_pid
        && actual.process_creation_time == expected.process_creation_time
        && expected.session_generation
            != (std::numeric_limits<std::uint64_t>::max)()
        && actual.session_generation == expected.session_generation + 1
        && next_profile_revision >= expected.profile_revision
        && actual.profile_revision == next_profile_revision
        && actual.product_major == expected.product_major
        && actual.product_minor == expected.product_minor
        && actual.product_patch == expected.product_patch;
}

bool exact_current(
    const WorkerRuntimeIdentity& expected,
    const WorkerRuntimeIdentity& actual) noexcept {
    return actual == expected;
}

bool valid_runtime_snapshot(
    const WorkerRuntimeSnapshot& snapshot) noexcept {
    const bool has_operation = snapshot.active_operation_id.has_value();
    return is_valid(snapshot.identity)
        && snapshot.device.length_bytes != 0
        && !all_zero(snapshot.device.gpt_disk_id)
        && !all_zero(snapshot.device.partition_id)
        && !all_zero(snapshot.device.volume_id)
        && snapshot.heartbeat_sequence != 0
        && snapshot.snapshot_sequence != 0
        && snapshot.checkpoint_policy_revision != 0
        && (!snapshot.checkpoint_policy_enabled
            || snapshot.checkpoint_deadline_unix_ms != 0)
        && (!has_operation
            || (!all_zero(snapshot.active_operation_id.value())
                && snapshot.active_operation_phase != 0))
        && (has_operation
            || (snapshot.active_operation_phase == 0
                && !snapshot.active_operation_noncancellable));
}

bool valid_worker_device(const WorkerDeviceIdentity& device) noexcept {
    return device.length_bytes != 0
        && !all_zero(device.gpt_disk_id)
        && !all_zero(device.partition_id)
        && !all_zero(device.volume_id);
}

bool valid_restart_attempt(const WorkerRestartAttempt& attempt) noexcept {
    return !all_zero(attempt.image_id)
        && !all_zero(attempt.recovery_operation_id)
        && attempt.attempted_at_unix_ms != 0
        && attempt.checkpoint_generation != 0
        && attempt.volatile_data_loss_possible;
}

bool valid_restart_history(
    const WorkerRestartHistory& history,
    const product::Uuid& profile_id) noexcept {
    if (all_zero(profile_id) || history.profile_id != profile_id
        || history.revision == 0
        || history.attempts.size() > kMaximumAutomaticWorkerRestarts) {
        return false;
    }
    std::uint64_t previous_time{};
    std::vector<product::Uuid> operations;
    operations.reserve(history.attempts.size());
    for (const auto& attempt : history.attempts) {
        if (!valid_restart_attempt(attempt)
            || attempt.attempted_at_unix_ms < previous_time
            || std::find(
                   operations.begin(), operations.end(),
                   attempt.recovery_operation_id)
                != operations.end()) {
            return false;
        }
        previous_time = attempt.attempted_at_unix_ms;
        operations.push_back(attempt.recovery_operation_id);
    }
    return true;
}

bool valid_admission_request(
    const WorkerRestartAdmissionRequest& request) noexcept {
    return !all_zero(request.profile_id)
        && !all_zero(request.image_id)
        && !all_zero(request.recovery_operation_id)
        && request.checkpoint_generation != 0;
}

bool replay_matches(
    const WorkerRestartAttempt& attempt,
    const WorkerRestartAdmissionRequest& request) noexcept {
    return attempt.image_id == request.image_id
        && attempt.checkpoint_generation == request.checkpoint_generation
        && attempt.observed_dirty_epoch == request.observed_dirty_epoch;
}

bool valid_restart_execution_proof(
    const WorkerRestartRecoveryRequest& request,
    const std::uint64_t generation,
    const WorkerRestartExecutionProof& proof) noexcept {
    const auto& restarted = proof.restarted_worker;
    return proof.profile_id == request.expected.profile_id
        && proof.image_id == request.expected.image_id
        && proof.recovery_operation_id == request.recovery_operation_id
        && proof.checkpoint_generation == generation
        && is_valid(restarted)
        && restarted.profile_id == request.expected.profile_id
        && restarted.image_id == request.expected.image_id
        && restarted.profile_revision == request.expected.profile_revision
        && restarted.process_creation_time
            != request.expected.process_creation_time
        && request.expected.session_generation
            != (std::numeric_limits<std::uint64_t>::max)()
        && restarted.session_generation
            == request.expected.session_generation + 1;
}

} // namespace

bool is_valid(const WorkerRuntimeIdentity& identity) noexcept {
    return !all_zero(identity.profile_id)
        && !all_zero(identity.image_id)
        && identity.worker_pid != 0
        && identity.process_creation_time != 0
        && identity.session_generation != 0
        && identity.profile_revision != 0
        && identity.product_major == product::kProductVersionMajor
        && identity.product_minor == product::kProductVersionMinor
        && identity.product_patch == product::kProductVersionPatch;
}

WorkerConnectionOpen::WorkerConnectionOpen(
    std::unique_ptr<IWorkerConnection> connection_value,
    WorkerRuntimeSnapshot snapshot_value,
    const WorkerReattachmentDisposition disposition_value) noexcept
    : connection(std::move(connection_value)),
      snapshot(std::move(snapshot_value)),
      disposition(disposition_value) {}

bool IReattachmentCheckpointScheduler::Intent::equivalent_checkpoint_truth(
    const Intent& other) const noexcept {
    return profile_id == other.profile_id
        && image_id == other.image_id
        && dirty_epoch == other.dirty_epoch
        && checkpoint_policy_revision == other.checkpoint_policy_revision
        && checkpoint_deadline_unix_ms
            == other.checkpoint_deadline_unix_ms;
}

WorkerSupervisor::WorkerSupervisor(
    IServiceClock& clock,
    IWorkerProcessIdentityQuery& processes,
    IAuthenticatedWorkerConnector& connector,
    IReattachmentCheckpointScheduler& checkpoints) noexcept
    : clock_(&clock),
      processes_(&processes),
      connector_(&connector),
      checkpoints_(&checkpoints) {}

WorkerSupervisor::~WorkerSupervisor() noexcept { disconnect(); }

void WorkerSupervisor::update_checkpoint_intent_locked(
    const WorkerRuntimeSnapshot& snapshot,
    const std::uint64_t now_unix_ms) noexcept {
    std::optional<IReattachmentCheckpointScheduler::Intent> desired;
    if (snapshot.dirty && snapshot.checkpoint_policy_enabled
        && now_unix_ms >= snapshot.checkpoint_deadline_unix_ms) {
        desired = IReattachmentCheckpointScheduler::Intent{
            snapshot.identity.profile_id,
            snapshot.identity.image_id,
            snapshot.identity.session_generation,
            snapshot.snapshot_sequence,
            snapshot.dirty_epoch,
            snapshot.checkpoint_policy_revision,
            snapshot.checkpoint_deadline_unix_ms};
    }

    if (!desired.has_value()) {
        if (pending_checkpoint_intent_.has_value()) {
            pending_checkpoint_intent_.reset();
            ++checkpoint_intent_epoch_;
        }
        accepted_checkpoint_intent_.reset();
        return;
    }
    if (accepted_checkpoint_intent_.has_value()
        && accepted_checkpoint_intent_->equivalent_checkpoint_truth(
            desired.value())) {
        if (pending_checkpoint_intent_.has_value()) {
            pending_checkpoint_intent_.reset();
            ++checkpoint_intent_epoch_;
        }
        return;
    }
    if (pending_checkpoint_intent_ != desired) {
        pending_checkpoint_intent_ = std::move(desired);
        ++checkpoint_intent_epoch_;
    }
}

Result<WorkerRuntimeSnapshot> WorkerSupervisor::reattach(
    const WorkerRuntimeIdentity& expected,
    const std::uint64_t next_profile_revision,
    const product::Uuid& service_instance_id) {
    std::optional<WorkerRuntimeSnapshot> accepted;
    {
        std::lock_guard lock(mutex_);
        if (connection_ != nullptr) {
            return supervisor_failure<WorkerRuntimeSnapshot>(
                WorkerSupervisorError::duplicate_attachment,
                L"worker_supervisor.reattach",
                L"This service supervisor already owns a worker connection");
        }
        if (!is_valid(expected) || all_zero(service_instance_id)
            || next_profile_revision < expected.profile_revision) {
            return supervisor_failure<WorkerRuntimeSnapshot>(
                WorkerSupervisorError::invalid_identity,
                L"worker_supervisor.reattach",
                L"Worker or service reattachment identity is invalid");
        }
        auto pinned = processes_->pin_process(expected.worker_pid);
        if (!pinned.ok()) {
            return Result<WorkerRuntimeSnapshot>::failure(pinned.error());
        }
        if (pinned.value() == nullptr
            || pinned.value()->process_id() != expected.worker_pid
            || pinned.value()->process_creation_time()
                != expected.process_creation_time
            || !pinned.value()->is_running()) {
            return supervisor_failure<WorkerRuntimeSnapshot>(
                WorkerSupervisorError::process_identity_mismatch,
                L"worker_supervisor.process_before",
                L"Worker PID was reused before authenticated reattachment");
        }
        auto opened = connector_->reattach_and_rotate(
            expected, next_profile_revision, service_instance_id);
        if (!opened.ok()) {
            return Result<WorkerRuntimeSnapshot>::failure(opened.error());
        }
        if (opened.value().connection == nullptr
            || !valid_runtime_snapshot(opened.value().snapshot)
            || (opened.value().disposition
                    != WorkerReattachmentDisposition::rotated_successor
                && opened.value().disposition
                    != WorkerReattachmentDisposition::recovered_current)
            || (opened.value().disposition
                    == WorkerReattachmentDisposition::rotated_successor
                && !exact_successor(
                    expected,
                    opened.value().snapshot.identity,
                    next_profile_revision))
            || (opened.value().disposition
                    == WorkerReattachmentDisposition::recovered_current
                && !exact_current(
                    expected,
                    opened.value().snapshot.identity))) {
            return supervisor_failure<WorkerRuntimeSnapshot>(
                WorkerSupervisorError::stale_runtime_snapshot,
                L"worker_supervisor.snapshot",
                L"Authenticated worker returned a stale or mismatched runtime snapshot");
        }
        if (!pinned.value()->is_running()
            || pinned.value()->process_id() != expected.worker_pid
            || pinned.value()->process_creation_time()
                != expected.process_creation_time) {
            return supervisor_failure<WorkerRuntimeSnapshot>(
                WorkerSupervisorError::process_identity_mismatch,
                L"worker_supervisor.process_after",
                L"Worker PID identity changed during authenticated reattachment");
        }

        process_pin_ = std::move(pinned.value());
        connection_ = std::move(opened.value().connection);
        runtime_ = opened.value().snapshot;
        last_valid_heartbeat_ms_ = clock_->monotonic_time_ms();
        accepted = *runtime_;
        update_checkpoint_intent_locked(
            accepted.value(), clock_->unix_time_ms());
    }
    static_cast<void>(retry_pending_checkpoint_intent());
    return Result<WorkerRuntimeSnapshot>::success(
        std::move(accepted.value()));
}

Result<void> WorkerSupervisor::accept_heartbeat(
    const WorkerRuntimeSnapshot& heartbeat) {
    {
        std::lock_guard lock(mutex_);
        if (connection_ == nullptr || !runtime_.has_value()) {
            return supervisor_failure<void>(
                WorkerSupervisorError::heartbeat_rejected,
                L"worker_supervisor.heartbeat",
                L"Heartbeat arrived without an authenticated connection");
        }
        if (!valid_runtime_snapshot(heartbeat)
            || heartbeat.identity != runtime_->identity
            || heartbeat.device != runtime_->device
            || heartbeat.heartbeat_sequence <= runtime_->heartbeat_sequence
            || heartbeat.snapshot_sequence <= runtime_->snapshot_sequence
            || heartbeat.dirty_epoch < runtime_->dirty_epoch) {
            return supervisor_failure<void>(
                WorkerSupervisorError::heartbeat_rejected,
                L"worker_supervisor.heartbeat",
                L"Heartbeat identity, sequence, device or dirty epoch is stale");
        }
        runtime_ = heartbeat;
        last_valid_heartbeat_ms_ = clock_->monotonic_time_ms();
        update_checkpoint_intent_locked(
            heartbeat, clock_->unix_time_ms());
    }
    static_cast<void>(retry_pending_checkpoint_intent());
    return Result<void>::success();
}

std::optional<IReattachmentCheckpointScheduler::Disposition>
WorkerSupervisor::retry_pending_checkpoint_intent() noexcept {
    std::optional<IReattachmentCheckpointScheduler::Intent> intent;
    std::uint64_t intent_epoch{};
    {
        std::lock_guard lock(mutex_);
        if (!pending_checkpoint_intent_.has_value()) return std::nullopt;
        intent = pending_checkpoint_intent_;
        intent_epoch = checkpoint_intent_epoch_;
    }

    const auto disposition =
        checkpoints_->enqueue_immediate_checkpoint_intent(intent.value());
    if (disposition
        != IReattachmentCheckpointScheduler::Disposition::blocked) {
        std::lock_guard lock(mutex_);
        if (checkpoint_intent_epoch_ == intent_epoch
            && pending_checkpoint_intent_ == intent) {
            accepted_checkpoint_intent_ = intent;
            pending_checkpoint_intent_.reset();
            ++checkpoint_intent_epoch_;
        }
    }
    return disposition;
}

WorkerSupervisionSnapshot WorkerSupervisor::observe() const noexcept {
    std::lock_guard lock(mutex_);
    const auto now = clock_->monotonic_time_ms();
    WorkerSupervisionSnapshot result{};
    result.attached = connection_ != nullptr && runtime_.has_value();
    result.automatic_kill_allowed = false;
    result.checkpoint_intent_pending =
        pending_checkpoint_intent_.has_value();
    if (!result.attached) return result;

    result.runtime = runtime_;
    result.last_valid_heartbeat_ms = last_valid_heartbeat_ms_;
    result.next_heartbeat_due_ms = saturating_add(
        last_valid_heartbeat_ms_, kWorkerHeartbeatIntervalMs);
    if (now < last_valid_heartbeat_ms_) {
        result.health = product::HealthState::needs_attention;
        result.heartbeat_due = true;
        result.reconnect_required = true;
        return result;
    }
    const auto elapsed = now - last_valid_heartbeat_ms_;
    result.heartbeat_due = elapsed >= kWorkerHeartbeatIntervalMs;
    if (elapsed >= kWorkerNeedsAttentionAfterMs) {
        result.health = product::HealthState::needs_attention;
        result.reconnect_required = true;
    } else if (elapsed >= kWorkerDegradedAfterMs) {
        result.health = product::HealthState::degraded;
        result.reconnect_required = true;
    } else {
        result.health = product::HealthState::healthy;
    }
    if (result.checkpoint_intent_pending
        && result.health == product::HealthState::healthy) {
        result.health = product::HealthState::degraded;
    }
    return result;
}

void WorkerSupervisor::disconnect() noexcept {
    std::unique_ptr<IWorkerConnection> connection;
    std::unique_ptr<IWorkerProcessIdentityPin> process_pin;
    {
        std::lock_guard lock(mutex_);
        connection = std::move(connection_);
        process_pin = std::move(process_pin_);
        runtime_.reset();
        pending_checkpoint_intent_.reset();
        accepted_checkpoint_intent_.reset();
        ++checkpoint_intent_epoch_;
        last_valid_heartbeat_ms_ = 0;
    }
    connection.reset();
    process_pin.reset();
}

WorkerFailureClassification classify_worker_failure(
    const WorkerFailureObservation& observation) noexcept {
    WorkerFailureClassification result{};
    result.automatic_kill_allowed = false;
    if (!is_valid(observation.expected)
        || !observation.process_query_succeeded) {
        result.kind = WorkerFailureKind::observation_indeterminate;
        result.health = product::HealthState::needs_attention;
        return result;
    }
    const bool contradictory_absence = !observation.process_present
        && (observation.process_running
            || observation.observed_process_creation_time != 0
            || observation.authenticated_transport_alive);
    const bool contradictory_exit = observation.process_present
        && !observation.process_running
        && observation.authenticated_transport_alive;
    const bool contradictory_live_identity = observation.process_present
        && observation.process_running
        && (observation.observed_process_creation_time == 0
            || (observation.observed_process_creation_time
                    != observation.expected.process_creation_time
                && observation.authenticated_transport_alive));
    if (contradictory_absence || contradictory_exit
        || contradictory_live_identity) {
        result.kind = WorkerFailureKind::observation_indeterminate;
        result.health = product::HealthState::needs_attention;
        return result;
    }
    if (!observation.process_present || !observation.process_running) {
        result.kind = WorkerFailureKind::process_exited;
        result.health = product::HealthState::needs_attention;
        result.worker_exit_proven = true;
        return result;
    }
    if (observation.observed_process_creation_time
        != observation.expected.process_creation_time) {
        result.kind = WorkerFailureKind::pid_reused;
        result.health = product::HealthState::needs_attention;
        result.worker_exit_proven = true;
        return result;
    }
    if (observation.elapsed_since_valid_heartbeat_ms
        >= kWorkerNeedsAttentionAfterMs) {
        result.kind = WorkerFailureKind::live_stalled;
        result.health = product::HealthState::needs_attention;
        return result;
    }
    if (!observation.authenticated_transport_alive) {
        result.kind = WorkerFailureKind::transport_lost;
        result.health = product::HealthState::degraded;
        return result;
    }
    result.kind = WorkerFailureKind::none;
    result.health = observation.elapsed_since_valid_heartbeat_ms
            >= kWorkerDegradedAfterMs
        ? product::HealthState::degraded
        : product::HealthState::healthy;
    return result;
}

WorkerRestartLimiter::WorkerRestartLimiter(
    IServiceClock& clock,
    IWorkerRestartHistoryStore& store) noexcept
    : clock_(&clock), store_(&store) {}

Result<WorkerRestartPreflight> WorkerRestartLimiter::preflight(
    const WorkerRestartPreflightRequest& request) {
    if (all_zero(request.profile_id) || all_zero(request.image_id)
        || all_zero(request.recovery_operation_id)) {
        return supervisor_failure<WorkerRestartPreflight>(
            WorkerSupervisorError::restart_history_invalid,
            L"worker_restart.preflight",
            L"Worker restart preflight request is invalid");
    }
    auto loaded = store_->load(request.profile_id);
    if (!loaded.ok()) {
        return Result<WorkerRestartPreflight>::failure(loaded.error());
    }
    if (!loaded.value().has_value()) {
        return Result<WorkerRestartPreflight>::success({
            WorkerRestartPreflightDisposition::available,
            std::nullopt,
            0});
    }
    const auto& history = loaded.value().value();
    if (!valid_restart_history(history, request.profile_id)) {
        return supervisor_failure<WorkerRestartPreflight>(
            WorkerSupervisorError::restart_history_invalid,
            L"worker_restart.preflight_history",
            L"Persisted worker restart history is invalid");
    }
    const auto replay = std::find_if(
        history.attempts.begin(), history.attempts.end(),
        [&](const WorkerRestartAttempt& attempt) {
            return attempt.recovery_operation_id
                == request.recovery_operation_id;
        });
    if (replay != history.attempts.end()) {
        if (replay->image_id != request.image_id
            || replay->observed_dirty_epoch != request.observed_dirty_epoch) {
            return supervisor_failure<WorkerRestartPreflight>(
                WorkerSupervisorError::restart_history_invalid,
                L"worker_restart.preflight_replay",
                L"Restart preflight replay disagrees with durable admission");
        }
        return Result<WorkerRestartPreflight>::success({
            WorkerRestartPreflightDisposition::replayed,
            *replay,
            history.attempts.size()});
    }
    const auto now = clock_->unix_time_ms();
    if (std::any_of(
            history.attempts.begin(), history.attempts.end(),
            [now](const WorkerRestartAttempt& attempt) {
                return attempt.attempted_at_unix_ms > now;
            })) {
        return Result<WorkerRestartPreflight>::success({
            WorkerRestartPreflightDisposition::clock_rollback,
            std::nullopt,
            history.attempts.size()});
    }
    const auto active = static_cast<std::size_t>(std::count_if(
        history.attempts.begin(), history.attempts.end(),
        [now](const WorkerRestartAttempt& attempt) {
            return now - attempt.attempted_at_unix_ms
                < kWorkerRestartWindowMs;
        }));
    return Result<WorkerRestartPreflight>::success({
        active >= kMaximumAutomaticWorkerRestarts
            ? WorkerRestartPreflightDisposition::limit_exhausted
            : WorkerRestartPreflightDisposition::available,
        std::nullopt,
        active});
}

Result<WorkerRestartAdmission> WorkerRestartLimiter::admit(
    const WorkerRestartAdmissionRequest& request) {
    if (!valid_admission_request(request)) {
        return supervisor_failure<WorkerRestartAdmission>(
            WorkerSupervisorError::restart_history_invalid,
            L"worker_restart.admit",
            L"Worker restart admission request is invalid");
    }
    auto loaded = store_->load(request.profile_id);
    if (!loaded.ok()) {
        return Result<WorkerRestartAdmission>::failure(loaded.error());
    }
    WorkerRestartHistory history{request.profile_id, 0, {}};
    std::optional<std::uint64_t> expected_revision;
    if (loaded.value().has_value()) {
        history = loaded.value().value();
        if (!valid_restart_history(history, request.profile_id)) {
            return supervisor_failure<WorkerRestartAdmission>(
                WorkerSupervisorError::restart_history_invalid,
                L"worker_restart.history",
                L"Persisted worker restart history is invalid");
        }
        expected_revision = history.revision;
    } else {
        history.profile_id = request.profile_id;
    }

    const auto now = clock_->unix_time_ms();
    const auto replay = std::find_if(
        history.attempts.begin(), history.attempts.end(),
        [&](const WorkerRestartAttempt& attempt) {
            return attempt.recovery_operation_id
                == request.recovery_operation_id;
        });
    if (replay != history.attempts.end()) {
        if (!replay_matches(*replay, request)) {
            return supervisor_failure<WorkerRestartAdmission>(
                WorkerSupervisorError::restart_history_invalid,
                L"worker_restart.replay",
                L"Restart operation replay disagrees with durable admission");
        }
        return Result<WorkerRestartAdmission>::success({
            WorkerRestartAdmissionDisposition::replayed,
            *replay,
            history.attempts.size()});
    }
    if (std::any_of(
            history.attempts.begin(), history.attempts.end(),
            [now](const WorkerRestartAttempt& attempt) {
                return attempt.attempted_at_unix_ms > now;
            })) {
        return Result<WorkerRestartAdmission>::success({
            WorkerRestartAdmissionDisposition::clock_rollback,
            std::nullopt,
            history.attempts.size()});
    }

    history.attempts.erase(
        std::remove_if(
            history.attempts.begin(), history.attempts.end(),
            [now](const WorkerRestartAttempt& attempt) {
                return now - attempt.attempted_at_unix_ms
                    >= kWorkerRestartWindowMs;
            }),
        history.attempts.end());
    if (history.attempts.size() >= kMaximumAutomaticWorkerRestarts) {
        return Result<WorkerRestartAdmission>::success({
            WorkerRestartAdmissionDisposition::limit_exhausted,
            std::nullopt,
            history.attempts.size()});
    }
    if (history.revision
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return supervisor_failure<WorkerRestartAdmission>(
            WorkerSupervisorError::restart_history_invalid,
            L"worker_restart.revision",
            L"Worker restart history revision overflowed");
    }
    WorkerRestartAttempt attempt{
        request.image_id,
        request.recovery_operation_id,
        now,
        request.checkpoint_generation,
        request.observed_dirty_epoch,
        true};
    history.attempts.push_back(attempt);
    ++history.revision;
    auto persisted = store_->write_next(
        request.profile_id, expected_revision, history);
    if (!persisted.ok()) {
        return Result<WorkerRestartAdmission>::failure(persisted.error());
    }
    return Result<WorkerRestartAdmission>::success({
        WorkerRestartAdmissionDisposition::admitted,
        std::move(attempt),
        history.attempts.size()});
}

WorkerFailureRecoveryCoordinator::WorkerFailureRecoveryCoordinator(
    WorkerRestartLimiter& limiter,
    IWorkerFailureRecoveryActions& actions) noexcept
    : limiter_(&limiter), actions_(&actions) {}

Result<WorkerRestartRecoveryResult>
WorkerFailureRecoveryCoordinator::recover(
    const WorkerRestartRecoveryRequest& request,
    const WorkerFailureObservation& observation) {
    const auto failure = classify_worker_failure(observation);
    if (!is_valid(request.expected)
        || !valid_worker_device(request.expected_device)
        || all_zero(request.recovery_operation_id)
        || request.confirmed_checkpoint_generation == 0
        || observation.expected != request.expected) {
        return supervisor_failure<WorkerRestartRecoveryResult>(
            WorkerSupervisorError::recovery_proof_invalid,
            L"worker_recovery.request",
            L"Worker recovery request and failure observation disagree");
    }
    if (!failure.worker_exit_proven) {
        return Result<WorkerRestartRecoveryResult>::success({
            failure.health == product::HealthState::needs_attention
                ? WorkerRestartRecoveryDisposition::needs_attention
                : WorkerRestartRecoveryDisposition::waiting_for_live_worker,
            failure,
            std::nullopt,
            false});
    }

    auto preflight = limiter_->preflight({
        request.expected.profile_id,
        request.expected.image_id,
        request.recovery_operation_id,
        request.observed_dirty_epoch});
    if (!preflight.ok()) {
        return Result<WorkerRestartRecoveryResult>::failure(
            preflight.error());
    }
    if (preflight.value().disposition
            == WorkerRestartPreflightDisposition::limit_exhausted
        || preflight.value().disposition
            == WorkerRestartPreflightDisposition::clock_rollback) {
        return Result<WorkerRestartRecoveryResult>::success({
            WorkerRestartRecoveryDisposition::needs_attention,
            failure,
            std::nullopt,
            true});
    }
    if (preflight.value().disposition
        == WorkerRestartPreflightDisposition::replayed) {
        if (!preflight.value().attempt.has_value()
            || preflight.value().attempt->checkpoint_generation
                < request.confirmed_checkpoint_generation) {
            return supervisor_failure<WorkerRestartRecoveryResult>(
                WorkerSupervisorError::restart_history_invalid,
                L"worker_recovery.preflight_replay",
                L"Durable restart replay is missing or stale");
        }
        const auto generation =
            preflight.value().attempt->checkpoint_generation;
        auto ensured = actions_->ensure_restarted_from_checkpoint(
            request, generation);
        if (!ensured.ok()) {
            return Result<WorkerRestartRecoveryResult>::failure(
                ensured.error());
        }
        if (!valid_restart_execution_proof(
                request, generation, ensured.value())) {
            return supervisor_failure<WorkerRestartRecoveryResult>(
                WorkerSupervisorError::recovery_proof_invalid,
                L"worker_recovery.replay_restart_proof",
                L"Replay restart action returned stale or mismatched proof");
        }
        return Result<WorkerRestartRecoveryResult>::success({
            WorkerRestartRecoveryDisposition::restarted,
            failure,
            generation,
            true});
    }

    auto checkpoint = actions_->reconcile_interrupted_checkpoint(request);
    if (!checkpoint.ok()) {
        return Result<WorkerRestartRecoveryResult>::failure(
            checkpoint.error());
    }
    if (checkpoint.value().profile_id != request.expected.profile_id
        || checkpoint.value().image_id != request.expected.image_id
        || checkpoint.value().recovery_operation_id
            != request.recovery_operation_id
        || checkpoint.value().profile_revision
            != request.expected.profile_revision
        || checkpoint.value().failed_worker_session_generation
            != request.expected.session_generation
        || checkpoint.value().observed_dirty_epoch
            != request.observed_dirty_epoch
        || checkpoint.value().latest_complete_generation
            < request.confirmed_checkpoint_generation) {
        return supervisor_failure<WorkerRestartRecoveryResult>(
            WorkerSupervisorError::recovery_proof_invalid,
            L"worker_recovery.checkpoint",
            L"Checkpoint recovery proof is stale or identity-mismatched");
    }
    if (!checkpoint.value().interrupted_checkpoint_resolved) {
        return Result<WorkerRestartRecoveryResult>::success({
            WorkerRestartRecoveryDisposition::needs_attention,
            failure,
            std::nullopt,
            true});
    }

    auto residual = actions_->reconcile_residual_device(request);
    if (!residual.ok()) {
        return Result<WorkerRestartRecoveryResult>::failure(residual.error());
    }
    if (residual.value().profile_id != request.expected.profile_id
        || residual.value().image_id != request.expected.image_id
        || residual.value().recovery_operation_id
            != request.recovery_operation_id
        || residual.value().profile_revision
            != request.expected.profile_revision
        || residual.value().failed_worker_session_generation
            != request.expected.session_generation
        || residual.value().device != request.expected_device) {
        return supervisor_failure<WorkerRestartRecoveryResult>(
            WorkerSupervisorError::recovery_proof_invalid,
            L"worker_recovery.residual",
            L"Residual-device recovery proof is identity-mismatched");
    }
    if (residual.value().disposition
            != WorkerResidualDeviceRecoveryDisposition::absent
        && residual.value().disposition
            != WorkerResidualDeviceRecoveryDisposition::exact_device_removed
        && residual.value().disposition
            != WorkerResidualDeviceRecoveryDisposition::needs_attention) {
        return supervisor_failure<WorkerRestartRecoveryResult>(
            WorkerSupervisorError::recovery_proof_invalid,
            L"worker_recovery.residual_disposition",
            L"Residual-device recovery proof has an unknown disposition");
    }
    if (residual.value().disposition
        == WorkerResidualDeviceRecoveryDisposition::needs_attention) {
        return Result<WorkerRestartRecoveryResult>::success({
            WorkerRestartRecoveryDisposition::needs_attention,
            failure,
            std::nullopt,
            true});
    }

    auto admitted = limiter_->admit({
        request.expected.profile_id,
        request.expected.image_id,
        request.recovery_operation_id,
        checkpoint.value().latest_complete_generation,
        request.observed_dirty_epoch});
    if (!admitted.ok()) {
        return Result<WorkerRestartRecoveryResult>::failure(admitted.error());
    }
    if (admitted.value().disposition
            == WorkerRestartAdmissionDisposition::limit_exhausted
        || admitted.value().disposition
            == WorkerRestartAdmissionDisposition::clock_rollback) {
        return Result<WorkerRestartRecoveryResult>::success({
            WorkerRestartRecoveryDisposition::needs_attention,
            failure,
            std::nullopt,
            true});
    }
    auto restarted = actions_->ensure_restarted_from_checkpoint(
        request, checkpoint.value().latest_complete_generation);
    if (!restarted.ok()) {
        return Result<WorkerRestartRecoveryResult>::failure(
            restarted.error());
    }
    if (!valid_restart_execution_proof(
            request,
            checkpoint.value().latest_complete_generation,
            restarted.value())) {
        return supervisor_failure<WorkerRestartRecoveryResult>(
            WorkerSupervisorError::recovery_proof_invalid,
            L"worker_recovery.restart_proof",
            L"Restart action returned stale or mismatched worker proof");
    }
    return Result<WorkerRestartRecoveryResult>::success({
        WorkerRestartRecoveryDisposition::restarted,
        failure,
        checkpoint.value().latest_complete_generation,
        true});
}

bool scm_failure_count_should_reset(
    const std::uint64_t last_failure_unix_ms,
    const std::uint64_t now_unix_ms) noexcept {
    constexpr std::uint64_t kResetMs =
        static_cast<std::uint64_t>(kScmFailureResetSeconds) * 1'000U;
    return now_unix_ms >= last_failure_unix_ms
        && now_unix_ms - last_failure_unix_ms >= kResetMs;
}

} // namespace myrd::management
