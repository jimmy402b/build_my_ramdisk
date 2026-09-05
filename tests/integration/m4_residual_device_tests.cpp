#include "src/host/device_lifecycle.h"
#include "tests/support/test_framework.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::host;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

ResidualDeviceIdentity expected_identity() {
    return {
        {
            uuid("11111111-1111-4111-8111-111111111111"),
            uuid("22222222-2222-4222-8222-222222222222"),
            11,
            7,
            aim::DeviceId{0x00000200}},
        {
            9,
            LR"(\\.\PhysicalDrive9)",
            8ULL << 30,
            3,
            0,
            2,
            0}};
}

ResidualDeviceObservation exact_observation() {
    const auto expected = expected_identity();
    return {
        expected.owner.aim_device_id,
        true,
        expected.owner,
        expected.physical_disk};
}

Error injected_error(const wchar_t* operation) {
    return {ErrorDomain::win32, 5, operation, L"injected failure"};
}

class DeviceControl final : public IResidualDeviceControl {
public:
    Result<std::vector<ResidualDeviceObservation>> enumerate_devices() override {
        ++enumerations;
        return Result<std::vector<ResidualDeviceObservation>>::success(
            observations);
    }

    Result<ResidualDeviceRemovalProof> remove_if_still_exact(
        const ResidualDeviceIdentity& expected) override {
        ++removals;
        removed_ids.push_back(expected.owner.aim_device_id);
        if (identity_changed_before_remove) {
            for (auto& observation : observations) {
                if (observation.device_id == expected.owner.aim_device_id) {
                    ++observation.physical_disk.length_bytes;
                }
            }
        }
        const auto exact = std::find_if(
            observations.begin(), observations.end(),
            [&](const ResidualDeviceObservation& observation) {
                return observation.device_id == expected.owner.aim_device_id
                    && observation.product_proxy
                    && observation.registered_owner
                        == std::optional<HostOwnershipMarker>(expected.owner)
                    && observation.physical_disk == expected.physical_disk;
            });
        if (exact == observations.end()) {
            return Result<ResidualDeviceRemovalProof>::failure(
                injected_error(L"residual.identity_changed_before_remove"));
        }
        if (remove_error) {
            return Result<ResidualDeviceRemovalProof>::failure(
                injected_error(L"residual.remove"));
        }
        if (!sticky_after_remove) {
            observations.erase(
                std::remove_if(
                    observations.begin(), observations.end(),
                    [&](const ResidualDeviceObservation& candidate) {
                        return candidate.device_id
                            == expected.owner.aim_device_id;
                    }),
                observations.end());
        }
        if (after_remove_observation.has_value()) {
            observations.push_back(after_remove_observation.value());
        }
        auto proof_identity = expected;
        if (forge_removal_proof) ++proof_identity.physical_disk.length_bytes;
        return Result<ResidualDeviceRemovalProof>::success({
            std::move(proof_identity), true});
    }

    std::vector<ResidualDeviceObservation> observations;
    std::vector<aim::DeviceId> removed_ids;
    std::size_t enumerations{};
    std::size_t removals{};
    bool remove_error{};
    bool sticky_after_remove{};
    bool identity_changed_before_remove{};
    bool forge_removal_proof{};
    std::optional<ResidualDeviceObservation> after_remove_observation;
};

TEST_CASE(residual_reconciliation_accepts_absent_device_without_mutation) {
    DeviceControl control;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        ResidualDeviceDisposition::absent);
    REQUIRE(result.value().restart_safe);
    REQUIRE(!result.value().mutation_attempted);
    REQUIRE_EQ(control.enumerations, 1U);
    REQUIRE_EQ(control.removals, 0U);
}

TEST_CASE(residual_reconciliation_removes_only_one_exact_product_match) {
    DeviceControl control;
    control.observations.push_back(exact_observation());
    auto unrelated = exact_observation();
    unrelated.device_id = aim::DeviceId{0x00000300};
    unrelated.registered_owner->aim_device_id = unrelated.device_id;
    unrelated.registered_owner->profile_uuid =
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    unrelated.registered_owner->image_uuid =
        uuid("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    unrelated.physical_disk.physical_drive_number = 10;
    unrelated.physical_disk.path = LR"(\\.\PhysicalDrive10)";
    control.observations.push_back(unrelated);

    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        ResidualDeviceDisposition::exact_device_removed);
    REQUIRE(result.value().restart_safe);
    REQUIRE(result.value().mutation_attempted);
    REQUIRE_EQ(control.enumerations, 2U);
    REQUIRE_EQ(control.removals, 1U);
    REQUIRE_EQ(control.removed_ids.front(), expected_identity().owner.aim_device_id);
    REQUIRE_EQ(control.observations.size(), 1U);
    REQUIRE_EQ(control.observations.front().device_id, unrelated.device_id);
}

TEST_CASE(residual_reconciliation_ignores_unrelated_device_ids) {
    DeviceControl control;
    auto unrelated = exact_observation();
    unrelated.device_id = aim::DeviceId{0x00000300};
    unrelated.registered_owner->aim_device_id = unrelated.device_id;
    unrelated.registered_owner->profile_uuid =
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    unrelated.registered_owner->image_uuid =
        uuid("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    unrelated.physical_disk.physical_drive_number = 10;
    unrelated.physical_disk.path = LR"(\\.\PhysicalDrive10)";
    control.observations.push_back(unrelated);
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().disposition, ResidualDeviceDisposition::absent);
    REQUIRE_EQ(control.removals, 0U);
}

