#include "src/control/ipc_codec.h"
#include "src/control/ipc_frame.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace myrd::ipc {

Result<ProtocolVersion> negotiate_protocol(ProtocolVersion local,
                                           ProtocolVersion peer) {
    if (local.major != peer.major) {
        return Result<ProtocolVersion>::failure(Error{
            ErrorDomain::protocol, 0x2101, L"negotiate_protocol",
            L"Protocol major versions are incompatible"});
    }
    return Result<ProtocolVersion>::success(ProtocolVersion{
        local.major, std::min(local.minor, peer.minor)});
}

namespace {

constexpr std::size_t kFieldHeaderBytes = 8;
constexpr std::size_t kMaxSchemaFields = 256;

template <class T>
Result<T> codec_error(const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol, 0x2102, L"decode_typed_payload", message});
}

bool read_u16(std::span<const std::byte> bytes, std::size_t offset,
              std::uint16_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = static_cast<std::uint16_t>(
        static_cast<unsigned>(bytes[offset])
        | (static_cast<unsigned>(bytes[offset + 1]) << 8U));
    return true;
}

bool read_u32(std::span<const std::byte> bytes, std::size_t offset,
              std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = static_cast<std::uint32_t>(
        static_cast<unsigned>(bytes[offset])
        | (static_cast<unsigned>(bytes[offset + 1]) << 8U)
        | (static_cast<unsigned>(bytes[offset + 2]) << 16U)
        | (static_cast<unsigned>(bytes[offset + 3]) << 24U));
    return true;
}

const FieldDescriptor* find_descriptor(const CommandSchema& schema,
                                       std::uint16_t tag,
                                       std::uint16_t minor) noexcept {
    for (const auto& descriptor : schema.fields) {
        if (descriptor.tag == tag && descriptor.introduced_minor <= minor) {
            return &descriptor;
        }
    }
    return nullptr;
}

bool fixed_size_matches(FieldKind kind, std::uint32_t length) noexcept {
    switch (kind) {
    case FieldKind::uint32_le: return length == 4;
    case FieldKind::uint64_le: return length == 8;
    case FieldKind::uuid_raw: return length == 16;
    case FieldKind::boolean: return length == 1;
    case FieldKind::bytes:
    case FieldKind::string_utf8:
    case FieldKind::path_local:
    case FieldKind::string_pairs: return true;
    }
    return false;
}

bool is_valid_utf8(std::span<const std::byte> text) noexcept {
    if (text.empty()) return false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        if (first == 0) return false;
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (text.size() - offset - 1 < continuation_count) return false;
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

bool validate_string_pairs(std::span<const std::byte> value,
                           std::uint16_t maximum_items) noexcept {
    std::uint16_t count = 0;
    if (!read_u16(value, 0, count) || count == 0 || count > maximum_items) {
        return false;
    }
    std::size_t offset = 2;
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint16_t key_length = 0;
        if (!read_u16(value, offset, key_length)) return false;
        offset += 2;
        if (value.size() - offset < key_length
            || !is_valid_utf8(value.subspan(offset, key_length))) return false;
        offset += key_length;
        std::uint16_t value_length = 0;
        if (!read_u16(value, offset, value_length)) return false;
        offset += 2;
        if (value.size() - offset < value_length
            || !is_valid_utf8(value.subspan(offset, value_length))) return false;
        offset += value_length;
    }
    return offset == value.size();
}

struct ValidatedField {
    const FieldDescriptor* descriptor{};
    std::size_t value_offset{};
    std::uint32_t value_length{};
};

Result<std::vector<ValidatedField>> validate_payload(
    std::span<const std::byte> payload,
    const CommandSchema& schema,
    std::uint16_t negotiated_minor) {
    if (payload.size() > kMaxPayloadBytes || schema.max_fields == 0
        || schema.max_fields > kMaxSchemaFields
        || schema.fields.size() > kMaxSchemaFields) {
        return codec_error<std::vector<ValidatedField>>(
            L"Payload or schema exceeds fixed limits");
    }

    std::array<std::uint16_t, kMaxSchemaFields> seen{};
    std::array<ValidatedField, kMaxSchemaFields> validated{};
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (payload.size() - offset < kFieldHeaderBytes
            || count >= schema.max_fields) {
            return codec_error<std::vector<ValidatedField>>(
                L"Field header is truncated or field count exceeds schema");
        }
        std::uint16_t tag = 0;
        std::uint32_t length = 0;
        if (!read_u16(payload, offset, tag)
            || !read_u32(payload, offset + 4, length)) {
            return codec_error<std::vector<ValidatedField>>(
                L"Field header is truncated");
        }
        const auto kind_raw = static_cast<std::uint8_t>(payload[offset + 2]);
        const auto requirement_raw = static_cast<std::uint8_t>(payload[offset + 3]);
        if (kind_raw > static_cast<std::uint8_t>(FieldKind::string_pairs)
            || requirement_raw > static_cast<std::uint8_t>(FieldRequirement::required)) {
            return codec_error<std::vector<ValidatedField>>(
                L"Field kind or flags are invalid");
        }
        const auto* descriptor = find_descriptor(schema, tag, negotiated_minor);
        if (!descriptor
            || kind_raw != static_cast<std::uint8_t>(descriptor->kind)
            || requirement_raw != static_cast<std::uint8_t>(descriptor->requirement)
            || length > descriptor->max_value_bytes
            || !fixed_size_matches(descriptor->kind, length)) {
            return codec_error<std::vector<ValidatedField>>(
                L"Field is unknown or violates its schema");
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (seen[index] == tag) {
                return codec_error<std::vector<ValidatedField>>(
                    L"Duplicate field is prohibited");
            }
        }
        offset += kFieldHeaderBytes;
        if (payload.size() - offset < length) {
            return codec_error<std::vector<ValidatedField>>(
                L"Field value is truncated");
        }
        if ((descriptor->kind == FieldKind::string_utf8
             || descriptor->kind == FieldKind::path_local)
            && !is_valid_utf8(payload.subspan(offset, length))) {
            return codec_error<std::vector<ValidatedField>>(
                L"String field is not valid non-empty UTF-8");
        }
        if (descriptor->kind == FieldKind::boolean
            && static_cast<unsigned>(payload[offset]) > 1U) {
            return codec_error<std::vector<ValidatedField>>(
                L"Boolean field is not zero or one");
        }
        if (descriptor->kind == FieldKind::string_pairs
            && !validate_string_pairs(payload.subspan(offset, length),
                                      descriptor->max_array_items)) {
            return codec_error<std::vector<ValidatedField>>(
                L"String-pair array is malformed or exceeds its item limit");
        }
        seen[count] = tag;
        validated[count] = ValidatedField{descriptor, offset, length};
        ++count;
        offset += length;
    }

    for (const auto& descriptor : schema.fields) {
        if (descriptor.introduced_minor > negotiated_minor
            || descriptor.requirement != FieldRequirement::required) {
            continue;
        }
        bool found = false;
        for (std::size_t index = 0; index < count; ++index) {
            found = found || seen[index] == descriptor.tag;
        }
        if (!found) {
            return codec_error<std::vector<ValidatedField>>(
                L"Required field is missing");
        }
    }

    std::vector<ValidatedField> result;
    result.reserve(count);
    result.insert(result.end(), validated.begin(), validated.begin() + count);
    return Result<std::vector<ValidatedField>>::success(std::move(result));
}

