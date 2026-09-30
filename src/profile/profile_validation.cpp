#include "src/profile/profile_validation.h"

#include "src/product/quantity_parser.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace myrd::profile {
namespace {

ProfileValidationResult valid() noexcept {
    return {};
}

ProfileValidationResult invalid(ProfileValidationCode code,
                                std::string_view stable_code,
                                std::wstring_view message) noexcept {
    return {code, stable_code, message};
}

bool uuid_is_nonzero(const product::Uuid& uuid) noexcept {
    return std::any_of(uuid.bytes().begin(), uuid.bytes().end(),
                       [](std::byte value) { return value != std::byte{0}; });
}

bool ordinal_equal_ignore_case(std::wstring_view left,
                               std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()), right.data(),
               static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

struct LocalMemoryDeleter final {
    void operator()(void* memory) const noexcept {
        if (memory != nullptr) {
            LocalFree(memory);
        }
    }
};

bool valid_owner_sid(std::wstring_view text) noexcept {
    if (text.empty() || text.size() > 256 || text.find(L'\0') != text.npos) {
        return false;
    }
    try {
        std::wstring terminated(text);
        PSID sid = nullptr;
        if (!ConvertStringSidToSidW(terminated.c_str(), &sid) || sid == nullptr) {
            return false;
        }
        std::unique_ptr<void, LocalMemoryDeleter> sid_guard(sid);
        const bool structurally_valid = IsValidSid(sid_guard.get()) != FALSE;
        LPWSTR canonical = nullptr;
        const bool converted = structurally_valid &&
            ConvertSidToStringSidW(sid_guard.get(), &canonical) != FALSE &&
            canonical != nullptr;
        std::unique_ptr<void, LocalMemoryDeleter> canonical_guard(canonical);
        const bool exact = converted && terminated == canonical;
        return exact;
    } catch (...) {
        return false;
    }
}

bool valid_display_name(std::wstring_view text) noexcept {
    if (text.empty() || text.size() > 128 || text.front() == L' ' ||
        text.back() == L' ') {
        return false;
    }
    return std::none_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch < L' ' || ch == L'\x7f';
    });
}

bool canonical_local_path(std::wstring_view text) noexcept {
    try {
        auto normalized = product::normalize_local_absolute_path(text);
        return normalized.ok() && normalized.value() == text;
    } catch (...) {
        return false;
    }
}