TEST_CASE(residual_reconciliation_refuses_foreign_or_unowned_candidate) {
    for (const bool product_proxy : {false, true}) {
        DeviceControl control;
        auto foreign = exact_observation();
        foreign.product_proxy = product_proxy;
        foreign.registered_owner.reset();
        control.observations.push_back(foreign);
        auto result = reconcile_residual_device(expected_identity(), control);
        REQUIRE(result.ok());
        REQUIRE_EQ(
            result.value().disposition,
            ResidualDeviceDisposition::needs_attention_foreign);
        REQUIRE(!result.value().restart_safe);
        REQUIRE_EQ(control.removals, 0U);
    }
}

TEST_CASE(residual_reconciliation_refuses_changed_owner_or_physical_identity) {
    {
        DeviceControl control;
        auto changed = exact_observation();
        ++changed.registered_owner->session_generation;
        control.observations.push_back(changed);
        auto result = reconcile_residual_device(expected_identity(), control);
        REQUIRE(result.ok());
        REQUIRE_EQ(
            result.value().disposition,
            ResidualDeviceDisposition::needs_attention_identity_changed);
        REQUIRE_EQ(control.removals, 0U);
    }
    {
        DeviceControl control;
        auto changed = exact_observation();
        changed.device_id = aim::DeviceId{0x00000300};
        control.observations.push_back(changed);
        auto result = reconcile_residual_device(expected_identity(), control);
        REQUIRE(result.ok());
        REQUIRE_EQ(
            result.value().disposition,
            ResidualDeviceDisposition::needs_attention_identity_changed);
        REQUIRE_EQ(control.removals, 0U);
    }
    {
        DeviceControl control;
        auto changed = exact_observation();
        ++changed.physical_disk.length_bytes;
        control.observations.push_back(changed);
        auto result = reconcile_residual_device(expected_identity(), control);
        REQUIRE(result.ok());
        REQUIRE_EQ(
            result.value().disposition,
            ResidualDeviceDisposition::needs_attention_identity_changed);
        REQUIRE_EQ(control.removals, 0U);
    }
}

TEST_CASE(residual_reconciliation_refuses_ambiguous_duplicate_candidate) {
    DeviceControl control;
    control.observations = {exact_observation(), exact_observation()};
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        ResidualDeviceDisposition::needs_attention_ambiguous);
    REQUIRE(!result.value().restart_safe);
    REQUIRE_EQ(control.removals, 0U);
}

TEST_CASE(residual_reconciliation_fails_closed_on_remove_error) {
    DeviceControl control;
    control.observations = {exact_observation()};
    control.remove_error = true;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(!result.ok());
    REQUIRE_EQ(control.removals, 1U);
    REQUIRE_EQ(control.enumerations, 1U);
}

TEST_CASE(residual_reconciliation_revalidates_full_identity_before_remove) {
    DeviceControl control;
    control.observations = {exact_observation()};
    control.identity_changed_before_remove = true;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(!result.ok());
    REQUIRE_EQ(control.removals, 1U);
    REQUIRE_EQ(control.observations.size(), 1U);
}

TEST_CASE(residual_reconciliation_requires_post_remove_absence_proof) {
    DeviceControl control;
    control.observations = {exact_observation()};
    control.sticky_after_remove = true;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        ResidualDeviceDisposition::needs_attention_removal_unconfirmed);
    REQUIRE(!result.value().restart_safe);
    REQUIRE(result.value().mutation_attempted);
    REQUIRE_EQ(control.enumerations, 2U);
    REQUIRE_EQ(control.removals, 1U);
}

TEST_CASE(residual_reconciliation_rejects_changed_related_post_remove_device) {
    DeviceControl control;
    control.observations = {exact_observation()};
    auto changed = exact_observation();
    changed.device_id = aim::DeviceId{0x00000300};
    control.after_remove_observation = changed;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(result.ok());
    REQUIRE_EQ(
        result.value().disposition,
        ResidualDeviceDisposition::needs_attention_removal_unconfirmed);
    REQUIRE(!result.value().restart_safe);
}

TEST_CASE(residual_reconciliation_rejects_forged_removal_proof) {
    DeviceControl control;
    control.observations = {exact_observation()};
    control.forge_removal_proof = true;
    auto result = reconcile_residual_device(expected_identity(), control);
    REQUIRE(!result.ok());
    REQUIRE_EQ(control.removals, 1U);
}

TEST_CASE(residual_reconciliation_rejects_sentinel_device_identity) {
    auto expected = expected_identity();
    expected.owner.aim_device_id = aim::DeviceId{aim::kAllDevices};
    DeviceControl control;
    auto result = reconcile_residual_device(expected, control);
    REQUIRE(!result.ok());
    REQUIRE_EQ(control.enumerations, 0U);
    REQUIRE_EQ(control.removals, 0U);
}

} // namespace
