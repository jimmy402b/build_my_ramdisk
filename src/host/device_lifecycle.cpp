#include "src/host/device_lifecycle.h"

#include <algorithm>

namespace myrd::host {
namespace {

bool zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

template <typename T>
Result<T> residual_error(
    const ResidualDeviceError code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code),
        operation,
        message});
}

bool valid_residual_identity(
    const ResidualDeviceIdentity& expected) {
    if (zero_uuid(expected.owner.profile_uuid)
        || zero_uuid(expected.owner.image_uuid)
        || expected.owner.profile_revision == 0
        || expected.owner.session_generation == 0
        || expected.owner.aim_device_id.value == aim::kAllDevices
        || expected.owner.aim_device_id.value == aim::kAutoDeviceNumber
        || expected.physical_disk.length_bytes == 0) {
        return false;
    }
    const auto canonical_path = std::wstring(LR"(\\.\PhysicalDrive)")
        + std::to_wstring(expected.physical_disk.physical_drive_number);
    if (expected.physical_disk.path != canonical_path) return false;
    const auto address = aim::decode_device_address(
        expected.owner.aim_device_id);
    return address.path_id == expected.physical_disk.path_id
        && address.target_id == expected.physical_disk.target_id
        && address.lun == expected.physical_disk.lun;
}

bool residual_observation_is_related(
    const ResidualDeviceObservation& observation,
    const ResidualDeviceIdentity& expected) noexcept {
    const bool related_owner = observation.registered_owner.has_value()
        && (observation.registered_owner->profile_uuid
                == expected.owner.profile_uuid
            || observation.registered_owner->image_uuid
                == expected.owner.image_uuid);
    const bool related_physical =
        observation.physical_disk.physical_drive_number
                == expected.physical_disk.physical_drive_number
        || observation.physical_disk.path == expected.physical_disk.path;
    return observation.device_id == expected.owner.aim_device_id
        || related_owner || related_physical;
}

void append(LifecyclePlan& plan, LifecyclePhase phase) noexcept {
    if (plan.phase_count < plan.phases.size()) {
        plan.phases[plan.phase_count++] = phase;
    }
}

void append_runtime_prefix(LifecyclePlan& plan) noexcept {
    append(plan, LifecyclePhase::create_runtime_and_worker);
    append(plan, LifecyclePhase::acquire_writer_lock_and_recover);
    append(plan, LifecyclePhase::start_proxy);
    append(plan, LifecyclePhase::create_exact_device);
    append(plan, LifecyclePhase::locate_exact_physical_drive);
}

void append_provisioning(
    LifecyclePlan& plan,
    LifecycleIdentityState identity) noexcept {
    if (identity == LifecycleIdentityState::none) {
        append(plan, LifecyclePhase::prove_new_uninitialized);
        append(plan, LifecyclePhase::provision_gpt_ntfs);
        append(plan, LifecyclePhase::record_volume_identity);
    } else {
        append(plan, LifecyclePhase::verify_existing_volume_identity);
    }
    append(plan, LifecyclePhase::flush_exact_volume);
    append(plan, LifecyclePhase::first_checkpoint);
    append(plan, LifecyclePhase::publish_ready_profile);
}

void append_mount_and_readiness(LifecyclePlan& plan) noexcept {
    append(plan, LifecyclePhase::assign_exact_mount);
    append(plan, LifecyclePhase::readiness_io);
}

std::optional<LifecycleResource> owned_resource(
    LifecyclePhase phase) noexcept {
    switch (phase) {
    case LifecyclePhase::create_runtime_and_worker:
        return LifecycleResource::runtime_and_worker;
    case LifecyclePhase::acquire_writer_lock_and_recover:
        return LifecycleResource::writer_lock;
    case LifecyclePhase::start_proxy:
        return LifecycleResource::proxy;
    case LifecyclePhase::create_exact_device:
        return LifecycleResource::exact_device;
    case LifecyclePhase::assign_exact_mount:
        return LifecycleResource::mount;
    default:
        return std::nullopt;
    }
}

