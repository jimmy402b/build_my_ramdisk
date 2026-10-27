#include "src/profile/profile_codec.h"

#include "src/common/sha256.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace myrd::profile {
namespace {

constexpr std::size_t kHashOffset = 40;
constexpr std::size_t kHashBytes = 32;
constexpr std::uint16_t kLastKnownTag =
    static_cast<std::uint16_t>(ProfileFieldTag::volume_label);

template <class T>
Result<T> codec_failure(ProfileCodecError code, const wchar_t* operation,
                        const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::protocol, static_cast<std::uint32_t>(code), operation,
        message});
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1]))
         << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

Result<std::vector<std::byte>> encode_utf8(
    std::wstring_view text, std::size_t maximum_bytes) {
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"Profile text cannot be represented safely");
    }
    if (text.empty()) {
        return Result<std::vector<std::byte>>::success({});
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0 || static_cast<std::size_t>(required) > maximum_bytes) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"Profile text is invalid or exceeds its encoded bound");
    }
    std::vector<std::byte> encoded(static_cast<std::size_t>(required));
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(encoded.data()), required, nullptr, nullptr);
    if (written != required) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"Profile text encoding failed");
    }
    return Result<std::vector<std::byte>>::success(std::move(encoded));
}

bool contains_nul(std::span<const std::byte> bytes) noexcept {
    return std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end();
}

Result<std::wstring> decode_utf8(
    std::span<const std::byte> bytes, std::size_t maximum_bytes) {
    if (bytes.size() > maximum_bytes || contains_nul(bytes) ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return codec_failure<std::wstring>(
            ProfileCodecError::text_encoding, L"profile.decode",
            L"Profile text is invalid or exceeds its encoded bound");
    }
    if (bytes.empty()) {
        return Result<std::wstring>::success({});
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
        nullptr, 0);
    if (required <= 0) {
        return codec_failure<std::wstring>(
            ProfileCodecError::text_encoding, L"profile.decode",
            L"Profile text is not canonical UTF-8");
    }
    std::wstring decoded(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
        decoded.data(), required);
    if (written != required) {
        return codec_failure<std::wstring>(
            ProfileCodecError::text_encoding, L"profile.decode",
            L"Profile text decoding failed");
    }
    return Result<std::wstring>::success(std::move(decoded));
}

void append_field_header(std::vector<std::byte>& payload, ProfileFieldTag tag,
                         ProfileFieldKind kind, std::uint32_t length) {
    append_u16(payload, static_cast<std::uint16_t>(tag));
    payload.push_back(static_cast<std::byte>(kind));
    payload.push_back(static_cast<std::byte>(kProfileMandatoryField));
    append_u32(payload, length);
}

void append_bytes_field(std::vector<std::byte>& payload, ProfileFieldTag tag,
                        ProfileFieldKind kind,
                        std::span<const std::byte> bytes) {
    append_field_header(payload, tag, kind,
                        static_cast<std::uint32_t>(bytes.size()));
    payload.insert(payload.end(), bytes.begin(), bytes.end());
}

void append_u8_field(std::vector<std::byte>& payload, ProfileFieldTag tag,
                     ProfileFieldKind kind, std::uint8_t value) {
    append_field_header(payload, tag, kind, 1);
    payload.push_back(static_cast<std::byte>(value));
}

void append_u32_field(std::vector<std::byte>& payload, ProfileFieldTag tag,
                      std::uint32_t value) {
    append_field_header(payload, tag, ProfileFieldKind::u32, 4);
    append_u32(payload, value);
}

void append_u64_field(std::vector<std::byte>& payload, ProfileFieldTag tag,
                      std::uint64_t value) {
    append_field_header(payload, tag, ProfileFieldKind::u64, 8);
    append_u64(payload, value);
}

void append_uuid_field(std::vector<std::byte>& payload, ProfileFieldTag tag,
                       const product::Uuid& value) {
    append_bytes_field(payload, tag, ProfileFieldKind::uuid, value.bytes());
}

