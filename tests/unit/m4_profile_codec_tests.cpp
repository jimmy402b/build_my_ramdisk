#include "test_framework.h"

#include "src/profile/profile_codec.h"
#include "src/profile/profile_types.h"
#include "src/profile/profile_validation.h"
#include "src/common/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

using namespace myrd::profile;

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

ProfileRecord minimal_record() {
    Profile profile{
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        L"S-1-5-18",
        L"A",
        1'700'000'000'000ULL,
        ProfileLifecycle::provisioning,
        L"D:\\B",
        "disk.rdm",
        uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
        8ULL << 30,
        512,
        4096,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        MountKind::drive_letter,
        L"R:",
        true,
        MemoryPolicy{MemoryMode::automatic, 1ULL << 30, 896ULL << 20,
                     768ULL << 20, 128ULL << 20},
        CheckpointPolicy{},
        CompactionPolicy{}};
    return ProfileRecord{7, std::move(profile)};
}

ProfileRecord ready_record() {
    auto record = minimal_record();
    record.profile.lifecycle = ProfileLifecycle::ready;
    record.profile.gpt_disk_guid =
        uuid("20000000-0000-0000-0000-000000000001");
    record.profile.partition_guid =
        uuid("20000000-0000-0000-0000-000000000002");
    record.profile.volume_guid =
        uuid("20000000-0000-0000-0000-000000000003");
    return record;
}

std::vector<std::byte> from_hex(std::string_view text) {
    REQUIRE((text.size() % 2) == 0);
    std::vector<std::byte> bytes;
    bytes.reserve(text.size() / 2);
    auto nibble = [](const char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        REQUIRE(false);
        return 0;
    };
    for (std::size_t i = 0; i < text.size(); i += 2) {
        bytes.push_back(static_cast<std::byte>((nibble(text[i]) << 4)
                                              | nibble(text[i + 1])));
    }
    return bytes;
}

const std::vector<std::byte>& minimal_golden() {
    static const auto bytes = from_hex(
        "4d595244505246000100000048000000fa010000000000000700000000000000"
        "b20100002300000013752174f2491fb8802b3cecbc72e65969f4ce0c20a16f9d"
        "01cd160e85772579010001011000000000112233445566778899aabbccddeeff"
        "0200020108000000532d312d352d31380300020101000000410400050108000000"
        "0068e5cf8b0100000500030101000000000600020104000000443a5c4207000201"
        "080000006469736b2e72646d0800010110000000102132435465768798a9bacb"
        "dcedfe0f090005010800000000000000020000000a0004010400000000020000"
        "0b00040104000000001000000c000701000000000d000701000000000e000701"
        "000000000f00030101000000001000020102000000523a110006010100000001"
        "120003010100000000130005010800000000000040000000001400050108000000"
        "0000003800000000150005010800000000000030000000001600050108000000"
        "000000080000000017000601010000000118000501080000002c01000000000000"
        "1900060101000000001a0005010800000000000000000000001b00060101000000"
        "011c00030101000000011d00030101000000011e00030101000000001f000601"
        "010000000120000401040000004600000021000401040000003200000022000401"
        "040000004000000023000401040000000a000000");
    return bytes;
}

void put_u16(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void refresh_payload_hash(std::vector<std::byte>& bytes) {
    auto digest = myrd::sha256_digest(
        std::span<const std::byte>(bytes).subspan(kProfileHeaderBytes));
    REQUIRE(digest.ok());
    std::copy(digest.value().begin(), digest.value().end(), bytes.begin() + 40);
}

void require_decode_rejected(const std::vector<std::byte>& bytes) {
    auto decoded = decode_profile(bytes);
    REQUIRE(decoded.has_error());
}

} // namespace

TEST_CASE(profile_v1_layout_constants_are_frozen) {
    constexpr std::array<std::byte, 8> expected_magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'P'}, std::byte{'R'}, std::byte{'F'}, std::byte{0}};
    REQUIRE_EQ(kProfileMagic, expected_magic);
    REQUIRE_EQ(kProfileSchemaMajor, std::uint16_t{1});
    REQUIRE_EQ(kProfileSchemaMinor, std::uint16_t{0});
    REQUIRE_EQ(kProfileHeaderBytes, std::uint32_t{72});
    REQUIRE_EQ(kProfileFieldHeaderBytes, std::uint32_t{8});
    REQUIRE_EQ(kProfileKnownFieldCount, std::uint32_t{35});
}