void apply_phase_evidence(
    LifecycleRunResult& result,
    LifecyclePhase phase) noexcept {
    switch (phase) {
    case LifecyclePhase::create_and_verify_bundle:
        result.bundle_preserved = true;
        break;
    case LifecyclePhase::publish_provisioning_profile:
        result.profile_published = true;
        result.profile_publication =
            LifecycleProfilePublication::provisioning;
        break;
    case LifecyclePhase::publish_imported_ready_profile:
        result.profile_published = true;
        result.ready_published = true;
        result.profile_publication = LifecycleProfilePublication::ready;
        break;
    case LifecyclePhase::record_volume_identity:
        result.volume_identity_recorded = true;
        break;
    case LifecyclePhase::first_checkpoint:
        result.first_checkpoint_acknowledged = true;
        break;
    case LifecyclePhase::publish_ready_profile:
        result.ready_published = true;
        result.profile_publication = LifecycleProfilePublication::ready;
        break;
    default:
        break;
    }
}

LifecycleCompletion stopped_completion(
    const LifecycleRunResult& result) noexcept {
    if (!result.profile_published) return LifecycleCompletion::no_profile;
    return result.ready_published
        ? LifecycleCompletion::ready_stopped
        : LifecycleCompletion::provisioning_stopped;
}

void cleanup_reverse(
    const LifecycleRequest& request,
    ILifecyclePhaseExecutor& executor,
    std::vector<LifecycleOwnershipToken>& resources,
    LifecycleRunResult& result) noexcept {
    for (auto cursor = resources.rbegin(); cursor != resources.rend(); ++cursor) {
        result.cleaned_resources.push_back(cursor->resource);
        if (!executor.cleanup_new_resource(request, *cursor)) {
            result.cleanup_complete = false;
        }
    }
}

bool command_requires_attention_on_failure(
    const LifecycleRequest& request,
    LifecyclePhaseDisposition disposition,
    bool profile_published) noexcept {
    if (disposition == LifecyclePhaseDisposition::commit_indeterminate) {
        return true;
    }
    if (!profile_published) return false;
    if (disposition == LifecyclePhaseDisposition::needs_attention_failure
        || disposition == LifecyclePhaseDisposition::commit_indeterminate) {
        return true;
    }
    return request.command == LifecycleCommand::create_and_start
        || (request.command == LifecycleCommand::start
            && request.registered == RegisteredLifecycle::provisioning);
}

void initialize_registered_evidence(
    const LifecycleRequest& request,
    LifecycleRunResult& result) noexcept {
    if (request.registered == RegisteredLifecycle::absent) return;
    result.bundle_preserved = true;
    result.profile_published = true;
    result.profile_publication =
        request.registered == RegisteredLifecycle::ready
        ? LifecycleProfilePublication::ready
        : LifecycleProfilePublication::provisioning;
    result.ready_published =
        request.registered == RegisteredLifecycle::ready;
    result.volume_identity_recorded =
        request.identity == LifecycleIdentityState::complete;
    result.completion = result.ready_published
        ? LifecycleCompletion::ready_stopped
        : LifecycleCompletion::provisioning_stopped;
}

bool token_matches_phase(
    const LifecycleRequest& request,
    LifecyclePhase phase,
    const LifecycleOwnershipToken& token,
    const std::vector<LifecycleOwnershipToken>& accepted) noexcept {
    const auto expected = owned_resource(phase);
    if (!expected.has_value() || token.operation_id != request.operation_id
        || token.resource != expected.value() || token.nonce == 0) {
        return false;
    }
    return std::none_of(
        accepted.begin(), accepted.end(), [&](const auto& prior) {
            return prior.operation_id == token.operation_id
                && prior.nonce == token.nonce;
        });
}

