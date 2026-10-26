#pragma once

#include "src/product/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace myrd::profile {

inline constexpr std::array<std::byte, 8> kProfileMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'P'}, std::byte{'R'}, std::byte{'F'}, std::byte{0}};
inline constexpr std::uint16_t kProfileSchemaMajor = 1;
inline constexpr std::uint16_t kProfileSchemaMinor = 0;
inline constexpr std::uint32_t kProfileHeaderBytes = 72;
inline constexpr std::uint32_t kProfileFieldHeaderBytes = 8;
inline constexpr std::uint32_t kProfileKnownFieldCount = 35;
inline constexpr std::uint32_t kProfileMaximumBytes = 256U * 1024U;
inline constexpr std::uint32_t kProfileMaximumFields = 128;
inline constexpr std::uint64_t kProfileMemoryAlignmentBytes = 64U * 1024U;

enum class ProfileFieldTag : std::uint16_t {
    profile_uuid = 1,
    owner_sid = 2,
    display_name = 3,
    created_unix_ms = 4,
    lifecycle = 5,
    bundle_path = 6,
    manifest_name = 7,
    image_uuid = 8,
    logical_capacity = 9,
    logical_sector_size = 10,
    physical_sector_size = 11,
    gpt_disk_guid = 12,
    partition_guid = 13,
    volume_guid = 14,
    mount_kind = 15,
    mount_target = 16,
    auto_start = 17,
    memory_mode = 18,
    memory_hard_limit = 19,
    memory_soft_limit = 20,
    memory_low_watermark = 21,
    snapshot_cow_limit = 22,
    checkpoint_interval_enabled = 23,
    checkpoint_interval_seconds = 24,
    dirty_threshold_enabled = 25,
    dirty_threshold_bytes = 26,
    checkpoint_on_memory_pressure = 27,
    checkpoint_on_shutdown = 28,
    checkpoint_on_suspend = 29,
    normal_stop_policy = 30,
    automatic_compaction = 31,
    journal_occupancy_percent = 32,
    reclaimable_percent = 33,
    checkpoint_count_threshold = 34,
    index_headroom_percent = 35,
};

enum class ProfileLifecycle : std::uint8_t { provisioning = 0, ready = 1 };
enum class MountKind : std::uint8_t { drive_letter = 0, directory = 1 };
enum class MemoryMode : std::uint8_t { automatic = 0, fixed = 1 };
enum class BestEffortPolicy : std::uint8_t { disabled = 0, best_effort = 1 };
enum class NormalStopPolicy : std::uint8_t {
    checkpoint_required = 0,
    require_clean = 1,
};

struct MemoryPolicy final {
    MemoryMode mode{MemoryMode::automatic};
    std::uint64_t hard_limit_bytes{};
    std::uint64_t soft_limit_bytes{};
    std::uint64_t low_watermark_bytes{};
    std::uint64_t snapshot_cow_limit_bytes{};
    bool operator==(const MemoryPolicy&) const noexcept = default;
};

// **The defaults a profile gets when nobody says otherwise.**
//
// Tightened in T1-10, and the reasoning is the product's own promise: a crash may lose at
// most one interval of writes.  A five-minute interval makes that promise true but useless --
// five minutes of a ramdisk's contents is most of a working session -- so the interval is a
// minute.  The byte threshold is the other half: a disk that absorbs 300 MB in ten seconds
// should not wait out the remaining fifty, so a dirty budget of 64 MiB commits early.  Both
// are policy, not mechanism: a profile may widen or disable either and the promise then
// belongs to whoever configured it.
struct CheckpointPolicy final {
    bool interval_enabled{true};
    std::uint64_t interval_seconds{60};
    bool dirty_threshold_enabled{true};
    std::uint64_t dirty_threshold_bytes{64ULL * 1024ULL * 1024ULL};
    bool on_memory_pressure{true};
    BestEffortPolicy on_shutdown{BestEffortPolicy::best_effort};
    BestEffortPolicy on_suspend{BestEffortPolicy::best_effort};
    NormalStopPolicy normal_stop{NormalStopPolicy::checkpoint_required};
    bool operator==(const CheckpointPolicy&) const noexcept = default;
};

struct CompactionPolicy final {
    bool automatic{true};
    std::uint32_t journal_occupancy_percent{70};
    std::uint32_t reclaimable_percent{50};
    std::uint32_t checkpoint_count_threshold{64};
    std::uint32_t index_headroom_percent{10};
    bool operator==(const CompactionPolicy&) const noexcept = default;
};

struct Profile final {
    product::Uuid profile_uuid;
    std::wstring owner_sid;
    std::wstring display_name;
    std::uint64_t created_unix_ms{};
    ProfileLifecycle lifecycle{ProfileLifecycle::provisioning};
    std::wstring bundle_path;
    std::string manifest_name;
    product::Uuid image_uuid;
    std::uint64_t logical_capacity_bytes{};
    std::uint32_t logical_sector_size{};
    std::uint32_t physical_sector_size{};
    std::optional<product::Uuid> gpt_disk_guid;
    std::optional<product::Uuid> partition_guid;
    std::optional<product::Uuid> volume_guid;
    MountKind mount_kind{MountKind::drive_letter};
    std::wstring mount_target;
    bool auto_start{true};
    MemoryPolicy memory{};
    CheckpointPolicy checkpoint{};
    CompactionPolicy compaction{};
    bool operator==(const Profile&) const noexcept = default;
};

struct ProfileRecord final {
    std::uint64_t revision{};
    Profile profile;
    bool operator==(const ProfileRecord&) const noexcept = default;
};

} // namespace myrd::profile