TEST_CASE(profile_v1_canonical_field_tags_are_contiguous_and_ordered) {
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::profile_uuid), 1);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::owner_sid), 2);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::display_name), 3);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::created_unix_ms), 4);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::lifecycle), 5);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::bundle_path), 6);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::manifest_name), 7);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::image_uuid), 8);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::logical_capacity), 9);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::logical_sector_size), 10);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::physical_sector_size), 11);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::gpt_disk_guid), 12);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::partition_guid), 13);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::volume_guid), 14);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::mount_kind), 15);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::mount_target), 16);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::auto_start), 17);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::memory_mode), 18);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::memory_hard_limit), 19);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::memory_soft_limit), 20);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::memory_low_watermark), 21);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::snapshot_cow_limit), 22);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_interval_enabled), 23);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_interval_seconds), 24);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::dirty_threshold_enabled), 25);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::dirty_threshold_bytes), 26);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_on_memory_pressure), 27);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_on_shutdown), 28);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_on_suspend), 29);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::normal_stop_policy), 30);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::automatic_compaction), 31);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::journal_occupancy_percent), 32);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::reclaimable_percent), 33);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::checkpoint_count_threshold), 34);
    REQUIRE_EQ(static_cast<std::uint16_t>(ProfileFieldTag::index_headroom_percent), 35);
}

TEST_CASE(profile_v1_minimal_profile_matches_exact_golden_bytes) {
    auto encoded = encode_profile(minimal_record());
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value(), minimal_golden());
}

TEST_CASE(profile_v1_minimal_profile_decodes_and_reencodes_byte_for_byte) {
    auto decoded = decode_profile(minimal_golden());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), minimal_record());

    auto reencoded = encode_profile(decoded.value());
    REQUIRE(reencoded.ok());
    REQUIRE_EQ(reencoded.value(), minimal_golden());
}

TEST_CASE(profile_v1_round_trips_require_clean_normal_stop_policy) {
    auto profile = minimal_record();
    profile.profile.checkpoint.normal_stop = NormalStopPolicy::require_clean;

    auto encoded = encode_profile(profile);
    REQUIRE(encoded.ok());
    auto decoded = decode_profile(encoded.value());

    REQUIRE(decoded.ok());
    REQUIRE_EQ(
        decoded.value().profile.checkpoint.normal_stop,
        NormalStopPolicy::require_clean);
}

TEST_CASE(profile_validation_accepts_windows_gpt_volume_guid_equal_to_partition) {
    auto record = ready_record();
    record.profile.volume_guid = record.profile.partition_guid;

    REQUIRE(validate_profile(record).ok());

    record.profile.gpt_disk_guid = record.profile.partition_guid;
    REQUIRE_EQ(
        validate_profile(record).code,
        ProfileValidationCode::device_identity_invalid);
}

TEST_CASE(profile_validation_accepts_complete_provisioning_identity) {
    auto record = ready_record();
    record.profile.lifecycle = ProfileLifecycle::provisioning;

    REQUIRE(validate_profile(record).ok());

    record.profile.volume_guid.reset();
    REQUIRE_EQ(
        validate_profile(record).code,
        ProfileValidationCode::device_identity_invalid);
}

TEST_CASE(profile_v1_rejects_magic_schema_and_zero_revision) {
    auto bad_magic = minimal_golden();
    bad_magic[0] ^= std::byte{1};
    require_decode_rejected(bad_magic);

    auto bad_major = minimal_golden();
    put_u16(bad_major, 8, 2);
    require_decode_rejected(bad_major);

    auto bad_minor = minimal_golden();
    put_u16(bad_minor, 10, 1);
    require_decode_rejected(bad_minor);

    auto zero_revision = minimal_golden();
    put_u64(zero_revision, 24, 0);
    require_decode_rejected(zero_revision);
}