Result<std::string> utf16_to_utf8(std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)())) {
        return codec_error<std::string>(L"Command string is empty or too large");
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return codec_error<std::string>(L"Command string contains invalid UTF-16");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) != required) {
        return codec_error<std::string>(L"Command string conversion failed");
    }
    return Result<std::string>::success(std::move(result));
}

Result<std::wstring> utf8_to_utf16(std::span<const std::byte> value) {
    if (!is_valid_utf8(value)) {
        return codec_error<std::wstring>(L"Command string is invalid UTF-8");
    }
    std::string text;
    text.reserve(value.size());
    for (const auto byte : value) {
        text.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
    }
    const int input_size = static_cast<int>(value.size());
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return codec_error<std::wstring>(L"Command string conversion failed");
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size,
            result.data(), required) != required) {
        return codec_error<std::wstring>(L"Command string conversion failed");
    }
    return Result<std::wstring>::success(std::move(result));
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_field_header(std::vector<std::byte>& payload,
                         std::uint16_t tag,
                         FieldKind kind,
                         FieldRequirement requirement,
                         std::uint32_t length) {
    append_u16(payload, tag);
    payload.push_back(static_cast<std::byte>(kind));
    payload.push_back(static_cast<std::byte>(requirement));
    append_u32(payload, length);
}

Result<void> append_string_field(std::vector<std::byte>& payload,
                                 std::uint16_t tag,
                                 FieldKind kind,
                                 std::wstring_view value,
                                 std::uint32_t maximum,
                                 FieldRequirement requirement =
                                     FieldRequirement::required) {
    auto utf8 = utf16_to_utf8(value);
    if (!utf8.ok()) return Result<void>::failure(utf8.error());
    if (utf8.value().size() > maximum) {
        return Result<void>::failure(Error{
            ErrorDomain::protocol, 0x2102, L"encode_command_payload",
            L"String field exceeds its limit"});
    }
    append_field_header(payload, tag, kind, requirement,
                        static_cast<std::uint32_t>(utf8.value().size()));
    for (const unsigned char ch : utf8.value()) {
        payload.push_back(static_cast<std::byte>(ch));
    }
    return Result<void>::success();
}

void append_uuid_field(std::vector<std::byte>& payload,
                       std::uint16_t tag,
                       const product::Uuid& value) {
    append_field_header(payload, tag, FieldKind::uuid_raw,
                        FieldRequirement::required, 16);
    payload.insert(payload.end(), value.bytes().begin(), value.bytes().end());
}

void append_bool_field(std::vector<std::byte>& payload,
                       std::uint16_t tag,
                       bool value,
                       FieldRequirement requirement = FieldRequirement::required) {
    append_field_header(payload, tag, FieldKind::boolean,
                        requirement, 1);
    payload.push_back(value ? std::byte{1} : std::byte{0});
}

void append_u32_field(std::vector<std::byte>& payload,
                      std::uint16_t tag,
                      std::uint32_t value,
                      FieldRequirement requirement = FieldRequirement::required) {
    append_field_header(payload, tag, FieldKind::uint32_le, requirement, 4);
    append_u32(payload, value);
}