void append_optional_uuid_field(
    std::vector<std::byte>& payload, ProfileFieldTag tag,
    const std::optional<product::Uuid>& value) {
    if (value) {
        append_bytes_field(payload, tag, ProfileFieldKind::optional_uuid,
                           value->bytes());
    } else {
        append_bytes_field(payload, tag, ProfileFieldKind::optional_uuid, {});
    }
}

struct FieldView final {
    std::uint16_t tag{};
    ProfileFieldKind kind{};
    std::uint8_t flags{};
    std::span<const std::byte> value;
};

bool expected_kind_and_length(const FieldView& field) noexcept {
    const auto tag = static_cast<ProfileFieldTag>(field.tag);
    switch (tag) {
    case ProfileFieldTag::profile_uuid:
    case ProfileFieldTag::image_uuid:
        return field.kind == ProfileFieldKind::uuid && field.value.size() == 16;
    case ProfileFieldTag::owner_sid:
    case ProfileFieldTag::display_name:
    case ProfileFieldTag::bundle_path:
    case ProfileFieldTag::manifest_name:
    case ProfileFieldTag::mount_target:
    case ProfileFieldTag::volume_label:
        return field.kind == ProfileFieldKind::utf8;
    case ProfileFieldTag::created_unix_ms:
    case ProfileFieldTag::logical_capacity:
    case ProfileFieldTag::memory_hard_limit:
    case ProfileFieldTag::memory_soft_limit:
    case ProfileFieldTag::memory_low_watermark:
    case ProfileFieldTag::snapshot_cow_limit:
    case ProfileFieldTag::checkpoint_interval_seconds:
    case ProfileFieldTag::dirty_threshold_bytes:
        return field.kind == ProfileFieldKind::u64 && field.value.size() == 8;
    case ProfileFieldTag::lifecycle:
    case ProfileFieldTag::mount_kind:
    case ProfileFieldTag::memory_mode:
    case ProfileFieldTag::checkpoint_on_shutdown:
    case ProfileFieldTag::checkpoint_on_suspend:
    case ProfileFieldTag::normal_stop_policy:
        return field.kind == ProfileFieldKind::u8 && field.value.size() == 1;
    case ProfileFieldTag::logical_sector_size:
    case ProfileFieldTag::physical_sector_size:
    case ProfileFieldTag::journal_occupancy_percent:
    case ProfileFieldTag::reclaimable_percent:
    case ProfileFieldTag::checkpoint_count_threshold:
    case ProfileFieldTag::index_headroom_percent:
        return field.kind == ProfileFieldKind::u32 && field.value.size() == 4;
    case ProfileFieldTag::gpt_disk_guid:
    case ProfileFieldTag::partition_guid:
    case ProfileFieldTag::volume_guid:
        return field.kind == ProfileFieldKind::optional_uuid &&
               (field.value.empty() || field.value.size() == 16);
    case ProfileFieldTag::auto_start:
    case ProfileFieldTag::checkpoint_interval_enabled:
    case ProfileFieldTag::dirty_threshold_enabled:
    case ProfileFieldTag::checkpoint_on_memory_pressure:
    case ProfileFieldTag::automatic_compaction:
        return field.kind == ProfileFieldKind::boolean && field.value.size() == 1;
    }
    return false;
}

std::string uuid_text(std::span<const std::byte> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        const auto value = std::to_integer<std::uint8_t>(bytes[index]);
        text.push_back(kHex[value >> 4U]);
        text.push_back(kHex[value & 0x0fU]);
    }
    return text;
}

Result<product::Uuid> decode_uuid(std::span<const std::byte> bytes) {
    if (bytes.size() != 16) {
        return codec_failure<product::Uuid>(
            ProfileCodecError::invalid_fields, L"profile.decode",
            L"Profile UUID field has an invalid length");
    }
    auto parsed = product::Uuid::parse(uuid_text(bytes));
    if (!parsed.ok()) {
        return codec_failure<product::Uuid>(
            ProfileCodecError::invalid_fields, L"profile.decode",
            L"Profile UUID field is invalid");
    }
    return parsed;
}

