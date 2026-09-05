#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/host/volume_identity.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::host {

enum class ProvisioningPolicyError : std::uint32_t {
    invalid_expectation = 0x5001,
    wrong_lifecycle,
    source_not_new,
    ownership_unproven,
    already_provisioned,
    not_found,
    identity_conflict,
    ambiguous,
    not_raw,
    not_empty,
    protected_disk,
    read_only,
    offline,
    invalid_environment,
    malformed_result,
};

enum class ProvisioningExecutionError : std::uint32_t {
    open_failed = 0x5101,
    process_failed,
    timed_out,
    output_truncated,
    postcondition_failed,
    platform_exception,
    io_indeterminate,
};

enum class ProvisioningLifecycle : std::uint8_t {
    provisioning = 0,
    ready,
    unknown,
};

enum class ProvisioningSource : std::uint8_t {
    new_image = 0,
    imported,
    existing,
    recovered,
    unknown,
};

enum class ObservedDiskLayout : std::uint8_t {
    raw = 0,
    mbr,
    gpt,
    unknown,
};

struct NewDiskProvisioningExpectation final {
    HostOwnershipMarker owner;
    product::Uuid operation_id;
    PhysicalDiskIdentity physical_disk;
    ProvisioningLifecycle lifecycle{ProvisioningLifecycle::unknown};
    ProvisioningSource source{ProvisioningSource::unknown};
    bool operation_owns_session{};
    bool device_created_by_operation{};
    bool has_recorded_disk_identity{};
    bool has_recorded_partition_identity{};
    bool has_recorded_volume_identity{};

    bool operator==(const NewDiskProvisioningExpectation&) const noexcept =
        default;
};

struct DiskProtectionFlags final {
    bool is_system{};
    bool is_boot{};
    bool boots_from_disk{};
    bool has_pagefile{};
    bool has_hibernation_file{};
    bool has_crash_dump{};
    bool is_clustered{};
    bool is_highly_available{};
    bool is_shadow_copy{};
    bool is_read_only{};
    bool is_offline{};

    bool operator==(const DiskProtectionFlags&) const noexcept = default;
};

struct NewDiskObservation final {
    PhysicalDiskIdentity physical_disk;
    ObservedDiskLayout layout{ObservedDiskLayout::unknown};
    std::uint32_t partition_count{};
    std::uint32_t volume_count{};
    std::uint32_t extent_count{};
    DiskProtectionFlags protection;

    bool operator==(const NewDiskObservation&) const noexcept = default;
};

class VerifiedNewDisk final {
public:
    VerifiedNewDisk(const VerifiedNewDisk&) = default;
    VerifiedNewDisk& operator=(const VerifiedNewDisk&) = default;
    VerifiedNewDisk(VerifiedNewDisk&&) noexcept = default;
    VerifiedNewDisk& operator=(VerifiedNewDisk&&) noexcept = default;

    const HostOwnershipMarker& owner() const noexcept;
    const product::Uuid& operation_id() const noexcept;
    const PhysicalDiskIdentity& physical_disk() const noexcept;
    const NewDiskObservation& observation() const noexcept;

private:
    VerifiedNewDisk(
        NewDiskProvisioningExpectation expectation,
        NewDiskObservation observation) noexcept;

    NewDiskProvisioningExpectation expectation_;
    NewDiskObservation observation_;

    friend Result<VerifiedNewDisk> assess_new_disk_provisioning(
        const NewDiskProvisioningExpectation&,
        std::span<const NewDiskObservation>);
};

Result<VerifiedNewDisk> assess_new_disk_provisioning(
    const NewDiskProvisioningExpectation& expected,
    std::span<const NewDiskObservation> observations);

struct ProvisioningCommand final {
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::chrono::milliseconds timeout{};
    std::uint64_t maximum_stdout_bytes{};
    std::uint64_t maximum_stderr_bytes{};
    std::string category;

    bool operator==(const ProvisioningCommand&) const noexcept = default;
};

Result<ProvisioningCommand> build_provisioning_command(
    const VerifiedNewDisk& verified,
    std::wstring_view windows_directory);

struct ProvisionalProvisioningIdentity final {
    product::Uuid gpt_disk_guid;
    product::Uuid partition_guid;
    product::Uuid volume_guid;

    bool operator==(const ProvisionalProvisioningIdentity&) const noexcept =
        default;
};

Result<ProvisionalProvisioningIdentity> parse_provisioning_result(
    std::string_view output);

struct ProvisioningProcessResult final {
    std::uint32_t exit_code{};
    bool timed_out{};
    bool stdout_truncated{};
    bool stderr_truncated{};
    std::string stdout_text;
    std::string stderr_text;

    bool operator==(const ProvisioningProcessResult&) const noexcept = default;
};

enum class ProvisioningMutationState : std::uint8_t {
    raw_unchanged = 0,
    planned_gpt_empty,
    planned_partition_no_volume,
    planned_partition_with_volume,
    unexpected_layout,
};