void append_u64_field(std::vector<std::byte>& payload,
                      std::uint16_t tag,
                      std::uint64_t value,
                      FieldRequirement requirement = FieldRequirement::required) {
    append_field_header(payload, tag, FieldKind::uint64_le, requirement, 8);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        payload.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

Result<void> append_pairs_field(
    std::vector<std::byte>& payload,
    std::uint16_t tag,
    const std::vector<control::ConfigAssignment>& assignments) {
    if (assignments.empty() || assignments.size() > 128) {
        return Result<void>::failure(Error{
            ErrorDomain::protocol, 0x2102, L"encode_command_payload",
            L"Configuration assignment count is invalid"});
    }
    std::vector<std::byte> value;
    append_u16(value, static_cast<std::uint16_t>(assignments.size()));
    for (const auto& assignment : assignments) {
        auto key = utf16_to_utf8(assignment.key);
        auto item_value = utf16_to_utf8(assignment.value);
        if (!key.ok() || !item_value.ok() || key.value().size() > 4096
            || item_value.value().size() > 4096) {
            return Result<void>::failure(Error{
                ErrorDomain::protocol, 0x2102, L"encode_command_payload",
                L"Configuration assignment is invalid"});
        }
        append_u16(value, static_cast<std::uint16_t>(key.value().size()));
        for (const unsigned char ch : key.value()) value.push_back(static_cast<std::byte>(ch));
        append_u16(value, static_cast<std::uint16_t>(item_value.value().size()));
        for (const unsigned char ch : item_value.value()) value.push_back(static_cast<std::byte>(ch));
    }
    append_field_header(payload, tag, FieldKind::string_pairs,
                        FieldRequirement::required,
                        static_cast<std::uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
    return Result<void>::success();
}

Result<std::vector<std::byte>> encode_target(std::wstring_view target) {
    auto utf8 = utf16_to_utf8(target);
    if (!utf8.ok() || utf8.value().size() > 4096) {
        return codec_error<std::vector<std::byte>>(L"Command target is invalid");
    }
    std::vector<std::byte> payload;
    payload.reserve(8 + utf8.value().size());
    append_u16(payload, 1);
    payload.push_back(static_cast<std::byte>(FieldKind::string_utf8));
    payload.push_back(static_cast<std::byte>(FieldRequirement::required));
    append_u32(payload, static_cast<std::uint32_t>(utf8.value().size()));
    for (const unsigned char ch : utf8.value()) {
        payload.push_back(static_cast<std::byte>(ch));
    }
    return Result<std::vector<std::byte>>::success(std::move(payload));
}

constexpr std::array<FieldDescriptor, 1> kTargetFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
}};
constexpr CommandSchema kTargetSchema{0, 1, kTargetFields};

constexpr std::array<FieldDescriptor, 1> kUuidFields{{
    {1, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
}};
constexpr CommandSchema kUuidSchema{0, 1, kUuidFields};

constexpr std::array<FieldDescriptor, 3> kStopFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::boolean, FieldRequirement::required,
     FieldDisposition::consume, 0, 1, 0},
    {3, FieldKind::boolean, FieldRequirement::required,
     FieldDisposition::consume, 0, 1, 0},
}};
constexpr CommandSchema kStopSchema{
    static_cast<std::uint32_t>(CommandId::disk_stop), 3, kStopFields};
// Protocol minor 0 shipped with only target + force. Keep decoding that
// payload for a normal stop while requiring the new acknowledgement whenever
// force=true. New encoders continue to emit the third field, so the force
// acknowledgement is never silently synthesized on the wire.
constexpr std::array<FieldDescriptor, 2> kLegacyStopFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::boolean, FieldRequirement::required,
     FieldDisposition::consume, 0, 1, 0},
}};
constexpr CommandSchema kLegacyStopSchema{
    static_cast<std::uint32_t>(CommandId::disk_stop), 2, kLegacyStopFields};

constexpr std::array<FieldDescriptor, 2> kRepairFields{{
    {1, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
    {2, FieldKind::boolean, FieldRequirement::required,
     FieldDisposition::consume, 0, 1, 0},
}};
constexpr CommandSchema kRepairSchema{
    static_cast<std::uint32_t>(CommandId::disk_repair_profile), 2, kRepairFields};

constexpr std::array<FieldDescriptor, 3> kRebindFields{{
    {1, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
    {2, FieldKind::path_local, FieldRequirement::required,
     FieldDisposition::consume, 0, 131072, 0},
    {3, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
}};
constexpr CommandSchema kRebindSchema{
    static_cast<std::uint32_t>(CommandId::disk_rebind), 3, kRebindFields};

constexpr std::array<FieldDescriptor, 2> kForgetFields{{
    {1, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
    {2, FieldKind::uuid_raw, FieldRequirement::required,
     FieldDisposition::consume, 0, 16, 0},
}};
constexpr CommandSchema kForgetSchema{
    static_cast<std::uint32_t>(CommandId::disk_forget), 2, kForgetFields};

constexpr std::array<FieldDescriptor, 2> kConfigSetFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::string_pairs, FieldRequirement::required,
     FieldDisposition::consume, 0, 262144, 128},
}};
constexpr CommandSchema kConfigSetSchema{
    static_cast<std::uint32_t>(CommandId::disk_config_set), 2, kConfigSetFields};

constexpr std::array<FieldDescriptor, 3> kDiagnosticsFields{{
    {1, FieldKind::string_utf8, FieldRequirement::optional,
     FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::path_local, FieldRequirement::optional,
     FieldDisposition::consume, 0, 131072, 0},
    {3, FieldKind::boolean, FieldRequirement::optional,
     FieldDisposition::consume, 0, 1, 0},
}};
constexpr CommandSchema kDiagnosticsSchema{
    static_cast<std::uint32_t>(CommandId::diagnostics_collect), 3,
    kDiagnosticsFields};

constexpr std::array<FieldDescriptor, 4> kImportFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::path_local, FieldRequirement::required,
     FieldDisposition::consume, 0, 131072, 0},
    {3, FieldKind::string_utf8, FieldRequirement::required,
     FieldDisposition::consume, 0, 4096, 0},
    {4, FieldKind::string_utf8, FieldRequirement::optional,
     FieldDisposition::consume, 0, 4096, 0},
}};
constexpr CommandSchema kImportSchema{
    static_cast<std::uint32_t>(CommandId::disk_import), 4, kImportFields};