TEST_CASE(profile_v1_rejects_length_checksum_truncation_and_file_trailing_bytes) {
    auto wrong_file_length = minimal_golden();
    put_u64(wrong_file_length, 16, wrong_file_length.size() + 1);
    require_decode_rejected(wrong_file_length);

    auto wrong_payload_length = minimal_golden();
    put_u32(wrong_payload_length, 32, 1);
    require_decode_rejected(wrong_payload_length);

    auto bad_checksum = minimal_golden();
    bad_checksum.back() ^= std::byte{1};
    require_decode_rejected(bad_checksum);

    auto truncated = minimal_golden();
    truncated.pop_back();
    require_decode_rejected(truncated);

    auto trailing_file_byte = minimal_golden();
    trailing_file_byte.push_back(std::byte{0});
    require_decode_rejected(trailing_file_byte);
}

TEST_CASE(profile_v1_rejects_missing_duplicate_and_unknown_mandatory_fields) {
    constexpr std::size_t kLastFieldOffset = 494;

    auto missing = minimal_golden();
    put_u16(missing, kLastFieldOffset, 36);
    missing[kLastFieldOffset + 3] = std::byte{0};
    refresh_payload_hash(missing);
    require_decode_rejected(missing);

    auto duplicate = minimal_golden();
    put_u16(duplicate, kLastFieldOffset, 34);
    refresh_payload_hash(duplicate);
    require_decode_rejected(duplicate);

    auto unknown_mandatory = minimal_golden();
    put_u16(unknown_mandatory, kLastFieldOffset, 36);
    refresh_payload_hash(unknown_mandatory);
    require_decode_rejected(unknown_mandatory);
}

TEST_CASE(profile_v1_accepts_bounded_unknown_optional_field) {
    auto bytes = minimal_golden();
    put_u64(bytes, 16, bytes.size() + 11);
    put_u32(bytes, 32, 445);
    put_u32(bytes, 36, 36);
    bytes.push_back(std::byte{36});
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{99});
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{3});
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0xaa});
    bytes.push_back(std::byte{0xbb});
    bytes.push_back(std::byte{0xcc});
    refresh_payload_hash(bytes);

    auto decoded = decode_profile(bytes);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), minimal_record());
}

TEST_CASE(profile_v1_rejects_payload_trailing_bytes_and_invalid_primitive_values) {
    auto trailing_payload = minimal_golden();
    put_u64(trailing_payload, 16, trailing_payload.size() + 1);
    put_u32(trailing_payload, 32, 435);
    trailing_payload.push_back(std::byte{0});
    refresh_payload_hash(trailing_payload);
    require_decode_rejected(trailing_payload);

    auto invalid_bool = minimal_golden();
    constexpr std::size_t kAutoStartValueOffset = 289;
    invalid_bool[kAutoStartValueOffset] = std::byte{2};
    refresh_payload_hash(invalid_bool);
    require_decode_rejected(invalid_bool);

    auto invalid_field_length = minimal_golden();
    put_u32(invalid_field_length, 76, 17);
    refresh_payload_hash(invalid_field_length);
    require_decode_rejected(invalid_field_length);
}

TEST_CASE(profile_v1_encoder_rejects_out_of_domain_enumerations) {
    auto record = minimal_record();
    record.profile.lifecycle = static_cast<ProfileLifecycle>(255);
    REQUIRE(encode_profile(record).has_error());

    record = minimal_record();
    record.profile.mount_kind = static_cast<MountKind>(255);
    REQUIRE(encode_profile(record).has_error());

    record = minimal_record();
    record.profile.memory.mode = static_cast<MemoryMode>(255);
    REQUIRE(encode_profile(record).has_error());
}

TEST_CASE(profile_v1_encoder_rejects_zero_revision_and_embedded_manifest_nul) {
    auto record = minimal_record();
    record.revision = 0;
    REQUIRE(encode_profile(record).has_error());

    record = minimal_record();
    record.profile.manifest_name = std::string("bad\0name", 8);
    REQUIRE(encode_profile(record).has_error());
}

TEST_CASE(profile_validation_accepts_complete_provisioning_and_ready_profiles) {
    REQUIRE(validate_profile(minimal_record()).ok());
    REQUIRE(validate_profile(ready_record()).ok());
}