bool valid_manifest_name(std::string_view text) noexcept {
    if (text.empty() || text.size() > 255 ||
        text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int wide_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (wide_count <= 0) {
        return false;
    }
    try {
        std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                static_cast<int>(text.size()), wide.data(), wide_count) !=
            wide_count) {
            return false;
        }
        if (wide == L"." || wide == L".." || wide.back() == L'.' ||
            wide.back() == L' ') {
            return false;
        }
        constexpr std::wstring_view forbidden = L"<>:\"/\\|?*~";
        for (const auto ch : wide) {
            if (ch < L' ' || forbidden.find(ch) != forbidden.npos) {
                return false;
            }
        }
        const auto dot = wide.find(L'.');
        const auto base = wide.substr(0, dot);
        constexpr std::array<std::wstring_view, 8> reserved{
            L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$",
            L"CLOCK$", L"CONFIG$"};
        for (const auto item : reserved) {
            if (ordinal_equal_ignore_case(base, item)) {
                return false;
            }
        }
        if (base.size() == 4 &&
            (ordinal_equal_ignore_case(base.substr(0, 3), L"COM") ||
             ordinal_equal_ignore_case(base.substr(0, 3), L"LPT")) &&
            base[3] >= L'1' && base[3] <= L'9') {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool valid_mount_target(const Profile& profile) noexcept {
    switch (profile.mount_kind) {
    case MountKind::drive_letter:
        return profile.mount_target.size() == 2 &&
               profile.mount_target[0] >= L'A' &&
               profile.mount_target[0] <= L'Z' &&
               profile.mount_target[1] == L':';
    case MountKind::directory:
        return canonical_local_path(profile.mount_target);
    }
    return false;
}

bool optional_uuid_valid(const std::optional<product::Uuid>& value) noexcept {
    return !value || uuid_is_nonzero(*value);
}

bool identities_equal(const std::optional<product::Uuid>& left,
                      const std::optional<product::Uuid>& right) noexcept {
    return left == right;
}

} // namespace

ProfileValidationResult validate_profile(const ProfileRecord& record) noexcept {
    const auto& profile = record.profile;
    if (record.revision == 0) {
        return invalid(ProfileValidationCode::revision_invalid,
                       "profile.revision_invalid",
                       L"Profile revision must be nonzero");
    }
    if (!uuid_is_nonzero(profile.profile_uuid)) {
        return invalid(ProfileValidationCode::profile_uuid_invalid,
                       "profile.profile_uuid_invalid",
                       L"Profile identifier is invalid");
    }
    if (!uuid_is_nonzero(profile.image_uuid)) {
        return invalid(ProfileValidationCode::image_uuid_invalid,
                       "profile.image_uuid_invalid",
                       L"Image identifier is invalid");
    }
    if (!valid_owner_sid(profile.owner_sid)) {
        return invalid(ProfileValidationCode::owner_sid_invalid,
                       "profile.owner_sid_invalid",
                       L"Owner identity is not a canonical Windows SID");
    }
    if (!valid_display_name(profile.display_name)) {
        return invalid(ProfileValidationCode::display_name_invalid,
                       "profile.display_name_invalid",
                       L"Profile display name is invalid");
    }
    if (profile.created_unix_ms == 0) {
        return invalid(ProfileValidationCode::creation_time_invalid,
                       "profile.creation_time_invalid",
                       L"Profile creation time is invalid");
    }
    if (!canonical_local_path(profile.bundle_path)) {
        return invalid(ProfileValidationCode::bundle_path_invalid,
                       "profile.bundle_path_invalid",
                       L"Bundle path is not a canonical local path");
    }
    if (!valid_manifest_name(profile.manifest_name)) {
        return invalid(ProfileValidationCode::manifest_name_invalid,
                       "profile.manifest_name_invalid",
                       L"Manifest component name is invalid");
    }
    if (profile.logical_capacity_bytes == 0 ||
        profile.logical_sector_size == 0 ||
        profile.logical_capacity_bytes % profile.logical_sector_size != 0) {
        return invalid(ProfileValidationCode::capacity_invalid,
                       "profile.capacity_invalid",
                       L"Logical capacity is invalid");
    }
    if ((profile.logical_sector_size != 512 &&
         profile.logical_sector_size != 4096) ||
        profile.physical_sector_size != 4096) {
        return invalid(ProfileValidationCode::sector_geometry_invalid,
                       "profile.sector_geometry_invalid",
                       L"Sector geometry is unsupported");
    }
    if (!optional_uuid_valid(profile.gpt_disk_guid) ||
        !optional_uuid_valid(profile.partition_guid) ||
        !optional_uuid_valid(profile.volume_guid)) {
        return invalid(ProfileValidationCode::device_identity_invalid,
                       "profile.device_identity_invalid",
                       L"Expected device identity is invalid");
    }
    const bool any_identity = profile.gpt_disk_guid || profile.partition_guid ||
                              profile.volume_guid;
    const bool all_identities = profile.gpt_disk_guid && profile.partition_guid &&
                                profile.volume_guid;
    // Provisioning records gain the complete Windows identity before the first
    // checkpoint makes them ready.  That identity is the recovery anchor for a
    // crash in the checkpoint window; only a partial identity is invalid.
    if ((any_identity && !all_identities) ||
        (profile.lifecycle == ProfileLifecycle::ready && !all_identities) ||
        (all_identities &&
         (*profile.gpt_disk_guid == *profile.partition_guid ||
          *profile.gpt_disk_guid == *profile.volume_guid))) {
        return invalid(ProfileValidationCode::device_identity_invalid,
                       "profile.device_identity_invalid",
                       L"Expected device identities do not match lifecycle requirements");
    }
    if (profile.lifecycle != ProfileLifecycle::provisioning &&
        profile.lifecycle != ProfileLifecycle::ready) {
        return invalid(ProfileValidationCode::lifecycle_transition_invalid,
                       "profile.lifecycle_invalid",
                       L"Profile lifecycle value is invalid");
    }
    if (!valid_mount_target(profile)) {
        return invalid(ProfileValidationCode::mount_target_invalid,
                       "profile.mount_target_invalid",
                       L"Mount target is not canonical");
    }

    const auto& memory = profile.memory;
    if (memory.mode != MemoryMode::automatic && memory.mode != MemoryMode::fixed) {
        return invalid(ProfileValidationCode::memory_mode_invalid,
                       "profile.memory_mode_invalid",
                       L"Memory mode is invalid");
    }
    const auto aligned = [](std::uint64_t value) {
        return value != 0 && value % kProfileMemoryAlignmentBytes == 0;
    };
    if (!aligned(memory.low_watermark_bytes) ||
        !aligned(memory.soft_limit_bytes) ||
        !aligned(memory.hard_limit_bytes)) {
        return invalid(ProfileValidationCode::memory_alignment_invalid,
                       "profile.memory_alignment_invalid",
                       L"Memory limits must be nonzero and block aligned");
    }
    if (!(memory.low_watermark_bytes < memory.soft_limit_bytes &&
          memory.soft_limit_bytes < memory.hard_limit_bytes)) {
        return invalid(ProfileValidationCode::memory_order_invalid,
                       "profile.memory_order_invalid",
                       L"Memory low, soft and hard limits are not strictly ordered");
    }
    if (memory.hard_limit_bytes > profile.logical_capacity_bytes) {
        return invalid(ProfileValidationCode::memory_bound_invalid,
                       "profile.memory_bound_invalid",
                       L"Memory hard limit exceeds logical capacity");
    }
    if (!aligned(memory.snapshot_cow_limit_bytes) ||
        memory.snapshot_cow_limit_bytes > profile.logical_capacity_bytes) {
        return invalid(ProfileValidationCode::snapshot_cow_invalid,
                       "profile.snapshot_cow_invalid",
                       L"Snapshot COW limit is outside its safe bound");
    }

    const auto& checkpoint = profile.checkpoint;
    if ((checkpoint.on_shutdown != BestEffortPolicy::disabled &&
         checkpoint.on_shutdown != BestEffortPolicy::best_effort) ||
        (checkpoint.on_suspend != BestEffortPolicy::disabled &&
         checkpoint.on_suspend != BestEffortPolicy::best_effort) ||
        (checkpoint.normal_stop != NormalStopPolicy::checkpoint_required
         && checkpoint.normal_stop != NormalStopPolicy::require_clean)) {
        return invalid(ProfileValidationCode::checkpoint_policy_invalid,
                       "profile.checkpoint_policy_invalid",
                       L"Checkpoint lifecycle policy is invalid");
    }
    constexpr std::uint64_t kMaximumIntervalSeconds = 7ULL * 24 * 60 * 60;
    if ((checkpoint.interval_enabled &&
         (checkpoint.interval_seconds < 30 ||
          checkpoint.interval_seconds > kMaximumIntervalSeconds)) ||
        (!checkpoint.interval_enabled && checkpoint.interval_seconds != 0)) {
        return invalid(ProfileValidationCode::checkpoint_interval_invalid,
                       "profile.checkpoint_interval_invalid",
                       L"Checkpoint interval is outside its configured bound");
    }
    if ((checkpoint.dirty_threshold_enabled &&
         (checkpoint.dirty_threshold_bytes < kProfileMemoryAlignmentBytes ||
          checkpoint.dirty_threshold_bytes > profile.logical_capacity_bytes ||
          checkpoint.dirty_threshold_bytes % kProfileMemoryAlignmentBytes != 0)) ||
        (!checkpoint.dirty_threshold_enabled &&
         checkpoint.dirty_threshold_bytes != 0)) {
        return invalid(ProfileValidationCode::dirty_threshold_invalid,
                       "profile.dirty_threshold_invalid",
                       L"Dirty threshold is outside its configured bound");
    }

    const auto& compaction = profile.compaction;
    if (compaction.journal_occupancy_percent < 50 ||
        compaction.journal_occupancy_percent > 95 ||
        compaction.reclaimable_percent < 10 ||
        compaction.reclaimable_percent > 90 ||
        compaction.checkpoint_count_threshold < 1 ||
        compaction.checkpoint_count_threshold > 4096 ||
        compaction.index_headroom_percent < 1 ||
        compaction.index_headroom_percent > 25) {
        return invalid(ProfileValidationCode::compaction_policy_invalid,
                       "profile.compaction_policy_invalid",
                       L"Compaction policy is outside its configured bound");
    }
    return valid();
}

ProfileValidationResult validate_profile_update(
    const ProfileRecord& current, const ProfileRecord& proposed,
    bool profile_is_running) noexcept {
    auto validation = validate_profile(proposed);
    if (!validation.ok()) {
        return validation;
    }
    if (proposed.revision <= current.revision) {
        return invalid(ProfileValidationCode::revision_invalid,
                       "profile.revision_invalid",
                       L"Proposed profile revision must increase");
    }
    if (proposed.profile.profile_uuid != current.profile.profile_uuid ||
        proposed.profile.image_uuid != current.profile.image_uuid ||
        proposed.profile.owner_sid != current.profile.owner_sid ||
        proposed.profile.manifest_name != current.profile.manifest_name ||
        proposed.profile.created_unix_ms != current.profile.created_unix_ms) {
        return invalid(ProfileValidationCode::immutable_identity_changed,
                       "profile.immutable_identity_changed",
                       L"An immutable profile identity field changed");
    }
    if (proposed.profile.bundle_path != current.profile.bundle_path) {
        return invalid(ProfileValidationCode::bundle_binding_changed,
                       "profile.bundle_binding_changed",
                       L"Bundle changes require the dedicated rebind operation");
    }
    if (current.profile.lifecycle == ProfileLifecycle::ready &&
        (proposed.profile.lifecycle != ProfileLifecycle::ready ||
         !identities_equal(proposed.profile.gpt_disk_guid,
                           current.profile.gpt_disk_guid) ||
         !identities_equal(proposed.profile.partition_guid,
                           current.profile.partition_guid) ||
         !identities_equal(proposed.profile.volume_guid,
                           current.profile.volume_guid))) {
        return invalid(ProfileValidationCode::immutable_identity_changed,
                       "profile.immutable_identity_changed",
                       L"Provisioned device identity is immutable");
    }
    if (current.profile.lifecycle == ProfileLifecycle::provisioning &&
        proposed.profile.lifecycle != ProfileLifecycle::provisioning &&
        proposed.profile.lifecycle != ProfileLifecycle::ready) {
        return invalid(ProfileValidationCode::lifecycle_transition_invalid,
                       "profile.lifecycle_transition_invalid",
                       L"Profile lifecycle transition is invalid");
    }
    if (profile_is_running &&
        (proposed.profile.memory != current.profile.memory ||
         proposed.profile.mount_kind != current.profile.mount_kind ||
         proposed.profile.mount_target != current.profile.mount_target)) {
        return invalid(ProfileValidationCode::stopped_only_setting_changed,
                       "profile.stopped_only_setting_changed",
                       L"This setting can change only while the disk is stopped");
    }
    return valid();
}

ProfileValidationResult validate_profile_rebind(
    const ProfileRecord& current,
    const ProfileRecord& proposed) noexcept {
    auto validation = validate_profile(proposed);
    if (!validation.ok()) return validation;
    if (current.revision == (std::numeric_limits<std::uint64_t>::max)()
        || proposed.revision != current.revision + 1) {
        return invalid(ProfileValidationCode::revision_invalid,
                       "profile.revision_invalid",
                       L"A rebind must advance the profile revision exactly once");
    }
    auto normalized = proposed.profile;
    normalized.bundle_path = current.profile.bundle_path;
    normalized.manifest_name = current.profile.manifest_name;
    if (normalized != current.profile) {
        return invalid(ProfileValidationCode::immutable_identity_changed,
                       "profile.immutable_identity_changed",
                       L"A rebind may change only the bundle binding");
    }
    return valid();
}

ProfileValidationResult validate_profile_name_unique(
    const Profile& candidate,
    std::span<const Profile> existing_profiles) noexcept {
    for (const auto& existing : existing_profiles) {
        if (existing.profile_uuid != candidate.profile_uuid &&
            ordinal_equal_ignore_case(existing.display_name,
                                      candidate.display_name)) {
            return invalid(ProfileValidationCode::display_name_conflict,
                           "profile.display_name_conflict",
                           L"Profile display name is already registered");
        }
    }
    return valid();
}

} // namespace myrd::profile