constexpr std::array<FieldDescriptor, 10> kCreateFields{{
    {1, FieldKind::string_utf8, FieldRequirement::required, FieldDisposition::consume, 0, 4096, 0},
    {2, FieldKind::path_local, FieldRequirement::required, FieldDisposition::consume, 0, 131072, 0},
    {3, FieldKind::uint64_le, FieldRequirement::required, FieldDisposition::consume, 0, 8, 0},
    {4, FieldKind::string_utf8, FieldRequirement::required, FieldDisposition::consume, 0, 4096, 0},
    {5, FieldKind::string_utf8, FieldRequirement::optional, FieldDisposition::consume, 0, 4096, 0},
    {6, FieldKind::boolean, FieldRequirement::required, FieldDisposition::consume, 0, 1, 0},
    {7, FieldKind::uint32_le, FieldRequirement::optional, FieldDisposition::consume, 0, 4, 0},
    {8, FieldKind::uint64_le, FieldRequirement::optional, FieldDisposition::consume, 0, 8, 0},
    {9, FieldKind::uint64_le, FieldRequirement::optional, FieldDisposition::consume, 0, 8, 0},
    {10, FieldKind::uint64_le, FieldRequirement::optional, FieldDisposition::consume, 0, 8, 0},
}};
constexpr CommandSchema kCreateSchema{
    static_cast<std::uint32_t>(CommandId::disk_create), 10, kCreateFields};

Result<std::wstring> decode_target(std::span<const std::byte> payload,
                                   std::uint16_t minor) {
    auto decoded = decode_typed_payload(payload, kTargetSchema, minor);
    if (!decoded.ok() || decoded.value().fields.size() != 1) {
        return codec_error<std::wstring>(L"Command target payload is invalid");
    }
    return utf8_to_utf16(decoded.value().fields[0].value);
}

std::vector<std::byte> encode_uuid_payload(const product::Uuid& value) {
    std::vector<std::byte> payload;
    payload.reserve(24);
    append_u16(payload, 1);
    payload.push_back(static_cast<std::byte>(FieldKind::uuid_raw));
    payload.push_back(static_cast<std::byte>(FieldRequirement::required));
    append_u32(payload, 16);
    payload.insert(payload.end(), value.bytes().begin(), value.bytes().end());
    return payload;
}

Result<product::Uuid> decode_uuid_payload(std::span<const std::byte> payload,
                                         std::uint16_t minor) {
    auto decoded = decode_typed_payload(payload, kUuidSchema, minor);
    if (!decoded.ok() || decoded.value().fields.size() != 1) {
        return codec_error<product::Uuid>(L"Command UUID payload is invalid");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    const auto& bytes = decoded.value().fields[0].value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        const auto value = static_cast<unsigned char>(bytes[index]);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0fU]);
    }
    auto parsed = product::Uuid::parse(text);
    if (!parsed.ok()) {
        return codec_error<product::Uuid>(L"Command UUID payload is invalid");
    }
    return parsed;
}

const DecodedField* find_field(const DecodedFieldSet& fields,
                               std::uint16_t tag) noexcept {
    for (const auto& field : fields.fields) {
        if (field.tag == tag) return &field;
    }
    return nullptr;
}

Result<product::Uuid> uuid_from_field(const DecodedFieldSet& fields,
                                     std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 16) {
        return codec_error<product::Uuid>(L"Required UUID field is missing");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < field->value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
        const auto value = static_cast<unsigned char>(field->value[index]);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0fU]);
    }
    auto parsed = product::Uuid::parse(text);
    if (!parsed.ok()) return codec_error<product::Uuid>(L"UUID field is invalid");
    return parsed;
}

Result<std::wstring> string_from_field(const DecodedFieldSet& fields,
                                       std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field) return codec_error<std::wstring>(L"Required string field is missing");
    return utf8_to_utf16(field->value);
}

Result<bool> bool_from_field(const DecodedFieldSet& fields,
                             std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 1
        || static_cast<unsigned>(field->value[0]) > 1U) {
        return codec_error<bool>(L"Required boolean field is invalid");
    }
    return Result<bool>::success(field->value[0] == std::byte{1});
}

Result<std::uint64_t> u64_from_field(const DecodedFieldSet& fields,
                                     std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 8) {
        return codec_error<std::uint64_t>(L"Required uint64 field is invalid");
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned>(field->value[index])) << (index * 8U);
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::uint32_t> u32_from_field(const DecodedFieldSet& fields,
                                     std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 4) {
        return codec_error<std::uint32_t>(L"Required uint32 field is invalid");
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned>(field->value[index])) << (index * 8U);
    }
    return Result<std::uint32_t>::success(value);
}

Result<std::vector<control::ConfigAssignment>> pairs_from_field(
    const DecodedFieldSet& fields, std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field) {
        return codec_error<std::vector<control::ConfigAssignment>>(
            L"Configuration assignments are missing");
    }
    std::span<const std::byte> bytes(field->value);
    std::uint16_t count = 0;
    if (!read_u16(bytes, 0, count)) {
        return codec_error<std::vector<control::ConfigAssignment>>(
            L"Configuration assignments are malformed");
    }
    std::size_t offset = 2;
    std::vector<control::ConfigAssignment> assignments;
    assignments.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint16_t key_length = 0;
        if (!read_u16(bytes, offset, key_length)) {
            return codec_error<std::vector<control::ConfigAssignment>>(
                L"Configuration assignments are malformed");
        }
        offset += 2;
        auto key = utf8_to_utf16(bytes.subspan(offset, key_length));
        offset += key_length;
        std::uint16_t value_length = 0;
        if (!read_u16(bytes, offset, value_length)) {
            return codec_error<std::vector<control::ConfigAssignment>>(
                L"Configuration assignments are malformed");
        }
        offset += 2;
        auto value = utf8_to_utf16(bytes.subspan(offset, value_length));
        offset += value_length;
        if (!key.ok() || !value.ok()) {
            return codec_error<std::vector<control::ConfigAssignment>>(
                L"Configuration assignments are malformed");
        }
        assignments.push_back(control::ConfigAssignment{
            key.move_value(), value.move_value()});
    }
    return Result<std::vector<control::ConfigAssignment>>::success(
        std::move(assignments));
}