TEST_CASE(profile_validation_rejects_revision_uuid_owner_name_and_time_bounds) {
    auto record = minimal_record();
    record.revision = 0;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::revision_invalid);

    record = minimal_record();
    record.profile.profile_uuid = uuid("00000000-0000-0000-0000-000000000000");
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::profile_uuid_invalid);

    record = minimal_record();
    record.profile.image_uuid = uuid("00000000-0000-0000-0000-000000000000");
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::image_uuid_invalid);

    record = minimal_record();
    record.profile.owner_sid = L"S-1-not-a-sid";
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::owner_sid_invalid);

    record = minimal_record();
    record.profile.display_name.clear();
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::display_name_invalid);

    record = minimal_record();
    record.profile.display_name.assign(129, L'x');
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::display_name_invalid);

    record = minimal_record();
    record.profile.created_unix_ms = 0;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::creation_time_invalid);
}

TEST_CASE(profile_validation_rejects_bundle_manifest_geometry_and_identity_errors) {
    auto record = minimal_record();
    record.profile.bundle_path = L"d:/A/../B";
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::bundle_path_invalid);

    record = minimal_record();
    record.profile.manifest_name = "../disk.rdm";
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::manifest_name_invalid);

    record = minimal_record();
    record.profile.logical_capacity_bytes = 0;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::capacity_invalid);

    record = minimal_record();
    record.profile.logical_capacity_bytes += 1;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::capacity_invalid);

    record = minimal_record();
    record.profile.logical_sector_size = 4096;
    REQUIRE(validate_profile(record).ok());

    record.profile.logical_sector_size = 1024;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::sector_geometry_invalid);

    record = minimal_record();
    record.profile.physical_sector_size = 512;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::sector_geometry_invalid);

    record = minimal_record();
    record.profile.gpt_disk_guid = uuid("20000000-0000-0000-0000-000000000001");
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::device_identity_invalid);

    record = ready_record();
    record.profile.volume_guid.reset();
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::device_identity_invalid);
}

TEST_CASE(profile_validation_rejects_mount_and_memory_policy_bounds) {
    auto record = minimal_record();
    record.profile.mount_target = L"r:";
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::mount_target_invalid);

    record = minimal_record();
    record.profile.mount_kind = MountKind::directory;
    record.profile.mount_target = L"D:\\Mount";
    REQUIRE(validate_profile(record).ok());

    record.profile.mount_target = L"D:\\Mount\\..\\Other";
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::mount_target_invalid);

    record = minimal_record();
    record.profile.memory.mode = static_cast<MemoryMode>(255);
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::memory_mode_invalid);

    record = minimal_record();
    record.profile.memory.low_watermark_bytes += 1;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::memory_alignment_invalid);

    record = minimal_record();
    record.profile.memory.soft_limit_bytes = record.profile.memory.hard_limit_bytes;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::memory_order_invalid);

    record = minimal_record();
    record.profile.memory.hard_limit_bytes = record.profile.logical_capacity_bytes +
                                             kProfileMemoryAlignmentBytes;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::memory_bound_invalid);

    record = minimal_record();
    record.profile.memory.snapshot_cow_limit_bytes = 0;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::snapshot_cow_invalid);
}

TEST_CASE(profile_validation_rejects_checkpoint_and_compaction_policy_bounds) {
    auto record = minimal_record();
    record.profile.checkpoint.on_shutdown = static_cast<BestEffortPolicy>(255);
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::checkpoint_policy_invalid);

    record = minimal_record();
    record.profile.checkpoint.interval_seconds = 29;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::checkpoint_interval_invalid);

    record = minimal_record();
    record.profile.checkpoint.interval_seconds = 7ULL * 24 * 60 * 60 + 1;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::checkpoint_interval_invalid);

    record = minimal_record();
    record.profile.checkpoint.interval_enabled = false;
    record.profile.checkpoint.interval_seconds = 1;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::checkpoint_interval_invalid);

    record = minimal_record();
    record.profile.checkpoint.dirty_threshold_enabled = true;
    record.profile.checkpoint.dirty_threshold_bytes = 1;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::dirty_threshold_invalid);

    record = minimal_record();
    record.profile.checkpoint.dirty_threshold_bytes = kProfileMemoryAlignmentBytes;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::dirty_threshold_invalid);

    record = minimal_record();
    record.profile.compaction.journal_occupancy_percent = 49;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::compaction_policy_invalid);
    record = minimal_record();
    record.profile.compaction.reclaimable_percent = 91;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::compaction_policy_invalid);
    record = minimal_record();
    record.profile.compaction.checkpoint_count_threshold = 4097;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::compaction_policy_invalid);
    record = minimal_record();
    record.profile.compaction.index_headroom_percent = 0;
    REQUIRE_EQ(validate_profile(record).code, ProfileValidationCode::compaction_policy_invalid);
}

