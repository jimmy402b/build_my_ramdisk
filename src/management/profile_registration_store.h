#pragma once

#include "src/management/service_core.h"

#include <memory>
#include <string_view>

namespace myrd::management {

// Production persistence adapter for the service-owned profile truth. The
// canonical user configuration remains profile.myrd/ProfileStore. Runtime
// truth is a separate atomic metadata record in the same protected profile
// directory; recovery reservations and forget tombstones live in the
// protected profiles root so they survive metadata-only profile removal.
class ProfileRegistrationStore final : public IServiceProfileStore {
public:
    static Result<ProfileRegistrationStore> open_existing(
        std::wstring_view trusted_program_data_root,
        profile::IProfileRepairBindingVerifier& repair_verifier);

    ProfileRegistrationStore(ProfileRegistrationStore&&) noexcept;
    ProfileRegistrationStore& operator=(ProfileRegistrationStore&&) noexcept;
    ~ProfileRegistrationStore() noexcept;

    ProfileRegistrationStore(const ProfileRegistrationStore&) = delete;
    ProfileRegistrationStore& operator=(const ProfileRegistrationStore&) =
        delete;

    Result<ManagedProfileSnapshot> resolve(
        std::wstring_view target) override;
    Result<ManagedProfileSnapshot> load(
        const product::Uuid& profile_id) override;
    Result<ManagedProfileSnapshot> publish_runtime(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate) override;
    Result<ManagedProfileSnapshot> publish_checkpoint(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint) override;
    Result<ManagedProfileSnapshot> publish_compaction(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint,
        const CompactionDurableState& proposed_compaction) override;
    Result<ManagedProfileSnapshot> publish_stop(
        const ManagedProfileSnapshot& expected,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate,
        const CheckpointDurableState& proposed_checkpoint,
        const CompactionDurableState& proposed_compaction,
        const StopDurableState& proposed_stop) override;
    Result<ManagedProfileSnapshot> publish_rebind(
        const ManagedProfileSnapshot& expected,
        const profile::ProfileRecord& proposed,
        const BundleRegistrationEvidence& verified_bundle,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate) override;
    Result<ManagedProfileSnapshot> load_repair_candidate(
        const product::Uuid& profile_id) override;
    Result<ManagedProfileSnapshot> repair_from_previous(
        const ManagedProfileSnapshot& expected_previous,
        const ProfileState& proposed_state,
        const OperationGateState& proposed_gate) override;
    Result<RecoveryCommandReservation> reserve_recovery_command(
        const ManagedProfileSnapshot& expected,
        const RecoveryCommandReservation& proposed) override;
    Result<std::optional<RecoveryCommandReservation>> find_recovery_command(
        const product::Uuid& operation_id) override;
    Result<ForgetTombstone> reserve_forget(
        const ManagedProfileSnapshot& expected,
        const ForgetTombstone& proposed) override;
    Result<std::optional<ForgetTombstone>> find_forget_tombstone(
        const product::Uuid& operation_id) override;
    Result<ForgetTombstone> remove_forget_metadata(
        const ForgetMetadataRemovalRequest& request,
        const ForgetSafetyEvidence& proof) override;

private:
    struct Impl;
    explicit ProfileRegistrationStore(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::management