Result<std::optional<std::wstring>> optional_string_from_field(
    const DecodedFieldSet& fields, std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field) {
        return Result<std::optional<std::wstring>>::success(std::nullopt);
    }
    auto value = utf8_to_utf16(field->value);
    if (!value.ok()) {
        return Result<std::optional<std::wstring>>::failure(value.error());
    }
    return Result<std::optional<std::wstring>>::success(value.move_value());
}

std::optional<std::uint64_t> optional_u64_from_field(
    const DecodedFieldSet& fields, std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 8) return std::nullopt;
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned>(field->value[index])) << (index * 8U);
    }
    return value;
}

std::optional<std::uint32_t> optional_u32_from_field(
    const DecodedFieldSet& fields, std::uint16_t tag) {
    const auto* field = find_field(fields, tag);
    if (!field || field->value.size() != 4) return std::nullopt;
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned>(field->value[index])) << (index * 8U);
    }
    return value;
}

} // namespace

Result<DecodedFieldSet> decode_typed_payload(
    std::span<const std::byte> payload,
    const CommandSchema& schema,
    std::uint16_t negotiated_minor) {
    auto validated = validate_payload(payload, schema, negotiated_minor);
    if (!validated.ok()) {
        return Result<DecodedFieldSet>::failure(validated.error());
    }

    DecodedFieldSet decoded;
    decoded.fields.reserve(validated.value().size());
    for (const auto& field : validated.value()) {
        if (field.descriptor->disposition == FieldDisposition::ignore) {
            continue;
        }
        const auto begin = payload.begin()
            + static_cast<std::ptrdiff_t>(field.value_offset);
        const auto end = begin + field.value_length;
        decoded.fields.push_back(DecodedField{
            field.descriptor->tag,
            field.descriptor->kind,
            std::vector<std::byte>(begin, end)});
        decoded.allocated_value_bytes += field.value_length;
    }
    return Result<DecodedFieldSet>::success(std::move(decoded));
}