struct ProvisioningMutationAudit final {
    ProvisioningMutationState state{
        ProvisioningMutationState::unexpected_layout};
    PhysicalDiskIdentity physical_disk;
    ObservedDiskLayout layout{ObservedDiskLayout::unknown};
    std::uint32_t partition_count{};
    std::uint32_t volume_count{};
    std::uint32_t extent_count{};
    bool planned_disk_guid_matches{};
    bool planned_partition_guid_matches{};
    bool planned_partition_layout_matches{};
    bool volume_extent_matches{};

    bool operator==(const ProvisioningMutationAudit&) const noexcept = default;
};

class IProvisioningSession {
public:
    virtual ~IProvisioningSession() = default;

    virtual Result<NewDiskObservation> observe(
        const OperationContext& context) = 0;
    virtual Result<ProvisioningProcessResult> run(
        const ProvisioningCommand& command,
        const OperationContext& context) = 0;
    virtual Result<ProvisioningMutationAudit> audit_after_mutation(
        const OperationContext& context) = 0;
    virtual Result<VerifiedVolumeIdentity> verify_postcondition(
        const VolumeIdentityExpectation& expected,
        const OperationContext& context) = 0;
};

class IProvisioningPlatform {
public:
    virtual ~IProvisioningPlatform() = default;

    virtual Result<std::unique_ptr<IProvisioningSession>> open_exact(
        const NewDiskProvisioningExpectation& expected,
        const OperationContext& context) = 0;
    virtual Result<std::wstring> windows_directory() = 0;
};

class ProvisionedDisk final {
public:
    ProvisionedDisk(const ProvisionedDisk&) = default;
    ProvisionedDisk& operator=(const ProvisionedDisk&) = default;
    ProvisionedDisk(ProvisionedDisk&&) noexcept = default;
    ProvisionedDisk& operator=(ProvisionedDisk&&) noexcept = default;

    const product::Uuid& operation_id() const noexcept;
    const VerifiedVolumeIdentity& identity() const noexcept;

private:
    ProvisionedDisk(
        product::Uuid operation_id,
        VerifiedVolumeIdentity identity) noexcept;

    product::Uuid operation_id_;
    VerifiedVolumeIdentity identity_;

    friend struct ProvisionedDiskProofFactory;
};

struct ProvisioningOutcome final {
    bool opened{};
    bool preflight_verified{};
    bool process_started{};
    bool process_completed{};
    bool postmutation_audited{};
    bool postcondition_verified{};
    std::optional<Error> error;
    std::optional<Error> audit_error;
    std::optional<ProvisioningMutationAudit> audit;
    std::optional<ProvisionedDisk> proof;

    [[nodiscard]] bool ok() const noexcept {
        return opened && preflight_verified && process_started
            && process_completed && postcondition_verified
            && postmutation_audited && proof.has_value()
            && !error.has_value() && !audit_error.has_value();
    }
};

ProvisioningOutcome provision_new_disk(
    IProvisioningPlatform& platform,
    const NewDiskProvisioningExpectation& expected,
    const OperationContext& context);

std::unique_ptr<IProvisioningPlatform> make_windows_provisioning_platform();

#ifdef MYRD_ENABLE_TEST_SEAMS
namespace testing {

struct GptLayoutPlan final {
    product::Uuid disk_guid;
    product::Uuid partition_guid;
    std::uint64_t partition_starting_offset{};
    std::uint64_t partition_length_bytes{};

    bool operator==(const GptLayoutPlan&) const noexcept = default;
};

Result<ProvisioningProcessResult> run_windows_provisioning_process(
    const ProvisioningCommand& command,
    const OperationContext& context);

Result<ProvisioningCommand> build_windows_disk_observation_command(
    const PhysicalDiskIdentity& expected,
    std::wstring_view windows_directory);

Result<NewDiskObservation> parse_windows_disk_observation(
    const PhysicalDiskIdentity& physical_disk,
    ObservedDiskLayout handle_layout,
    std::uint32_t handle_partition_count,
    std::uint32_t handle_volume_count,
    std::uint32_t handle_extent_count,
    bool handle_read_only,
    bool handle_offline,
    std::string_view output);

Result<GptLayoutPlan> plan_windows_gpt_layout(
    const product::Uuid& operation_id,
    std::uint64_t disk_length_bytes,
    std::uint64_t starting_usable_offset,
    std::uint64_t usable_length_bytes,
    std::uint32_t bytes_per_sector);

bool provisioning_volume_requires_disk_extents(
    std::uint32_t device_type) noexcept;

ObservedDiskLayout classify_windows_provisioning_layout(
    std::uint32_t partition_style,
    std::uint32_t partition_count,
    std::uint32_t mbr_signature,
    std::uint32_t mbr_checksum) noexcept;

} // namespace testing
#endif

} // namespace myrd::host
