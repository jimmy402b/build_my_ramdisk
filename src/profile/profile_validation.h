#pragma once

#include "src/profile/profile_types.h"

#include <span>
#include <string_view>

namespace myrd::profile {

enum class ProfileValidationCode : std::uint16_t {
    ok = 0,
    revision_invalid,
    profile_uuid_invalid,
    image_uuid_invalid,
    owner_sid_invalid,
    display_name_invalid,
    display_name_conflict,
    creation_time_invalid,
    bundle_path_invalid,
    manifest_name_invalid,
    capacity_invalid,
    sector_geometry_invalid,
    device_identity_invalid,
    mount_target_invalid,
    memory_alignment_invalid,
    memory_mode_invalid,
    memory_order_invalid,
    memory_bound_invalid,
    snapshot_cow_invalid,
    checkpoint_interval_invalid,
    dirty_threshold_invalid,
    checkpoint_policy_invalid,
    compaction_policy_invalid,
    immutable_identity_changed,
    bundle_binding_changed,
    stopped_only_setting_changed,
    lifecycle_transition_invalid,
};

struct ProfileValidationResult final {
    ProfileValidationCode code{ProfileValidationCode::ok};
    std::string_view stable_code{"ok"};
    std::wstring_view public_message{L"Profile is valid"};

    [[nodiscard]] bool ok() const noexcept {
        return code == ProfileValidationCode::ok;
    }
};

[[nodiscard]] ProfileValidationResult validate_profile(
    const ProfileRecord& record) noexcept;

[[nodiscard]] ProfileValidationResult validate_profile_update(
    const ProfileRecord& current,
    const ProfileRecord& proposed,
    bool profile_is_running) noexcept;

// Dedicated stopped-profile rebind validation. Only the canonical bundle path
// and manifest component may change; every image/device/policy/owner field is
// retained and the revision advances exactly once.
[[nodiscard]] ProfileValidationResult validate_profile_rebind(
    const ProfileRecord& current,
    const ProfileRecord& proposed) noexcept;

[[nodiscard]] ProfileValidationResult validate_profile_name_unique(
    const Profile& candidate,
    std::span<const Profile> existing_profiles) noexcept;

} // namespace myrd::profile