Result<EncodedCommand> encode_command_payload(const control::Command& command) {
    return std::visit([](const auto& value) -> Result<EncodedCommand> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, control::StatusCommand>) {
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::status, {}});
        } else if constexpr (std::is_same_v<T, control::DiskListCommand>) {
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_list, {}});
        } else if constexpr (std::is_same_v<T, control::DiskShowCommand>
                             || std::is_same_v<T, control::DiskStartCommand>
                             || std::is_same_v<T, control::DiskCheckpointCommand>
                             || std::is_same_v<T, control::DiskCompactCommand>
                             || std::is_same_v<T, control::DiskConfigGetCommand>) {
            auto payload = encode_target(value.target);
            if (!payload.ok()) {
                return Result<EncodedCommand>::failure(payload.error());
            }
            constexpr CommandId id = [] {
                if constexpr (std::is_same_v<T, control::DiskShowCommand>) {
                    return CommandId::disk_show;
                } else if constexpr (std::is_same_v<T, control::DiskStartCommand>) {
                    return CommandId::disk_start;
                } else if constexpr (std::is_same_v<T, control::DiskCheckpointCommand>) {
                    return CommandId::disk_checkpoint;
                } else if constexpr (std::is_same_v<T, control::DiskCompactCommand>) {
                    return CommandId::disk_compact;
                } else {
                    return CommandId::disk_config_get;
                }
            }();
            return Result<EncodedCommand>::success(
                EncodedCommand{id, payload.move_value()});
        } else if constexpr (std::is_same_v<T, control::OperationListCommand>) {
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::operation_list, {}});
        } else if constexpr (std::is_same_v<T, control::DriverStatusCommand>) {
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::driver_status, {}});
        } else if constexpr (std::is_same_v<T, control::OperationShowCommand>
                             || std::is_same_v<T, control::OperationWaitCommand>
                             || std::is_same_v<T, control::OperationCancelCommand>) {
            constexpr CommandId id = std::is_same_v<T, control::OperationShowCommand>
                ? CommandId::operation_show
                : std::is_same_v<T, control::OperationWaitCommand>
                    ? CommandId::operation_wait : CommandId::operation_cancel;
            return Result<EncodedCommand>::success(
                EncodedCommand{id, encode_uuid_payload(value.operation_id)});
        } else if constexpr (std::is_same_v<T, control::DiskStopCommand>) {
            if (value.force != value.accept_volatile_data_loss) {
                return codec_error<EncodedCommand>(
                    L"Force stop requires a matching volatile-data-loss acknowledgement");
            }
            std::vector<std::byte> payload;
            auto target = append_string_field(payload, 1, FieldKind::string_utf8,
                                              value.target, 4096);
            if (!target.ok()) return Result<EncodedCommand>::failure(target.error());
            append_bool_field(payload, 2, value.force);
            append_bool_field(payload, 3, value.accept_volatile_data_loss);
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_stop, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskRepairProfileCommand>) {
            if (!value.from_previous) {
                return codec_error<EncodedCommand>(
                    L"Repair-profile requires the previous profile revision");
            }
            std::vector<std::byte> payload;
            append_uuid_field(payload, 1, value.profile_id);
            append_bool_field(payload, 2, value.from_previous);
            return Result<EncodedCommand>::success(EncodedCommand{
                CommandId::disk_repair_profile, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskRebindCommand>) {
            std::vector<std::byte> payload;
            append_uuid_field(payload, 1, value.profile_id);
            auto bundle = append_string_field(payload, 2, FieldKind::path_local,
                                              value.bundle, 131072);
            if (!bundle.ok()) return Result<EncodedCommand>::failure(bundle.error());
            append_uuid_field(payload, 3, value.expected_image_id);
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_rebind, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskForgetCommand>) {
            std::vector<std::byte> payload;
            append_uuid_field(payload, 1, value.profile_id);
            append_uuid_field(payload, 2, value.confirmation_id);
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_forget, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskConfigSetCommand>) {
            std::vector<std::byte> payload;
            auto target = append_string_field(payload, 1, FieldKind::string_utf8,
                                              value.target, 4096);
            auto pairs = append_pairs_field(payload, 2, value.assignments);
            if (!target.ok() || !pairs.ok()) {
                return codec_error<EncodedCommand>(L"Configuration payload is invalid");
            }
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_config_set, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiagnosticsCollectCommand>) {
            std::vector<std::byte> payload;
            if (value.disk) {
                auto result = append_string_field(payload, 1, FieldKind::string_utf8,
                    *value.disk, 4096, FieldRequirement::optional);
                if (!result.ok()) return Result<EncodedCommand>::failure(result.error());
            }
            if (value.output) {
                auto result = append_string_field(payload, 2, FieldKind::path_local,
                    *value.output, 131072, FieldRequirement::optional);
                if (!result.ok()) return Result<EncodedCommand>::failure(result.error());
            }
            if (value.include_paths) {
                append_bool_field(payload, 3, true, FieldRequirement::optional);
            }
            return Result<EncodedCommand>::success(EncodedCommand{
                CommandId::diagnostics_collect, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskImportCommand>) {
            std::vector<std::byte> payload;
            auto name = append_string_field(payload, 1, FieldKind::string_utf8,
                                            value.name, 4096);
            auto bundle = append_string_field(payload, 2, FieldKind::path_local,
                                              value.bundle, 131072);
            auto mount = append_string_field(payload, 3, FieldKind::string_utf8,
                                             value.mount, 4096);
            if (!name.ok() || !bundle.ok() || !mount.ok()) {
                return codec_error<EncodedCommand>(L"Import payload is invalid");
            }
            if (value.owner_sid) {
                auto owner = append_string_field(payload, 4, FieldKind::string_utf8,
                    *value.owner_sid, 4096, FieldRequirement::optional);
                if (!owner.ok()) return Result<EncodedCommand>::failure(owner.error());
            }
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_import, std::move(payload)});
        } else if constexpr (std::is_same_v<T, control::DiskCreateCommand>) {
            std::vector<std::byte> payload;
            auto name = append_string_field(payload, 1, FieldKind::string_utf8,
                                            value.name, 4096);
            auto bundle = append_string_field(payload, 2, FieldKind::path_local,
                                              value.bundle, 131072);
            append_u64_field(payload, 3, value.size_bytes);
            auto mount = append_string_field(payload, 4, FieldKind::string_utf8,
                                             value.mount, 4096);
            if (!name.ok() || !bundle.ok() || !mount.ok()) {
                return codec_error<EncodedCommand>(L"Create payload is invalid");
            }
            if (value.owner_sid) {
                auto owner = append_string_field(payload, 5, FieldKind::string_utf8,
                    *value.owner_sid, 4096, FieldRequirement::optional);
                if (!owner.ok()) return Result<EncodedCommand>::failure(owner.error());
            }
            append_bool_field(payload, 6, value.no_start);
            if (value.memory_mode) {
                append_u32_field(payload, 7,
                    *value.memory_mode == control::MemoryMode::fixed ? 1U : 0U,
                    FieldRequirement::optional);
            }
            if (value.memory_hard_limit_bytes) {
                append_u64_field(payload, 8, *value.memory_hard_limit_bytes,
                                 FieldRequirement::optional);
            }
            if (value.checkpoint_interval) {
                const auto encoded_interval = value.checkpoint_interval->enabled
                    ? static_cast<std::uint64_t>(
                        value.checkpoint_interval->duration.count())
                    : (std::numeric_limits<std::uint64_t>::max)();
                append_u64_field(payload, 9, encoded_interval,
                                 FieldRequirement::optional);
            }
            if (value.dirty_threshold) {
                const auto encoded_dirty = value.dirty_threshold->enabled
                    ? value.dirty_threshold->bytes
                    : (std::numeric_limits<std::uint64_t>::max)();
                append_u64_field(payload, 10, encoded_dirty,
                                 FieldRequirement::optional);
            }
            return Result<EncodedCommand>::success(
                EncodedCommand{CommandId::disk_create, std::move(payload)});
        } else {
            return codec_error<EncodedCommand>(
                L"Command codec is not implemented for this command");
        }
    }, command);
}