Result<std::optional<product::Uuid>> decode_optional_uuid(
    std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return Result<std::optional<product::Uuid>>::success(std::nullopt);
    }
    auto parsed = decode_uuid(bytes);
    if (!parsed.ok()) {
        return Result<std::optional<product::Uuid>>::failure(parsed.error());
    }
    return Result<std::optional<product::Uuid>>::success(parsed.move_value());
}

bool valid_boolean(const FieldView& field) noexcept {
    return std::to_integer<std::uint8_t>(field.value[0]) <= 1;
}

} // namespace

Result<std::vector<std::byte>> encode_profile(const ProfileRecord& record) {
    if (record.revision == 0) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::invalid_revision, L"profile.encode",
            L"Profile revision must be nonzero");
    }
    if (static_cast<std::uint8_t>(record.profile.lifecycle) > 1 ||
        static_cast<std::uint8_t>(record.profile.mount_kind) > 1 ||
        static_cast<std::uint8_t>(record.profile.memory.mode) > 1 ||
        static_cast<std::uint8_t>(record.profile.checkpoint.on_shutdown) > 1 ||
        static_cast<std::uint8_t>(record.profile.checkpoint.on_suspend) > 1 ||
        static_cast<std::uint8_t>(record.profile.checkpoint.normal_stop) > 1) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::invalid_fields, L"profile.encode",
            L"Profile enumeration field has an invalid value");
    }

    auto owner = encode_utf8(record.profile.owner_sid, 256);
    auto name = encode_utf8(record.profile.display_name, 512);
    auto bundle = encode_utf8(record.profile.bundle_path, 32'768);
    auto mount = encode_utf8(record.profile.mount_target, 32'768);
    if (!owner.ok() || !name.ok() || !bundle.ok() || !mount.ok()) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"One or more profile text fields are invalid");
    }
    if (record.profile.manifest_name.size() > 255 ||
        contains_nul(std::as_bytes(std::span(record.profile.manifest_name)))) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"Manifest name is invalid or exceeds its encoded bound");
    }
    auto manifest_utf16 = decode_utf8(
        std::as_bytes(std::span(record.profile.manifest_name)), 255);
    if (!manifest_utf16.ok()) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::text_encoding, L"profile.encode",
            L"Manifest name is not canonical UTF-8");
    }

    std::vector<std::byte> payload;
    payload.reserve(512 + owner.value().size() + name.value().size() +
                    bundle.value().size() + mount.value().size() +
                    record.profile.manifest_name.size());
    const auto& value = record.profile;
    append_uuid_field(payload, ProfileFieldTag::profile_uuid, value.profile_uuid);
    append_bytes_field(payload, ProfileFieldTag::owner_sid, ProfileFieldKind::utf8,
                       owner.value());
    append_bytes_field(payload, ProfileFieldTag::display_name, ProfileFieldKind::utf8,
                       name.value());
    append_u64_field(payload, ProfileFieldTag::created_unix_ms, value.created_unix_ms);
    append_u8_field(payload, ProfileFieldTag::lifecycle, ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.lifecycle));
    append_bytes_field(payload, ProfileFieldTag::bundle_path, ProfileFieldKind::utf8,
                       bundle.value());
    append_bytes_field(payload, ProfileFieldTag::manifest_name, ProfileFieldKind::utf8,
                       std::as_bytes(std::span(value.manifest_name)));
    append_uuid_field(payload, ProfileFieldTag::image_uuid, value.image_uuid);
    append_u64_field(payload, ProfileFieldTag::logical_capacity,
                     value.logical_capacity_bytes);
    append_u32_field(payload, ProfileFieldTag::logical_sector_size,
                     value.logical_sector_size);
    append_u32_field(payload, ProfileFieldTag::physical_sector_size,
                     value.physical_sector_size);
    append_optional_uuid_field(payload, ProfileFieldTag::gpt_disk_guid,
                               value.gpt_disk_guid);
    append_optional_uuid_field(payload, ProfileFieldTag::partition_guid,
                               value.partition_guid);
    append_optional_uuid_field(payload, ProfileFieldTag::volume_guid,
                               value.volume_guid);
    append_u8_field(payload, ProfileFieldTag::mount_kind, ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.mount_kind));
    append_bytes_field(payload, ProfileFieldTag::mount_target, ProfileFieldKind::utf8,
                       mount.value());
    append_u8_field(payload, ProfileFieldTag::auto_start, ProfileFieldKind::boolean,
                    static_cast<std::uint8_t>(value.auto_start));
    append_u8_field(payload, ProfileFieldTag::memory_mode, ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.memory.mode));
    append_u64_field(payload, ProfileFieldTag::memory_hard_limit,
                     value.memory.hard_limit_bytes);
    append_u64_field(payload, ProfileFieldTag::memory_soft_limit,
                     value.memory.soft_limit_bytes);
    append_u64_field(payload, ProfileFieldTag::memory_low_watermark,
                     value.memory.low_watermark_bytes);
    append_u64_field(payload, ProfileFieldTag::snapshot_cow_limit,
                     value.memory.snapshot_cow_limit_bytes);
    append_u8_field(payload, ProfileFieldTag::checkpoint_interval_enabled,
                    ProfileFieldKind::boolean,
                    static_cast<std::uint8_t>(value.checkpoint.interval_enabled));
    append_u64_field(payload, ProfileFieldTag::checkpoint_interval_seconds,
                     value.checkpoint.interval_seconds);
    append_u8_field(payload, ProfileFieldTag::dirty_threshold_enabled,
                    ProfileFieldKind::boolean,
                    static_cast<std::uint8_t>(value.checkpoint.dirty_threshold_enabled));
    append_u64_field(payload, ProfileFieldTag::dirty_threshold_bytes,
                     value.checkpoint.dirty_threshold_bytes);
    append_u8_field(payload, ProfileFieldTag::checkpoint_on_memory_pressure,
                    ProfileFieldKind::boolean,
                    static_cast<std::uint8_t>(value.checkpoint.on_memory_pressure));
    append_u8_field(payload, ProfileFieldTag::checkpoint_on_shutdown,
                    ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.checkpoint.on_shutdown));
    append_u8_field(payload, ProfileFieldTag::checkpoint_on_suspend,
                    ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.checkpoint.on_suspend));
    append_u8_field(payload, ProfileFieldTag::normal_stop_policy,
                    ProfileFieldKind::u8,
                    static_cast<std::uint8_t>(value.checkpoint.normal_stop));
    append_u8_field(payload, ProfileFieldTag::automatic_compaction,
                    ProfileFieldKind::boolean,
                    static_cast<std::uint8_t>(value.compaction.automatic));
    append_u32_field(payload, ProfileFieldTag::journal_occupancy_percent,
                     value.compaction.journal_occupancy_percent);
    append_u32_field(payload, ProfileFieldTag::reclaimable_percent,
                     value.compaction.reclaimable_percent);
    append_u32_field(payload, ProfileFieldTag::checkpoint_count_threshold,
                     value.compaction.checkpoint_count_threshold);
    append_u32_field(payload, ProfileFieldTag::index_headroom_percent,
                     value.compaction.index_headroom_percent);
    // Tag 36, last because fields are ordered by tag.  Mandatory like every other field here -- the decoder
    // requires each tag up to kLastKnownTag and the header declares how many are known -- and the field codec
    // refuses a zero-length value, so "no label" travels as a sentinel no real label can contain.
    auto label = encode_utf8(value.volume_label.value_or(std::wstring(1, L'\u0001')), 64);
    if (!label.ok()) return Result<std::vector<std::byte>>::failure(label.error());
    append_bytes_field(payload, ProfileFieldTag::volume_label, ProfileFieldKind::utf8,
                       label.value());

    if (payload.size() > kProfileMaximumBytes - kProfileHeaderBytes) {
        return codec_failure<std::vector<std::byte>>(
            ProfileCodecError::invalid_size, L"profile.encode",
            L"Encoded profile exceeds the format bound");
    }
    auto hash = sha256_digest(payload);
    if (!hash.ok()) {
        return Result<std::vector<std::byte>>::failure(hash.error());
    }

    std::vector<std::byte> output;
    output.reserve(kProfileHeaderBytes + payload.size());
    output.insert(output.end(), kProfileMagic.begin(), kProfileMagic.end());
    append_u16(output, kProfileSchemaMajor);
    append_u16(output, kProfileSchemaMinor);
    append_u32(output, kProfileHeaderBytes);
    append_u64(output, kProfileHeaderBytes + payload.size());
    append_u64(output, record.revision);
    append_u32(output, static_cast<std::uint32_t>(payload.size()));
    append_u32(output, kProfileKnownFieldCount);
    output.insert(output.end(), hash.value().begin(), hash.value().end());
    output.insert(output.end(), payload.begin(), payload.end());
    return Result<std::vector<std::byte>>::success(std::move(output));
}

