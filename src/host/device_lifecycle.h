#pragma once

#include "src/common/result.h"
#include "src/driver/aim_protocol.h"
#include "src/host/volume_identity.h"
#include "src/product/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace myrd::host {

enum class LifecycleCommand : std::uint8_t {
    create_and_start = 0,
    create_stopped,
    import_stopped,
    start,
};

enum class RegisteredLifecycle : std::uint8_t {
    absent = 0,
    provisioning,
    ready,
};

enum class LifecycleIdentityState : std::uint8_t {
    none = 0,
    complete,
    partial_or_mismatched,
};

enum class LifecyclePhase : std::uint8_t {
    create_and_verify_bundle = 0,
    publish_provisioning_profile,
    inspect_bundle_read_only,
    publish_imported_ready_profile,
    create_runtime_and_worker,
    acquire_writer_lock_and_recover,
    start_proxy,
    create_exact_device,
    locate_exact_physical_drive,
    prove_new_uninitialized,
    provision_gpt_ntfs,
    record_volume_identity,
    verify_existing_volume_identity,
    flush_exact_volume,
    first_checkpoint,
    publish_ready_profile,
    assign_exact_mount,
    readiness_io,
};

enum class LifecycleResource : std::uint8_t {
    runtime_and_worker = 0,
    writer_lock,
    proxy,
    exact_device,
    mount,
};

struct LifecycleOwnershipToken final {
    product::Uuid operation_id;
    LifecycleResource resource{LifecycleResource::runtime_and_worker};
    std::uint64_t nonce{};

    bool operator==(const LifecycleOwnershipToken&) const noexcept = default;
};

enum class LifecyclePlanDisposition : std::uint8_t {
    allowed = 0,
    invalid_request,
    ambiguous_provisioning_identity,
};

enum class LifecyclePhaseDisposition : std::uint8_t {
    succeeded = 0,
    transient_failure,
    needs_attention_failure,
    commit_indeterminate,
};

enum class LifecycleCompletion : std::uint8_t {
    no_profile = 0,
    provisioning_stopped,
    ready_stopped,
    running,
    commit_indeterminate,
};

enum class LifecycleProfilePublication : std::uint8_t {
    absent = 0,
    provisioning,
    ready,
    commit_indeterminate,
};

struct LifecycleRequest final {
    product::Uuid operation_id;
    product::Uuid profile_id;
    LifecycleCommand command{LifecycleCommand::start};
    RegisteredLifecycle registered{RegisteredLifecycle::absent};
    LifecycleIdentityState identity{LifecycleIdentityState::none};
};

inline constexpr std::size_t kMaximumLifecyclePhases = 15;

struct LifecyclePlan final {
    LifecyclePlanDisposition disposition{
        LifecyclePlanDisposition::invalid_request};
    std::array<LifecyclePhase, kMaximumLifecyclePhases> phases{};
    std::size_t phase_count{};

    std::span<const LifecyclePhase> phase_span() const noexcept {
        if (phase_count > phases.size()) return {};
        return std::span<const LifecyclePhase>(phases.data(), phase_count);
    }
};

struct LifecyclePhaseExecution final {
    LifecyclePhaseDisposition disposition{
        LifecyclePhaseDisposition::succeeded};
    std::optional<LifecycleOwnershipToken> newly_owned;
};

class ILifecyclePhaseExecutor {
public:
    virtual ~ILifecyclePhaseExecutor() noexcept = default;
    virtual LifecyclePhaseExecution execute(
        const LifecycleRequest& request,
        LifecyclePhase phase) noexcept = 0;
    virtual bool cleanup_new_resource(
        const LifecycleRequest& request,
        const LifecycleOwnershipToken& token) noexcept = 0;
};

struct LifecycleRunResult final {
    bool succeeded{};
    LifecycleCompletion completion{LifecycleCompletion::no_profile};
    std::optional<LifecyclePhase> failed_phase;
    bool bundle_preserved{};
    bool profile_published{};
    LifecycleProfilePublication profile_publication{
        LifecycleProfilePublication::absent};
    bool volume_identity_recorded{};
    bool first_checkpoint_acknowledged{};
    bool ready_published{};
    bool needs_attention{};
    bool cleanup_complete{true};
    std::vector<LifecycleResource> cleaned_resources;
};

LifecyclePlan plan_lifecycle(const LifecycleRequest& request) noexcept;
LifecycleRunResult run_lifecycle(
    const LifecycleRequest& request,
    ILifecyclePhaseExecutor& executor) noexcept;

enum class ResidualDeviceError : std::uint32_t {
    invalid_identity = 0x5B01,
    enumeration_failed,
    removal_failed,
};

struct ResidualDeviceIdentity final {
    HostOwnershipMarker owner;
    PhysicalDiskIdentity physical_disk;

    bool operator==(const ResidualDeviceIdentity&) const noexcept = default;
};

struct ResidualDeviceObservation final {
    aim::DeviceId device_id{};
    bool product_proxy{};
    std::optional<HostOwnershipMarker> registered_owner;
    PhysicalDiskIdentity physical_disk;

    bool operator==(const ResidualDeviceObservation&) const noexcept =
        default;
};

struct ResidualDeviceRemovalProof final {
    ResidualDeviceIdentity removed_identity;
    bool removed{};
};

class IResidualDeviceControl {
public:
    virtual ~IResidualDeviceControl() noexcept = default;
    virtual Result<std::vector<ResidualDeviceObservation>>
        enumerate_devices() = 0;
    // Production must revalidate the complete expected owner and physical
    // identity immediately before issuing the exact single-device remove.
    // An ID-only implementation does not satisfy this contract.
    virtual Result<ResidualDeviceRemovalProof> remove_if_still_exact(
        const ResidualDeviceIdentity& expected) = 0;
};

enum class ResidualDeviceDisposition : std::uint8_t {
    absent = 0,
    exact_device_removed,
    needs_attention_foreign,
    needs_attention_identity_changed,
    needs_attention_ambiguous,
    needs_attention_removal_unconfirmed,
};

struct ResidualDeviceReconciliation final {
    ResidualDeviceDisposition disposition{
        ResidualDeviceDisposition::needs_attention_ambiguous};
    bool restart_safe{};
    bool mutation_attempted{};
};

Result<ResidualDeviceReconciliation> reconcile_residual_device(
    const ResidualDeviceIdentity& expected,
    IResidualDeviceControl& control);

} // namespace myrd::host