Result<control::Command> decode_command_payload(
    CommandId command_id,
    std::span<const std::byte> payload,
    std::uint16_t negotiated_minor) {
    if (command_id == CommandId::status && payload.empty()) {
        return Result<control::Command>::success(control::StatusCommand{});
    }
    if (command_id == CommandId::disk_list && payload.empty()) {
        return Result<control::Command>::success(control::DiskListCommand{});
    }
    if (command_id == CommandId::status || command_id == CommandId::disk_list) {
        return codec_error<control::Command>(L"No-field command has a payload");
    }
    if (command_id == CommandId::disk_show
        || command_id == CommandId::disk_start
        || command_id == CommandId::disk_checkpoint
        || command_id == CommandId::disk_compact
        || command_id == CommandId::disk_config_get) {
        auto target = decode_target(payload, negotiated_minor);
        if (!target.ok()) {
            return Result<control::Command>::failure(target.error());
        }
        if (command_id == CommandId::disk_show) {
            return Result<control::Command>::success(
                control::DiskShowCommand{target.move_value()});
        }
        if (command_id == CommandId::disk_start) {
            return Result<control::Command>::success(
                control::DiskStartCommand{target.move_value()});
        }
        if (command_id == CommandId::disk_checkpoint) {
            return Result<control::Command>::success(
                control::DiskCheckpointCommand{target.move_value()});
        }
        if (command_id == CommandId::disk_compact) {
            return Result<control::Command>::success(
                control::DiskCompactCommand{target.move_value()});
        }
        return Result<control::Command>::success(
            control::DiskConfigGetCommand{target.move_value()});
    }
    if ((command_id == CommandId::operation_list
         || command_id == CommandId::driver_status) && payload.empty()) {
        if (command_id == CommandId::operation_list) {
            return Result<control::Command>::success(control::OperationListCommand{});
        }
        return Result<control::Command>::success(control::DriverStatusCommand{});
    }
    if (command_id == CommandId::operation_show
        || command_id == CommandId::operation_wait
        || command_id == CommandId::operation_cancel) {
        auto operation_id = decode_uuid_payload(payload, negotiated_minor);
        if (!operation_id.ok()) {
            return Result<control::Command>::failure(operation_id.error());
        }
        if (command_id == CommandId::operation_show) {
            return Result<control::Command>::success(
                control::OperationShowCommand{operation_id.move_value()});
        }
        if (command_id == CommandId::operation_wait) {
            return Result<control::Command>::success(
                control::OperationWaitCommand{operation_id.move_value()});
        }
        return Result<control::Command>::success(
            control::OperationCancelCommand{operation_id.move_value()});
    }
    if (command_id == CommandId::disk_stop) {
        auto fields = decode_typed_payload(payload, kStopSchema, negotiated_minor);
        bool legacy_payload = false;
        if (!fields.ok()) {
            fields = decode_typed_payload(
                payload, kLegacyStopSchema, negotiated_minor);
            legacy_payload = fields.ok();
        }
        if (!fields.ok()) return Result<control::Command>::failure(fields.error());
        auto target = string_from_field(fields.value(), 1);
        auto force = bool_from_field(fields.value(), 2);
        auto acknowledgement = legacy_payload
            ? Result<bool>::success(false)
            : bool_from_field(fields.value(), 3);
        if (!target.ok() || !force.ok() || !acknowledgement.ok()
            || force.value() != acknowledgement.value()) {
            return codec_error<control::Command>(L"Stop payload is invalid");
        }
        return Result<control::Command>::success(
            control::DiskStopCommand{
                target.move_value(), force.value(), acknowledgement.value()});
    }
    if (command_id == CommandId::disk_repair_profile) {
        auto fields = decode_typed_payload(payload, kRepairSchema, negotiated_minor);
        if (!fields.ok()) return Result<control::Command>::failure(fields.error());
        auto profile = uuid_from_field(fields.value(), 1);
        auto from_previous = bool_from_field(fields.value(), 2);
        if (!profile.ok() || !from_previous.ok() || !from_previous.value()) {
            return codec_error<control::Command>(L"Repair payload is invalid");
        }
        return Result<control::Command>::success(control::DiskRepairProfileCommand{
            profile.move_value(), from_previous.value()});
    }
    if (command_id == CommandId::disk_rebind) {
        auto fields = decode_typed_payload(payload, kRebindSchema, negotiated_minor);
        if (!fields.ok()) return Result<control::Command>::failure(fields.error());
        auto profile = uuid_from_field(fields.value(), 1);
        auto bundle_text = string_from_field(fields.value(), 2);
        auto image = uuid_from_field(fields.value(), 3);
        if (!profile.ok() || !bundle_text.ok() || !image.ok()) {
            return codec_error<control::Command>(L"Rebind payload is invalid");
        }
        auto bundle = product::normalize_local_absolute_path(bundle_text.value());
        if (!bundle.ok()) {
            return codec_error<control::Command>(L"Rebind bundle path is invalid");
        }
        return Result<control::Command>::success(control::DiskRebindCommand{
            profile.move_value(), bundle.move_value(), image.move_value()});
    }
    if (command_id == CommandId::disk_forget) {
        auto fields = decode_typed_payload(payload, kForgetSchema, negotiated_minor);
        if (!fields.ok()) return Result<control::Command>::failure(fields.error());
        auto profile = uuid_from_field(fields.value(), 1);
        auto confirmation = uuid_from_field(fields.value(), 2);
        if (!profile.ok() || !confirmation.ok()
            || profile.value() != confirmation.value()) {
            return codec_error<control::Command>(L"Forget identity confirmation is invalid");
        }
        return Result<control::Command>::success(control::DiskForgetCommand{
            profile.move_value(), confirmation.move_value()});
    }
    if (command_id == CommandId::disk_config_set) {
        auto fields = decode_typed_payload(payload, kConfigSetSchema,
                                           negotiated_minor);
        if (!fields.ok()) {
            return Result<control::Command>::failure(fields.error());
        }
        auto target = string_from_field(fields.value(), 1);
        auto assignments = pairs_from_field(fields.value(), 2);
        if (!target.ok() || !assignments.ok()) {
            return codec_error<control::Command>(
                L"Configuration payload is invalid");
        }
        return Result<control::Command>::success(
            control::DiskConfigSetCommand{
                target.move_value(), assignments.move_value()});
    }
    if (command_id == CommandId::diagnostics_collect) {
        auto fields = decode_typed_payload(payload, kDiagnosticsSchema,
                                           negotiated_minor);
        if (!fields.ok()) {
            return Result<control::Command>::failure(fields.error());
        }
        auto disk = optional_string_from_field(fields.value(), 1);
        auto output_text = optional_string_from_field(fields.value(), 2);
        if (!disk.ok() || !output_text.ok()) {
            return codec_error<control::Command>(
                L"Diagnostics payload is invalid");
        }
        std::optional<std::wstring> output;
        if (output_text.value()) {
            auto normalized = product::normalize_local_absolute_path(
                *output_text.value());
            if (!normalized.ok()) {
                return codec_error<control::Command>(
                    L"Diagnostics output path is invalid");
            }
            output = normalized.move_value();
        }
        bool include_paths = false;
        if (find_field(fields.value(), 3)) {
            auto decoded = bool_from_field(fields.value(), 3);
            if (!decoded.ok()) {
                return codec_error<control::Command>(
                    L"Diagnostics path flag is invalid");
            }
            include_paths = decoded.value();
        }
        return Result<control::Command>::success(
            control::DiagnosticsCollectCommand{
                disk.move_value(), std::move(output), include_paths});
    }
    if (command_id == CommandId::disk_import) {
        auto fields = decode_typed_payload(payload, kImportSchema,
                                           negotiated_minor);
        if (!fields.ok()) {
            return Result<control::Command>::failure(fields.error());
        }
        auto name = string_from_field(fields.value(), 1);
        auto bundle_text = string_from_field(fields.value(), 2);
        auto mount = string_from_field(fields.value(), 3);
        auto owner = optional_string_from_field(fields.value(), 4);
        if (!name.ok() || !bundle_text.ok() || !mount.ok() || !owner.ok()) {
            return codec_error<control::Command>(L"Import payload is invalid");
        }
        auto bundle = product::normalize_local_absolute_path(bundle_text.value());
        if (!bundle.ok()) {
            return codec_error<control::Command>(L"Import bundle path is invalid");
        }
        return Result<control::Command>::success(control::DiskImportCommand{
            name.move_value(), bundle.move_value(), mount.move_value(),
            owner.move_value()});
    }
    if (command_id == CommandId::disk_create) {
        auto fields = decode_typed_payload(payload, kCreateSchema,
                                           negotiated_minor);
        if (!fields.ok()) {
            return Result<control::Command>::failure(fields.error());
        }
        auto name = string_from_field(fields.value(), 1);
        auto bundle_text = string_from_field(fields.value(), 2);
        auto size = u64_from_field(fields.value(), 3);
        auto mount = string_from_field(fields.value(), 4);
        auto owner = optional_string_from_field(fields.value(), 5);
        auto no_start = bool_from_field(fields.value(), 6);
        if (!name.ok() || !bundle_text.ok() || !size.ok() || size.value() == 0
            || !mount.ok() || !owner.ok() || !no_start.ok()) {
            return codec_error<control::Command>(L"Create payload is invalid");
        }
        auto bundle = product::normalize_local_absolute_path(bundle_text.value());
        if (!bundle.ok()) {
            return codec_error<control::Command>(L"Create bundle path is invalid");
        }

        std::optional<control::MemoryMode> memory_mode;
        const auto encoded_memory_mode = optional_u32_from_field(fields.value(), 7);
        if (encoded_memory_mode) {
            if (*encoded_memory_mode > 1) {
                return codec_error<control::Command>(L"Memory mode is invalid");
            }
            memory_mode = *encoded_memory_mode == 0
                ? control::MemoryMode::automatic : control::MemoryMode::fixed;
        }
        const auto hard_limit = optional_u64_from_field(fields.value(), 8);
        if ((memory_mode == control::MemoryMode::fixed) != hard_limit.has_value()
            || (hard_limit && (*hard_limit == 0 || *hard_limit > size.value()))) {
            return codec_error<control::Command>(
                L"Fixed memory mode and hard limit are inconsistent");
        }

        std::optional<product::CheckpointInterval> checkpoint;
        const auto encoded_checkpoint = optional_u64_from_field(fields.value(), 9);
        if (encoded_checkpoint) {
            if (*encoded_checkpoint == (std::numeric_limits<std::uint64_t>::max)()) {
                checkpoint = product::CheckpointInterval{
                    false, std::chrono::seconds::zero()};
            } else if (*encoded_checkpoint < 30 || *encoded_checkpoint > 7 * 24 * 60 * 60) {
                return codec_error<control::Command>(
                    L"Checkpoint interval is outside its limits");
            } else {
                checkpoint = product::CheckpointInterval{
                    true, std::chrono::seconds(*encoded_checkpoint)};
            }
        }

        std::optional<control::OptionalSize> dirty_threshold;
        const auto encoded_dirty = optional_u64_from_field(fields.value(), 10);
        if (encoded_dirty) {
            if (*encoded_dirty == (std::numeric_limits<std::uint64_t>::max)()) {
                dirty_threshold = control::OptionalSize{false, 0};
            } else if (*encoded_dirty == 0) {
                return codec_error<control::Command>(
                    L"Dirty threshold must be nonzero when enabled");
            } else {
                dirty_threshold = control::OptionalSize{true, *encoded_dirty};
            }
        }

        return Result<control::Command>::success(control::DiskCreateCommand{
            name.move_value(), bundle.move_value(), size.value(),
            mount.move_value(), owner.move_value(), no_start.value(), memory_mode,
            hard_limit, checkpoint, dirty_threshold});
    }
    return codec_error<control::Command>(L"Command ID is unknown or unsupported");
}

Result<void> InFlightGate::begin() noexcept {
    bool expected = false;
    if (!active_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
        return Result<void>::failure(Error{
            ErrorDomain::protocol, 0x2103, L"InFlightGate::begin",
            L"A request is already in flight on this connection"});
    }
    return Result<void>::success();
}

Result<void> InFlightGate::complete() noexcept {
    if (!active_.exchange(false, std::memory_order_acq_rel)) {
        return Result<void>::failure(Error{
            ErrorDomain::protocol, 0x2104, L"InFlightGate::complete",
            L"No request is in flight on this connection"});
    }
    return Result<void>::success();
}

bool private_product_version_matches(ProductVersion local,
                                     ProductVersion peer) noexcept {
    return local == peer;
}

} // namespace myrd::ipc
