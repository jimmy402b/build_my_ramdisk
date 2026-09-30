#pragma once

#include "src/common/result.h"
#include "src/profile/profile_types.h"

#include <cstddef>
#include <span>
#include <vector>

namespace myrd::profile {

enum class ProfileFieldKind : std::uint8_t {
    uuid = 1,
    utf8 = 2,
    u8 = 3,
    u32 = 4,
    u64 = 5,
    boolean = 6,
    optional_uuid = 7,
};

inline constexpr std::uint8_t kProfileMandatoryField = 0x01;

enum class ProfileCodecError : std::uint32_t {
    invalid_size = 0x4701,
    invalid_magic,
    unsupported_schema,
    invalid_header,
    invalid_revision,
    checksum_mismatch,
    invalid_fields,
    text_encoding,
};

Result<std::vector<std::byte>> encode_profile(const ProfileRecord& record);
Result<ProfileRecord> decode_profile(std::span<const std::byte> bytes);

} // namespace myrd::profile