Result<ProfileRecord> decode_profile(std::span<const std::byte> bytes) {
    if (bytes.size() < kProfileHeaderBytes || bytes.size() > kProfileMaximumBytes) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_size, L"profile.decode",
            L"Profile file length is outside the format bound");
    }
    if (!std::equal(kProfileMagic.begin(), kProfileMagic.end(), bytes.begin())) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_magic, L"profile.decode",
            L"Profile magic is invalid");
    }
    if (read_u16(bytes, 8) != kProfileSchemaMajor ||
        read_u16(bytes, 10) != kProfileSchemaMinor) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::unsupported_schema, L"profile.decode",
            L"Profile schema version is unsupported");
    }
    const auto header_bytes = read_u32(bytes, 12);
    const auto file_bytes = read_u64(bytes, 16);
    const auto revision = read_u64(bytes, 24);
    const auto payload_bytes = read_u32(bytes, 32);
    const auto field_count = read_u32(bytes, 36);
    if (header_bytes != kProfileHeaderBytes || file_bytes != bytes.size() ||
        payload_bytes != bytes.size() - kProfileHeaderBytes ||
        field_count < kProfileKnownFieldCount ||
        field_count > kProfileMaximumFields) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_header, L"profile.decode",
            L"Profile header lengths or field count are inconsistent");
    }
    if (revision == 0) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_revision, L"profile.decode",
            L"Profile revision must be nonzero");
    }
    const auto payload = bytes.subspan(kProfileHeaderBytes);
    auto hash = sha256_digest(payload);
    if (!hash.ok()) {
        return Result<ProfileRecord>::failure(hash.error());
    }
    if (!std::equal(hash.value().begin(), hash.value().end(),
                    bytes.begin() + kHashOffset)) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::checksum_mismatch, L"profile.decode",
            L"Profile payload checksum does not match");
    }

    std::array<FieldView, kProfileMaximumFields> fields{};
    std::array<const FieldView*, kLastKnownTag + 1> known{};
    std::size_t cursor = 0;
    std::uint16_t previous_tag = 0;
    for (std::uint32_t index = 0; index < field_count; ++index) {
        if (payload.size() - cursor < kProfileFieldHeaderBytes) {
            return codec_failure<ProfileRecord>(
                ProfileCodecError::invalid_fields, L"profile.decode",
                L"Profile field header is truncated");
        }
        const auto tag = read_u16(payload, cursor);
        const auto kind = static_cast<ProfileFieldKind>(
            std::to_integer<std::uint8_t>(payload[cursor + 2]));
        const auto flags = std::to_integer<std::uint8_t>(payload[cursor + 3]);
        const auto length = read_u32(payload, cursor + 4);
        cursor += kProfileFieldHeaderBytes;
        if (tag == 0 || tag <= previous_tag || flags > kProfileMandatoryField ||
            length > payload.size() - cursor) {
            return codec_failure<ProfileRecord>(
                ProfileCodecError::invalid_fields, L"profile.decode",
                L"Profile fields are duplicate, unordered, or truncated");
        }
        previous_tag = tag;
        fields[index] = FieldView{
            tag, kind, flags, payload.subspan(cursor, length)};
        cursor += length;

        if (tag <= kLastKnownTag) {
            if (flags != kProfileMandatoryField ||
                !expected_kind_and_length(fields[index])) {
                return codec_failure<ProfileRecord>(
                    ProfileCodecError::invalid_fields, L"profile.decode",
                    L"Known profile field has an invalid contract");
            }
            known[tag] = &fields[index];
        } else if ((flags & kProfileMandatoryField) != 0) {
            return codec_failure<ProfileRecord>(
                ProfileCodecError::unsupported_schema, L"profile.decode",
                L"Profile contains an unknown mandatory field");
        }
    }
    if (cursor != payload.size()) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_fields, L"profile.decode",
            L"Profile contains trailing payload bytes");
    }
    for (std::uint16_t tag = 1; tag <= kLastKnownTag; ++tag) {
        if (known[tag] == nullptr) {
            return codec_failure<ProfileRecord>(
                ProfileCodecError::invalid_fields, L"profile.decode",
                L"Profile is missing a required field");
        }
    }

    const auto field = [&known](ProfileFieldTag tag) -> const FieldView& {
        return *known[static_cast<std::uint16_t>(tag)];
    };
    for (const auto tag : {ProfileFieldTag::auto_start,
                           ProfileFieldTag::checkpoint_interval_enabled,
                           ProfileFieldTag::dirty_threshold_enabled,
                           ProfileFieldTag::checkpoint_on_memory_pressure,
                           ProfileFieldTag::automatic_compaction}) {
        if (!valid_boolean(field(tag))) {
            return codec_failure<ProfileRecord>(
                ProfileCodecError::invalid_fields, L"profile.decode",
                L"Profile boolean field has an invalid value");
        }
    }
    const auto byte_value = [&field](ProfileFieldTag tag) {
        return std::to_integer<std::uint8_t>(field(tag).value[0]);
    };
    if (byte_value(ProfileFieldTag::lifecycle) > 1 ||
        byte_value(ProfileFieldTag::mount_kind) > 1 ||
        byte_value(ProfileFieldTag::memory_mode) > 1 ||
        byte_value(ProfileFieldTag::checkpoint_on_shutdown) > 1 ||
        byte_value(ProfileFieldTag::checkpoint_on_suspend) > 1 ||
        byte_value(ProfileFieldTag::normal_stop_policy) > 1) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_fields, L"profile.decode",
            L"Profile enumeration field has an invalid value");
    }

    auto profile_uuid = decode_uuid(field(ProfileFieldTag::profile_uuid).value);
    auto owner = decode_utf8(field(ProfileFieldTag::owner_sid).value, 256);
    auto name = decode_utf8(field(ProfileFieldTag::display_name).value, 512);
    auto bundle = decode_utf8(field(ProfileFieldTag::bundle_path).value, 32'768);
    auto manifest_wide = decode_utf8(field(ProfileFieldTag::manifest_name).value, 255);
    auto image_uuid = decode_uuid(field(ProfileFieldTag::image_uuid).value);
    auto disk_guid = decode_optional_uuid(field(ProfileFieldTag::gpt_disk_guid).value);
    auto partition_guid =
        decode_optional_uuid(field(ProfileFieldTag::partition_guid).value);
    auto volume_guid = decode_optional_uuid(field(ProfileFieldTag::volume_guid).value);
    auto mount = decode_utf8(field(ProfileFieldTag::mount_target).value, 32'768);
    if (!profile_uuid.ok() || !owner.ok() || !name.ok() || !bundle.ok() ||
        !manifest_wide.ok() || !image_uuid.ok() || !disk_guid.ok() ||
        !partition_guid.ok() || !volume_guid.ok() || !mount.ok()) {
        return codec_failure<ProfileRecord>(
            ProfileCodecError::invalid_fields, L"profile.decode",
            L"One or more profile fields cannot be decoded");
    }
    const auto manifest_bytes = field(ProfileFieldTag::manifest_name).value;
    std::string manifest(reinterpret_cast<const char*>(manifest_bytes.data()),
                         manifest_bytes.size());

    Profile decoded{
        profile_uuid.move_value(),
        owner.move_value(),
        name.move_value(),
        read_u64(field(ProfileFieldTag::created_unix_ms).value, 0),
        static_cast<ProfileLifecycle>(byte_value(ProfileFieldTag::lifecycle)),
        bundle.move_value(),
        std::move(manifest),
        image_uuid.move_value(),
        read_u64(field(ProfileFieldTag::logical_capacity).value, 0),
        read_u32(field(ProfileFieldTag::logical_sector_size).value, 0),
        read_u32(field(ProfileFieldTag::physical_sector_size).value, 0),
        disk_guid.move_value(),
        partition_guid.move_value(),
        volume_guid.move_value(),
        static_cast<MountKind>(byte_value(ProfileFieldTag::mount_kind)),
        mount.move_value(),
        byte_value(ProfileFieldTag::auto_start) != 0,
        MemoryPolicy{
            static_cast<MemoryMode>(byte_value(ProfileFieldTag::memory_mode)),
            read_u64(field(ProfileFieldTag::memory_hard_limit).value, 0),
            read_u64(field(ProfileFieldTag::memory_soft_limit).value, 0),
            read_u64(field(ProfileFieldTag::memory_low_watermark).value, 0),
            read_u64(field(ProfileFieldTag::snapshot_cow_limit).value, 0)},
        CheckpointPolicy{
            byte_value(ProfileFieldTag::checkpoint_interval_enabled) != 0,
            read_u64(field(ProfileFieldTag::checkpoint_interval_seconds).value, 0),
            byte_value(ProfileFieldTag::dirty_threshold_enabled) != 0,
            read_u64(field(ProfileFieldTag::dirty_threshold_bytes).value, 0),
            byte_value(ProfileFieldTag::checkpoint_on_memory_pressure) != 0,
            static_cast<BestEffortPolicy>(
                byte_value(ProfileFieldTag::checkpoint_on_shutdown)),
            static_cast<BestEffortPolicy>(
                byte_value(ProfileFieldTag::checkpoint_on_suspend)),
            static_cast<NormalStopPolicy>(
                byte_value(ProfileFieldTag::normal_stop_policy))},
        CompactionPolicy{
            byte_value(ProfileFieldTag::automatic_compaction) != 0,
            read_u32(field(ProfileFieldTag::journal_occupancy_percent).value, 0),
            read_u32(field(ProfileFieldTag::reclaimable_percent).value, 0),
            read_u32(field(ProfileFieldTag::checkpoint_count_threshold).value, 0),
            read_u32(field(ProfileFieldTag::index_headroom_percent).value, 0)},
        [&]() -> std::optional<std::wstring> {
            // The sentinel is what "no label was asked for" looks like on the wire; the tag is required, so a
            // record without it cannot exist.
            auto label = decode_utf8(field(ProfileFieldTag::volume_label).value, 64);
            if (!label.ok()) return std::nullopt;
            if (label.value() == std::wstring(1, L'\u0001')) return std::nullopt;
            return std::optional<std::wstring>(label.move_value());
        }()};
    return Result<ProfileRecord>::success(
        ProfileRecord{revision, std::move(decoded)});
}

} // namespace myrd::profile