void mark_publication_indeterminate(
    LifecycleRunResult& result,
    LifecyclePhase phase) noexcept {
    if (phase != LifecyclePhase::publish_provisioning_profile
        && phase != LifecyclePhase::publish_imported_ready_profile
        && phase != LifecyclePhase::publish_ready_profile) {
        return;
    }
    if (phase != LifecyclePhase::publish_ready_profile) {
        result.profile_published = false;
    }
    result.ready_published = false;
    result.profile_publication =
        LifecycleProfilePublication::commit_indeterminate;
}

} // namespace

LifecyclePlan plan_lifecycle(const LifecycleRequest& request) noexcept {
    LifecyclePlan plan{};
    if (zero_uuid(request.operation_id) || zero_uuid(request.profile_id)) {
        return plan;
    }

    switch (request.command) {
    case LifecycleCommand::create_and_start:
    case LifecycleCommand::create_stopped:
        if (request.registered != RegisteredLifecycle::absent
            || request.identity != LifecycleIdentityState::none) {
            return plan;
        }
        plan.disposition = LifecyclePlanDisposition::allowed;
        append(plan, LifecyclePhase::create_and_verify_bundle);
        append(plan, LifecyclePhase::publish_provisioning_profile);
        if (request.command == LifecycleCommand::create_stopped) return plan;
        append_runtime_prefix(plan);
        append_provisioning(plan, LifecycleIdentityState::none);
        append_mount_and_readiness(plan);
        return plan;
    case LifecycleCommand::import_stopped:
        if (request.registered != RegisteredLifecycle::absent
            || request.identity != LifecycleIdentityState::complete) {
            return plan;
        }
        plan.disposition = LifecyclePlanDisposition::allowed;
        append(plan, LifecyclePhase::inspect_bundle_read_only);
        append(plan, LifecyclePhase::publish_imported_ready_profile);
        return plan;
    case LifecycleCommand::start:
        if (request.registered == RegisteredLifecycle::absent) return plan;
        if (request.identity
            == LifecycleIdentityState::partial_or_mismatched) {
            plan.disposition =
                LifecyclePlanDisposition::ambiguous_provisioning_identity;
            return plan;
        }
        if (request.registered == RegisteredLifecycle::ready
            && request.identity != LifecycleIdentityState::complete) {
            return plan;
        }
        plan.disposition = LifecyclePlanDisposition::allowed;
        append_runtime_prefix(plan);
        if (request.registered == RegisteredLifecycle::provisioning) {
            append_provisioning(plan, request.identity);
        } else {
            append(plan, LifecyclePhase::verify_existing_volume_identity);
        }
        append_mount_and_readiness(plan);
        return plan;
    }
    return plan;
}

LifecycleRunResult run_lifecycle(
    const LifecycleRequest& request,
    ILifecyclePhaseExecutor& executor) noexcept {
    LifecycleRunResult result{};
    initialize_registered_evidence(request, result);
    const auto plan = plan_lifecycle(request);
    if (plan.disposition != LifecyclePlanDisposition::allowed) {
        if (plan.disposition
                == LifecyclePlanDisposition::ambiguous_provisioning_identity
            && request.registered == RegisteredLifecycle::provisioning) {
            result.needs_attention = true;
        }
        return result;
    }

    std::vector<LifecycleOwnershipToken> resources;
    resources.reserve(5);
    for (const auto phase : plan.phase_span()) {
        auto execution = executor.execute(request, phase);
        if (execution.newly_owned.has_value()) {
            if (!token_matches_phase(
                    request, phase, execution.newly_owned.value(), resources)) {
                execution.disposition =
                    LifecyclePhaseDisposition::needs_attention_failure;
                execution.newly_owned.reset();
            } else {
                resources.push_back(execution.newly_owned.value());
            }
        }
        if (execution.disposition != LifecyclePhaseDisposition::succeeded) {
            if (execution.disposition
                == LifecyclePhaseDisposition::commit_indeterminate) {
                mark_publication_indeterminate(result, phase);
            }
            result.failed_phase = phase;
            result.completion = execution.disposition
                    == LifecyclePhaseDisposition::commit_indeterminate
                ? LifecycleCompletion::commit_indeterminate
                : stopped_completion(result);
            result.needs_attention = command_requires_attention_on_failure(
                request, execution.disposition, result.profile_published);
            cleanup_reverse(request, executor, resources, result);
            return result;
        }
        apply_phase_evidence(result, phase);
    }

    result.succeeded = true;
    switch (request.command) {
    case LifecycleCommand::create_stopped:
        result.completion = LifecycleCompletion::provisioning_stopped;
        break;
    case LifecycleCommand::import_stopped:
        result.completion = LifecycleCompletion::ready_stopped;
        break;
    case LifecycleCommand::create_and_start:
    case LifecycleCommand::start:
        result.completion = LifecycleCompletion::running;
        break;
    }
    return result;
}