TEST_CASE(profile_validation_enforces_immutable_and_stopped_only_updates) {
    const auto current = ready_record();
    auto proposed = current;
    ++proposed.revision;
    proposed.profile.display_name = L"Renamed";
    proposed.profile.checkpoint.interval_seconds = 600;
    REQUIRE(validate_profile_update(current, proposed, true).ok());

    proposed = current;
    ++proposed.revision;
    proposed.profile.owner_sid = L"S-1-5-19";
    REQUIRE_EQ(validate_profile_update(current, proposed, false).code,
               ProfileValidationCode::immutable_identity_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.profile_uuid =
        uuid("40000000-0000-0000-0000-000000000001");
    REQUIRE_EQ(validate_profile_update(current, proposed, false).code,
               ProfileValidationCode::immutable_identity_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.image_uuid =
        uuid("40000000-0000-0000-0000-000000000002");
    REQUIRE_EQ(validate_profile_update(current, proposed, false).code,
               ProfileValidationCode::immutable_identity_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.manifest_name = "other.rdm";
    REQUIRE_EQ(validate_profile_update(current, proposed, false).code,
               ProfileValidationCode::immutable_identity_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.bundle_path = L"D:\\Other";
    REQUIRE_EQ(validate_profile_update(current, proposed, false).code,
               ProfileValidationCode::bundle_binding_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.memory.hard_limit_bytes -= 64ULL << 20;
    proposed.profile.memory.soft_limit_bytes -= 64ULL << 20;
    proposed.profile.memory.low_watermark_bytes -= 64ULL << 20;
    REQUIRE_EQ(validate_profile_update(current, proposed, true).code,
               ProfileValidationCode::stopped_only_setting_changed);

    proposed = current;
    ++proposed.revision;
    proposed.profile.mount_target = L"S:";
    REQUIRE_EQ(validate_profile_update(current, proposed, true).code,
               ProfileValidationCode::stopped_only_setting_changed);
}

TEST_CASE(profile_validation_enforces_case_insensitive_machine_name_uniqueness) {
    auto candidate = minimal_record();
    auto existing_record = ready_record();
    existing_record.profile.profile_uuid =
        uuid("30000000-0000-0000-0000-000000000001");
    existing_record.profile.display_name = L"a";
    const std::array<Profile, 1> existing{existing_record.profile};
    REQUIRE_EQ(validate_profile_name_unique(candidate.profile, existing).code,
               ProfileValidationCode::display_name_conflict);

    existing_record.profile.display_name = L"Different";
    const std::array<Profile, 1> non_conflicting{existing_record.profile};
    REQUIRE(validate_profile_name_unique(candidate.profile, non_conflicting).ok());
}

TEST_CASE(profile_validation_does_not_touch_bundle_path_and_public_error_is_path_safe) {
    const std::wstring canary = L"D:\\MYRD_PATH_CANARY_7f1b\\missing";
    REQUIRE(!std::filesystem::exists(canary));

    auto valid = minimal_record();
    valid.profile.bundle_path = canary;
    REQUIRE(validate_profile(valid).ok());
    REQUIRE(!std::filesystem::exists(canary));

    auto invalid_path = minimal_record();
    invalid_path.profile.bundle_path = L"d:/MYRD_PATH_CANARY_7f1b/../escape";
    const auto result = validate_profile(invalid_path);
    REQUIRE_EQ(result.code, ProfileValidationCode::bundle_path_invalid);
    REQUIRE(result.stable_code.find("MYRD_PATH_CANARY_7f1b") ==
            std::string_view::npos);
    REQUIRE(result.public_message.find(L"MYRD_PATH_CANARY_7f1b") ==
            std::wstring_view::npos);
}