Result<ResidualDeviceReconciliation> reconcile_residual_device(
    const ResidualDeviceIdentity& expected,
    IResidualDeviceControl& control) {
    if (!valid_residual_identity(expected)) {
        return residual_error<ResidualDeviceReconciliation>(
            ResidualDeviceError::invalid_identity,
            L"residual_device.identity",
            L"Stored residual-device identity is invalid or unsafe");
    }
    auto enumerated = control.enumerate_devices();
    if (!enumerated.ok()) {
        return Result<ResidualDeviceReconciliation>::failure(
            enumerated.error());
    }
    std::vector<ResidualDeviceObservation> candidates;
    for (const auto& observation : enumerated.value()) {
        if (residual_observation_is_related(observation, expected)) {
            candidates.push_back(observation);
        }
    }
    if (candidates.empty()) {
        return Result<ResidualDeviceReconciliation>::success({
            ResidualDeviceDisposition::absent, true, false});
    }
    if (candidates.size() != 1) {
        return Result<ResidualDeviceReconciliation>::success({
            ResidualDeviceDisposition::needs_attention_ambiguous,
            false,
            false});
    }
    const auto& candidate = candidates.front();
    if (candidate.device_id == expected.owner.aim_device_id
        && (!candidate.product_proxy
            || !candidate.registered_owner.has_value())) {
        return Result<ResidualDeviceReconciliation>::success({
            ResidualDeviceDisposition::needs_attention_foreign,
            false,
            false});
    }
    if (!candidate.product_proxy
        || !candidate.registered_owner.has_value()
        || candidate.device_id != expected.owner.aim_device_id
        || candidate.registered_owner.value() != expected.owner
        || candidate.physical_disk != expected.physical_disk) {
        return Result<ResidualDeviceReconciliation>::success({
            ResidualDeviceDisposition::needs_attention_identity_changed,
            false,
            false});
    }

    auto removed = control.remove_if_still_exact(expected);
    if (!removed.ok()) {
        return Result<ResidualDeviceReconciliation>::failure(
            removed.error());
    }
    if (!removed.value().removed
        || removed.value().removed_identity != expected) {
        return residual_error<ResidualDeviceReconciliation>(
            ResidualDeviceError::removal_failed,
            L"residual_device.remove_proof",
            L"Residual-device removal returned no exact identity proof");
    }
    auto after = control.enumerate_devices();
    if (!after.ok()) {
        return Result<ResidualDeviceReconciliation>::failure(after.error());
    }
    const bool still_present = std::any_of(
        after.value().begin(), after.value().end(),
        [&](const ResidualDeviceObservation& observation) {
            return residual_observation_is_related(observation, expected);
        });
    if (still_present) {
        return Result<ResidualDeviceReconciliation>::success({
            ResidualDeviceDisposition::needs_attention_removal_unconfirmed,
            false,
            true});
    }
    return Result<ResidualDeviceReconciliation>::success({
        ResidualDeviceDisposition::exact_device_removed,
        true,
        true});
}

} // namespace myrd::host
