#include "src/common/diagnostic_trace.h"
#include "src/management/operation_store.h"

#include "src/common/sha256.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>

namespace myrd::management {
namespace {

constexpr std::size_t kHashOffset = 40;
constexpr std::uint8_t kMandatory = 1;

enum class FieldTag : std::uint16_t {
    request_id = 1,
    operation_id = 2,
    profile_id = 3,
    caller_sid = 4,
    command_scope = 5,
    argument_digest = 6,
    state = 7,
    phase = 8,
    created_unix_ms = 9,
    updated_unix_ms = 10,
    terminal_unix_ms = 11,
    cancellable = 12,
    cancel_requested = 13,
    terminal_result = 14,
    revision_binding = 15,
};

enum class FieldKind : std::uint8_t {
    uuid = 1,
    utf8 = 2,
    bytes = 3,
    u8 = 4,
    u64 = 5,
    boolean = 6,
    optional_u64 = 7,
};

template <class T>
Result<T> operation_error(OperationStoreError code,
                          const wchar_t* operation,
                          const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code), operation, message});
}

struct LocalFreeDeleter final {
    void operator()(void* value) const noexcept {
        if (value != nullptr) static_cast<void>(LocalFree(value));
    }
};
using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

bool is_zero(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

bool is_terminal(OperationState state) noexcept {
    return state == OperationState::succeeded
        || state == OperationState::failed
        || state == OperationState::cancelled;
}

bool valid_command(std::string_view value) noexcept {
    static constexpr std::array<std::string_view, 9> kCommands{
        "disk.create", "disk.import", "disk.start", "disk.stop",
        "disk.checkpoint", "disk.compact", "disk.rebind",
        "disk.repair-profile", "disk.forget"};
    return std::find(kCommands.begin(), kCommands.end(), value)
        != kCommands.end();
}

bool valid_stable_token(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-';
    });
}

bool canonical_sid(std::wstring_view value) {
    if (value.empty() || value.size() > 256
        || value.find(L'\0') != std::wstring_view::npos) {
        return false;
    }
    const std::wstring terminated(value);
    PSID raw_sid = nullptr;
    if (!ConvertStringSidToSidW(terminated.c_str(), &raw_sid)) return false;
    LocalMemory sid(raw_sid);
    wchar_t* raw_canonical = nullptr;
    if (!ConvertSidToStringSidW(raw_sid, &raw_canonical)) return false;
    LocalMemory canonical(raw_canonical);
    return value == static_cast<const wchar_t*>(raw_canonical);
}

bool valid_record(const OperationRecord& record) {
    if (record.revision == 0 || is_zero(record.request_id)
        || is_zero(record.operation_id) || is_zero(record.profile_id)
        || !canonical_sid(record.caller_sid)
        || !valid_command(record.command_scope)
        || !valid_stable_token(record.phase)
        || std::all_of(
            record.argument_digest.begin(), record.argument_digest.end(),
            [](std::byte value) { return value == std::byte{0}; })
        || static_cast<std::uint8_t>(record.state)
            > static_cast<std::uint8_t>(OperationState::cancelled)
        || record.created_unix_ms == 0
        || record.updated_unix_ms < record.created_unix_ms
        || record.terminal_result.size()
            > kOperationMaximumTerminalResultBytes) {
        return false;
    }
    if (is_terminal(record.state)) {
        if (!record.terminal_unix_ms.has_value()
            || record.terminal_unix_ms.value() != record.updated_unix_ms
            || record.terminal_result.empty() || record.cancellable) {
            return false;
        }
        return (record.state == OperationState::cancelled)
            == record.cancel_requested;
    }
    return !record.terminal_unix_ms.has_value()
        && record.terminal_result.empty()
        && !(record.cancel_requested && !record.cancellable);
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
        std::to_integer<std::uint8_t>(bytes[offset])
        | (static_cast<std::uint16_t>(
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
    std::wstring_view value, std::size_t maximum) {
    if (value.empty()
        || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return operation_error<std::vector<std::byte>>(
            OperationStoreError::invalid_record,
            L"operation.encode", L"Operation text is invalid");
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0 || static_cast<std::size_t>(required) > maximum) {
        return operation_error<std::vector<std::byte>>(
            OperationStoreError::invalid_record,
            L"operation.encode", L"Operation text exceeds its bound");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(required));
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()),
        reinterpret_cast<char*>(result.data()), required, nullptr, nullptr);
    if (written != required) {
        return operation_error<std::vector<std::byte>>(
            OperationStoreError::invalid_record,
            L"operation.encode", L"Operation text encoding failed");
    }
    return Result<std::vector<std::byte>>::success(std::move(result));
}

Result<std::wstring> decode_utf8(
    std::span<const std::byte> value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum
        || std::find(value.begin(), value.end(), std::byte{0}) != value.end()
        || value.size()
            > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return operation_error<std::wstring>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation text is invalid");
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(value.data()),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return operation_error<std::wstring>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation text is not canonical UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(value.data()),
        static_cast<int>(value.size()), result.data(), required);
    if (written != required) {
        return operation_error<std::wstring>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation text decoding failed");
    }
    return Result<std::wstring>::success(std::move(result));
}

void append_field(std::vector<std::byte>& payload,
                  FieldTag tag,
                  FieldKind kind,
                  std::span<const std::byte> value) {
    append_u16(payload, static_cast<std::uint16_t>(tag));
    payload.push_back(static_cast<std::byte>(kind));
    payload.push_back(static_cast<std::byte>(kMandatory));
    append_u32(payload, static_cast<std::uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
}

void append_u8_field(std::vector<std::byte>& payload,
                     FieldTag tag, FieldKind kind, std::uint8_t value) {
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    append_field(payload, tag, kind, bytes);
}

void append_u64_field(std::vector<std::byte>& payload,
                      FieldTag tag, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned index = 0; index != 8; ++index) {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    append_field(payload, tag, FieldKind::u64, bytes);
}

std::string uuid_text(std::span<const std::byte> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
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
        return operation_error<product::Uuid>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation UUID length is invalid");
    }
    auto parsed = product::Uuid::parse(uuid_text(bytes));
    if (!parsed.ok()) {
        return operation_error<product::Uuid>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation UUID is invalid");
    }
    return parsed;
}

struct Field final {
    FieldTag tag;
    FieldKind kind;
    std::span<const std::byte> value;
};

Result<std::array<Field, kOperationFieldCount>> parse_fields(
    std::span<const std::byte> payload) {
    std::array<Field, kOperationFieldCount> fields{};
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (payload.size() - cursor < 8) {
            return operation_error<std::array<Field, kOperationFieldCount>>(
                OperationStoreError::invalid_fields,
                L"operation.decode", L"Operation field header is truncated");
        }
        const auto tag = read_u16(payload, cursor);
        const auto kind = std::to_integer<std::uint8_t>(payload[cursor + 2]);
        const auto flags = std::to_integer<std::uint8_t>(payload[cursor + 3]);
        const auto length = read_u32(payload, cursor + 4);
        cursor += 8;
        if (tag != index + 1 || flags != kMandatory
            || length > payload.size() - cursor) {
            return operation_error<std::array<Field, kOperationFieldCount>>(
                OperationStoreError::invalid_fields,
                L"operation.decode", L"Operation fields are noncanonical");
        }
        fields[index] = Field{
            static_cast<FieldTag>(tag), static_cast<FieldKind>(kind),
            payload.subspan(cursor, length)};
        cursor += length;
    }
    if (cursor != payload.size()) {
        return operation_error<std::array<Field, kOperationFieldCount>>(
            OperationStoreError::invalid_fields,
            L"operation.decode", L"Operation payload has trailing bytes");
    }
    return Result<std::array<Field, kOperationFieldCount>>::success(fields);
}

bool field_contract(const std::array<Field, kOperationFieldCount>& fields) {
    const auto check = [&fields](FieldTag tag, FieldKind kind,
                                 std::size_t minimum,
                                 std::size_t maximum) {
        const auto& field = fields[static_cast<std::size_t>(tag) - 1];
        return field.kind == kind && field.value.size() >= minimum
            && field.value.size() <= maximum;
    };
    return check(FieldTag::request_id, FieldKind::uuid, 16, 16)
        && check(FieldTag::operation_id, FieldKind::uuid, 16, 16)
        && check(FieldTag::profile_id, FieldKind::uuid, 16, 16)
        && check(FieldTag::caller_sid, FieldKind::utf8, 1, 256)
        && check(FieldTag::command_scope, FieldKind::utf8, 1, 64)
        && check(FieldTag::argument_digest, FieldKind::bytes, 32, 32)
        && check(FieldTag::state, FieldKind::u8, 1, 1)
        && check(FieldTag::phase, FieldKind::utf8, 1, 64)
        && check(FieldTag::created_unix_ms, FieldKind::u64, 8, 8)
        && check(FieldTag::updated_unix_ms, FieldKind::u64, 8, 8)
        && check(FieldTag::terminal_unix_ms, FieldKind::optional_u64, 0, 8)
        && check(FieldTag::cancellable, FieldKind::boolean, 1, 1)
        && check(FieldTag::cancel_requested, FieldKind::boolean, 1, 1)
        && check(FieldTag::terminal_result, FieldKind::bytes, 0,
                 kOperationMaximumTerminalResultBytes)
        && check(FieldTag::revision_binding, FieldKind::u64, 8, 8);
}

} // namespace

Result<std::vector<std::byte>> encode_operation_record(
    const OperationRecord& record) {
    try {
        if (!valid_record(record)) {
            return operation_error<std::vector<std::byte>>(
                OperationStoreError::invalid_record,
                L"operation.encode", L"Operation record invariants are invalid");
        }
        auto sid = encode_utf8(record.caller_sid, 256);
        if (!sid.ok()) return sid;
        std::vector<std::byte> payload;
        payload.reserve(256 + record.terminal_result.size());
        append_field(payload, FieldTag::request_id, FieldKind::uuid,
                     record.request_id.bytes());
        append_field(payload, FieldTag::operation_id, FieldKind::uuid,
                     record.operation_id.bytes());
        append_field(payload, FieldTag::profile_id, FieldKind::uuid,
                     record.profile_id.bytes());
        append_field(payload, FieldTag::caller_sid, FieldKind::utf8, sid.value());
        append_field(payload, FieldTag::command_scope, FieldKind::utf8,
                     std::as_bytes(std::span(record.command_scope)));
        append_field(payload, FieldTag::argument_digest, FieldKind::bytes,
                     record.argument_digest);
        append_u8_field(payload, FieldTag::state, FieldKind::u8,
                        static_cast<std::uint8_t>(record.state));
        append_field(payload, FieldTag::phase, FieldKind::utf8,
                     std::as_bytes(std::span(record.phase)));
        append_u64_field(payload, FieldTag::created_unix_ms,
                         record.created_unix_ms);
        append_u64_field(payload, FieldTag::updated_unix_ms,
                         record.updated_unix_ms);
        if (record.terminal_unix_ms.has_value()) {
            std::array<std::byte, 8> terminal{};
            const auto value = record.terminal_unix_ms.value();
            for (unsigned index = 0; index != 8; ++index) {
                terminal[index] = static_cast<std::byte>(
                    (value >> (index * 8U)) & 0xffU);
            }
            append_field(payload, FieldTag::terminal_unix_ms,
                         FieldKind::optional_u64, terminal);
        } else {
            append_field(payload, FieldTag::terminal_unix_ms,
                         FieldKind::optional_u64, {});
        }
        append_u8_field(payload, FieldTag::cancellable, FieldKind::boolean,
                        static_cast<std::uint8_t>(record.cancellable));
        append_u8_field(payload, FieldTag::cancel_requested, FieldKind::boolean,
                        static_cast<std::uint8_t>(record.cancel_requested));
        append_field(payload, FieldTag::terminal_result, FieldKind::bytes,
                     record.terminal_result);
        append_u64_field(payload, FieldTag::revision_binding, record.revision);
        if (payload.size() > kOperationMaximumBytes - kOperationHeaderBytes) {
            return operation_error<std::vector<std::byte>>(
                OperationStoreError::invalid_size,
                L"operation.encode", L"Operation record exceeds its bound");
        }
        auto hash = sha256_digest(payload);
        if (!hash.ok()) {
            return Result<std::vector<std::byte>>::failure(hash.error());
        }
        std::vector<std::byte> output;
        output.reserve(kOperationHeaderBytes + payload.size());
        output.insert(output.end(), kOperationMagic.begin(), kOperationMagic.end());
        append_u16(output, kOperationSchemaMajor);
        append_u16(output, kOperationSchemaMinor);
        append_u32(output, kOperationHeaderBytes);
        append_u64(output, kOperationHeaderBytes + payload.size());
        append_u64(output, record.revision);
        append_u32(output, static_cast<std::uint32_t>(payload.size()));
        append_u32(output, kOperationFieldCount);
        output.insert(output.end(), hash.value().begin(), hash.value().end());
        output.insert(output.end(), payload.begin(), payload.end());
        return Result<std::vector<std::byte>>::success(std::move(output));
    } catch (...) {
        return operation_error<std::vector<std::byte>>(
            OperationStoreError::io_failure,
            L"operation.encode", L"Operation encoding allocation failed");
    }
}

Result<OperationRecord> decode_operation_record(
    std::span<const std::byte> bytes) {
    try {
        if (bytes.size() < kOperationHeaderBytes
            || bytes.size() > kOperationMaximumBytes) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_size,
                L"operation.decode", L"Operation file length is invalid");
        }
        if (!std::equal(kOperationMagic.begin(), kOperationMagic.end(),
                        bytes.begin())) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_magic,
                L"operation.decode", L"Operation magic is invalid");
        }
        if (read_u16(bytes, 8) != kOperationSchemaMajor
            || read_u16(bytes, 10) != kOperationSchemaMinor) {
            return operation_error<OperationRecord>(
                OperationStoreError::unsupported_schema,
                L"operation.decode", L"Operation schema is unsupported");
        }
        const auto payload_bytes = read_u32(bytes, 32);
        if (read_u32(bytes, 12) != kOperationHeaderBytes
            || read_u64(bytes, 16) != bytes.size()
            || read_u64(bytes, 24) == 0
            || payload_bytes != bytes.size() - kOperationHeaderBytes
            || read_u32(bytes, 36) != kOperationFieldCount) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_header,
                L"operation.decode", L"Operation header is inconsistent");
        }
        const auto payload = bytes.subspan(kOperationHeaderBytes);
        auto hash = sha256_digest(payload);
        if (!hash.ok()) return Result<OperationRecord>::failure(hash.error());
        if (!std::equal(hash.value().begin(), hash.value().end(),
                        bytes.begin() + kHashOffset)) {
            return operation_error<OperationRecord>(
                OperationStoreError::checksum_mismatch,
                L"operation.decode", L"Operation checksum does not match");
        }
        auto parsed = parse_fields(payload);
        if (!parsed.ok()) return Result<OperationRecord>::failure(parsed.error());
        if (!field_contract(parsed.value())) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_fields,
                L"operation.decode", L"Operation field contract is invalid");
        }
        const auto& fields = parsed.value();
        if (read_u64(fields[14].value, 0) != read_u64(bytes, 24)) {
            return operation_error<OperationRecord>(
                OperationStoreError::checksum_mismatch,
                L"operation.decode",
                L"Operation revision is not bound to the payload checksum");
        }
        auto request = decode_uuid(fields[0].value);
        auto operation = decode_uuid(fields[1].value);
        auto profile = decode_uuid(fields[2].value);
        auto sid = decode_utf8(fields[3].value, 256);
        if (!request.ok() || !operation.ok() || !profile.ok() || !sid.ok()) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_fields,
                L"operation.decode", L"Operation identity fields are invalid");
        }
        const std::string command(
            reinterpret_cast<const char*>(fields[4].value.data()),
            fields[4].value.size());
        const std::string phase(
            reinterpret_cast<const char*>(fields[7].value.data()),
            fields[7].value.size());
        std::array<std::byte, 32> digest{};
        std::copy(fields[5].value.begin(), fields[5].value.end(), digest.begin());
        const auto state_value =
            std::to_integer<std::uint8_t>(fields[6].value[0]);
        const auto cancellable =
            std::to_integer<std::uint8_t>(fields[11].value[0]);
        const auto cancel_requested =
            std::to_integer<std::uint8_t>(fields[12].value[0]);
        if (state_value > static_cast<std::uint8_t>(OperationState::cancelled)
            || cancellable > 1 || cancel_requested > 1) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_fields,
                L"operation.decode", L"Operation primitive value is invalid");
        }
        std::optional<std::uint64_t> terminal;
        if (!fields[10].value.empty()) {
            terminal = read_u64(fields[10].value, 0);
        }
        OperationRecord record{
            read_u64(bytes, 24),
            request.move_value(),
            operation.move_value(),
            profile.move_value(),
            sid.move_value(),
            command,
            digest,
            static_cast<OperationState>(state_value),
            phase,
            read_u64(fields[8].value, 0),
            read_u64(fields[9].value, 0),
            terminal,
            cancellable != 0,
            cancel_requested != 0,
            std::vector<std::byte>(
                fields[13].value.begin(), fields[13].value.end())};
        if (!valid_record(record)) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_record,
                L"operation.decode", L"Operation record invariants are invalid");
        }
        return Result<OperationRecord>::success(std::move(record));
    } catch (...) {
        return operation_error<OperationRecord>(
            OperationStoreError::io_failure,
            L"operation.decode", L"Operation decoding allocation failed");
    }
}

Result<std::vector<product::Uuid>> select_operation_retention_candidates(
    std::span<const OperationRecord> records,
    std::uint64_t now_unix_ms) {
    try {
        if (now_unix_ms == 0) {
            return operation_error<std::vector<product::Uuid>>(
                OperationStoreError::invalid_record,
                L"operation_store.retention",
                L"Retention timestamp is invalid");
        }
        std::vector<const OperationRecord*> terminal;
        terminal.reserve(records.size());
        for (const auto& record : records) {
            if (!is_terminal(record.state)) continue;
            if (!record.terminal_unix_ms.has_value()) {
                return operation_error<std::vector<product::Uuid>>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.retention",
                    L"Terminal operation lacks a terminal timestamp");
            }
            terminal.push_back(&record);
        }
        std::sort(terminal.begin(), terminal.end(), [](const auto* left,
                                                       const auto* right) {
            if (left->terminal_unix_ms != right->terminal_unix_ms) {
                return left->terminal_unix_ms < right->terminal_unix_ms;
            }
            return left->operation_id.to_string()
                < right->operation_id.to_string();
        });
        const auto cutoff = now_unix_ms >= kOperationRetentionMilliseconds
            ? now_unix_ms - kOperationRetentionMilliseconds : 0;
        std::size_t expired_count = 0;
        while (expired_count < terminal.size()
               && terminal[expired_count]->terminal_unix_ms.value() <= cutoff) {
            ++expired_count;
        }
        const auto remaining_after_expiry = terminal.size() - expired_count;
        const auto cap_removals = remaining_after_expiry
                > kOperationMaximumTerminalRecords
            ? remaining_after_expiry - kOperationMaximumTerminalRecords : 0;
        const auto removal_count = expired_count + cap_removals;
        std::vector<product::Uuid> selected;
        selected.reserve(removal_count);
        for (std::size_t index = 0; index < removal_count; ++index) {
            selected.push_back(terminal[index]->operation_id);
        }
        return Result<std::vector<product::Uuid>>::success(
            std::move(selected));
    } catch (...) {
        return operation_error<std::vector<product::Uuid>>(
            OperationStoreError::io_failure,
            L"operation_store.retention",
            L"Failed to allocate retention selection state");
    }
}

namespace {

std::wstring operation_id_text(const product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end());
}

std::wstring operation_current_leaf(const product::Uuid& value) {
    return operation_id_text(value) + L".myop";
}

std::wstring operation_previous_leaf(const product::Uuid& value) {
    return operation_id_text(value) + L".prev";
}

std::wstring request_index_key(const OperationRecord& record) {
    std::wstring key = record.caller_sid;
    key.push_back(L'\0');
    key.append(record.command_scope.begin(), record.command_scope.end());
    key.push_back(L'\0');
    const auto request = operation_id_text(record.request_id);
    key.append(request);
    return key;
}

std::wstring request_index_key(const OperationRequest& request) {
    std::wstring key = request.caller_sid;
    key.push_back(L'\0');
    key.append(request.command_scope.begin(), request.command_scope.end());
    key.push_back(L'\0');
    const auto request_id = operation_id_text(request.request_id);
    key.append(request_id);
    return key;
}

bool valid_request(const OperationRequest& request) {
    return !is_zero(request.request_id) && !is_zero(request.profile_id)
        && canonical_sid(request.caller_sid)
        && valid_command(request.command_scope)
        && request.created_unix_ms != 0
        && !std::all_of(
            request.argument_digest.begin(), request.argument_digest.end(),
            [](std::byte value) { return value == std::byte{0}; });
}

bool ends_with_ignore_case(std::wstring_view value,
                           std::wstring_view suffix) noexcept {
    if (value.size() < suffix.size()) return false;
    const auto tail = value.substr(value.size() - suffix.size());
    return std::equal(tail.begin(), tail.end(), suffix.begin(), suffix.end(),
                      [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
}

bool starts_with_ignore_case(std::wstring_view value,
                             std::wstring_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    const auto head = value.substr(0, prefix.size());
    return std::equal(head.begin(), head.end(), prefix.begin(), prefix.end(),
        [](wchar_t left, wchar_t right) {
            return std::towlower(left) == std::towlower(right);
        });
}

bool is_operation_temp_like(std::wstring_view name) noexcept {
    return starts_with_ignore_case(name, L".next.")
        || starts_with_ignore_case(name, L".previous.");
}

bool is_canonical_operation_temp(std::wstring_view name) {
    std::wstring_view id_text_wide;
    if (name.starts_with(L".next.") && name.ends_with(L".tmp")
        && name.size() == 46) {
        id_text_wide = name.substr(6, 36);
    } else if (name.starts_with(L".previous.") && name.ends_with(L".tmp")
               && name.size() == 50) {
        id_text_wide = name.substr(10, 36);
    } else {
        return false;
    }
    if (!std::all_of(id_text_wide.begin(), id_text_wide.end(), [](wchar_t value) {
            return static_cast<unsigned int>(value) <= 0x7fU;
        })) {
        return false;
    }
    std::string id_text;
    id_text.reserve(id_text_wide.size());
    for (const auto value : id_text_wide) {
        id_text.push_back(static_cast<char>(value));
    }
    auto id = product::Uuid::parse(id_text);
    return id.ok() && id.value().to_string() == id_text;
}

bool same_immutable_request(const OperationRecord& current,
                            const OperationRecord& proposed) noexcept {
    return current.request_id == proposed.request_id
        && current.operation_id == proposed.operation_id
        && current.profile_id == proposed.profile_id
        && current.caller_sid == proposed.caller_sid
        && current.command_scope == proposed.command_scope
        && current.argument_digest == proposed.argument_digest
        && current.created_unix_ms == proposed.created_unix_ms;
}

bool valid_state_transition(OperationState current,
                            OperationState proposed) noexcept {
    if (is_terminal(current)) return false;
    if (current == proposed) return true;
    if (is_terminal(proposed)) return true;
    if (current == OperationState::queued) {
        return proposed == OperationState::running;
    }
    if (current == OperationState::running) {
        return proposed == OperationState::reconciling;
    }
    return current == OperationState::reconciling
        && proposed == OperationState::running;
}

bool valid_initial_record(const OperationRecord& record) noexcept {
    return record.revision == 1 && record.state == OperationState::queued
        && record.phase == "queued"
        && record.created_unix_ms == record.updated_unix_ms
        && !record.terminal_unix_ms.has_value()
        && record.cancellable && !record.cancel_requested
        && record.terminal_result.empty();
}

bool valid_update(const OperationRecord& current,
                  const OperationRecord& proposed) noexcept {
    if (current.revision == (std::numeric_limits<std::uint64_t>::max)()
        || proposed.revision != current.revision + 1
        || proposed.updated_unix_ms < current.updated_unix_ms
        || !same_immutable_request(current, proposed)
        || !valid_state_transition(current.state, proposed.state)
        || (!current.cancellable && proposed.cancellable)
        || (current.cancel_requested && !proposed.cancel_requested)
        || (!current.cancel_requested && proposed.cancel_requested
            && !current.cancellable)) {
        return false;
    }
    return true;
}

Result<void> persistence_error(const Error& source) {
    const bool indeterminate =
        source.code == static_cast<std::uint32_t>(
            profile::ProfileStoreError::commit_indeterminate);
    return Result<void>::failure(Error{
        source.domain,
        static_cast<std::uint32_t>(
            indeterminate
                ? OperationStoreError::commit_indeterminate
                : OperationStoreError::io_failure),
        L"operation_store.persist",
        indeterminate
            ? L"Operation record publication may already be visible"
            : L"Operation record persistence failed",
        source.native_code});
}

std::shared_ptr<std::shared_mutex> shared_operation_gate(std::wstring key) {
    static std::mutex registry_mutex;
    static std::map<std::wstring, std::weak_ptr<std::shared_mutex>, std::less<>> registry;
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    std::scoped_lock lock(registry_mutex);
    if (const auto found = registry.find(key); found != registry.end()) {
        if (auto existing = found->second.lock()) return existing;
        registry.erase(found);
    }
    auto created = std::make_shared<std::shared_mutex>();
    registry.emplace(std::move(key), created);
    for (auto iterator = registry.begin(); iterator != registry.end();) {
        if (iterator->second.expired()) {
            iterator = registry.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return created;
}

} // namespace

struct OperationStore::Impl final {
    Impl(std::unique_ptr<profile::IAtomicFileDirectory> directory_value,
         OperationStorePaths paths_value,
         std::shared_ptr<std::shared_mutex> gate_value)
        : directory(std::move(directory_value)),
          paths(std::move(paths_value)),
          gate(std::move(gate_value)) {}

    std::unique_ptr<profile::IAtomicFileDirectory> directory;
    OperationStorePaths paths;
    std::shared_ptr<std::shared_mutex> gate;
    std::map<std::wstring, product::Uuid, std::less<>> request_index;
};

OperationStore::OperationStore(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
OperationStore::OperationStore(OperationStore&&) noexcept = default;
OperationStore& OperationStore::operator=(OperationStore&&) noexcept = default;
OperationStore::~OperationStore() noexcept = default;

Result<OperationStore> OperationStore::open_existing(
    std::wstring_view trusted_program_data_root) {
    try {
        const std::array<std::wstring_view, 2> components{
            L"MyRamDisk", L"operations"};
        auto managed = profile::open_managed_atomic_directory(
            trusted_program_data_root, components);
        if (!managed.ok()) {
            return Result<OperationStore>::failure(managed.error());
        }
        auto path = managed.value().path;
        auto gate_key = path;
        return from_directory_capability(
            std::move(managed.value().directory),
            OperationStorePaths{std::move(path)}, std::move(gate_key));
    } catch (...) {
        return operation_error<OperationStore>(
            OperationStoreError::io_failure,
            L"operation_store.open", L"Failed to allocate operation store state");
    }
}

Result<OperationStore> OperationStore::from_directory_capability(
    std::unique_ptr<profile::IAtomicFileDirectory> directory,
    OperationStorePaths paths,
    std::wstring gate_key) {
    try {
        if (!directory || gate_key.empty()
            || gate_key.find(L'\0') != std::wstring::npos) {
            return operation_error<OperationStore>(
                OperationStoreError::invalid_record,
                L"operation_store.open", L"Operation capability is invalid");
        }
        auto gate = shared_operation_gate(std::move(gate_key));
        auto impl = std::make_unique<Impl>(
            std::move(directory), std::move(paths), std::move(gate));
        OperationStore store(std::move(impl));
        {
            std::scoped_lock lock(*store.impl_->gate);
            auto cleaned = store.cleanup_orphan_temps_locked();
            if (!cleaned.ok()) {
                return Result<OperationStore>::failure(cleaned.error());
            }
            auto rebuilt = store.rebuild_index_locked();
            if (!rebuilt.ok()) {
                return Result<OperationStore>::failure(rebuilt.error());
            }
        }
        return Result<OperationStore>::success(std::move(store));
    } catch (...) {
        return operation_error<OperationStore>(
            OperationStoreError::io_failure,
            L"operation_store.open", L"Failed to allocate operation store state");
    }
}

Result<void> OperationStore::cleanup_orphan_temps_locked() {
    bool deletion_attempted = false;
    auto names = impl_->directory->list_files();
    if (!names.ok()) {
        return operation_error<void>(
            OperationStoreError::io_failure,
            L"operation_store.open",
            L"Failed to enumerate operation metadata during temp cleanup");
    }

    std::vector<std::wstring> orphan_temps;
    try {
        for (const auto& name : names.value()) {
            if (!is_operation_temp_like(name)) continue;
            if (!is_canonical_operation_temp(name)) {
                return operation_error<void>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.open",
                    L"Operation temp filename is noncanonical");
            }
            orphan_temps.push_back(name);
        }

        // Validate every candidate before deleting any candidate. read_file()
        // rejects directories, reparse points, hard links, and oversized files.
        for (const auto& name : orphan_temps) {
            auto verified = impl_->directory->read_file(name);
            if (!verified.ok()) {
                return operation_error<void>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.open",
                    L"Operation temp file failed safe reopen validation");
            }
        }
        for (const auto& name : orphan_temps) {
            deletion_attempted = true;
            auto removed = impl_->directory->remove_file(name);
            if (!removed.ok()) {
                return operation_error<void>(
                    OperationStoreError::commit_indeterminate,
                    L"operation_store.open",
                    L"Operation temp deletion outcome is indeterminate");
            }
        }
        if (!orphan_temps.empty()) {
            auto flushed = impl_->directory->flush_directory();
            if (!flushed.ok()) {
                return operation_error<void>(
                    OperationStoreError::commit_indeterminate,
                    L"operation_store.open",
                    L"Operation temp cleanup flush is indeterminate");
            }
        }
        return Result<void>::success();
    } catch (...) {
        return operation_error<void>(
            deletion_attempted
                ? OperationStoreError::commit_indeterminate
                : OperationStoreError::io_failure,
            L"operation_store.open",
            deletion_attempted
                ? L"Operation temp cleanup outcome is indeterminate"
                : L"Failed to allocate operation temp cleanup state");
    }
}

const OperationStorePaths& OperationStore::paths() const noexcept {
    return impl_->paths;
}

Result<OperationRecord> OperationStore::load_record(
    const product::Uuid& operation_id) {
    try {
        // A query: shared, so it does not queue behind another query and only waits for a writer that is
        // actually in progress.  See `shared_operation_gate` for why the gate is a reader-writer one.
        std::shared_lock lock(*impl_->gate);
        return load_record_locked(operation_id);
    } catch (...) {
        return operation_error<OperationRecord>(
            OperationStoreError::io_failure,
            L"operation_store.load", L"Failed to allocate operation load state");
    }
}

Result<OperationRecord> OperationStore::load_record_locked(
    const product::Uuid& operation_id) {
    try {
        if (is_zero(operation_id)) {
            return operation_error<OperationRecord>(
                OperationStoreError::operation_not_found,
                L"operation_store.load", L"Operation identifier is invalid");
        }
        auto bytes = impl_->directory->read_file(
            operation_current_leaf(operation_id));
        if (!bytes.ok()) {
            if (bytes.error().code == static_cast<std::uint32_t>(
                    profile::ProfileStoreError::path_not_found)) {
                return operation_error<OperationRecord>(
                    OperationStoreError::operation_not_found,
                    L"operation_store.load", L"Operation record was not found");
            }
            return operation_error<OperationRecord>(
                OperationStoreError::storage_corrupt,
                L"operation_store.load", L"Operation storage is unsafe or unreadable");
        }
        auto decoded = decode_operation_record(bytes.value());
        if (!decoded.ok() || decoded.value().operation_id != operation_id) {
            return operation_error<OperationRecord>(
                OperationStoreError::storage_corrupt,
                L"operation_store.load", L"Operation record is corrupt or misbound");
        }
        return decoded;
    } catch (...) {
        return operation_error<OperationRecord>(
            OperationStoreError::io_failure,
            L"operation_store.load", L"Failed to allocate operation load state");
    }
}

Result<void> OperationStore::write_record(
    std::uint64_t expected_revision,
    const OperationRecord& proposed) {
    try {
        myrd::diagnostic_trace("opstore.write: waiting for gate");
        std::scoped_lock lock(*impl_->gate);
        myrd::diagnostic_trace("opstore.write: gate held");
        auto written = write_record_locked(expected_revision, proposed);
        myrd::diagnostic_trace("opstore.write: done");
        return written;
    } catch (...) {
        return operation_error<void>(
            OperationStoreError::io_failure,
            L"operation_store.write", L"Operation write failed internally");
    }
}

Result<void> OperationStore::write_record_locked(
    std::uint64_t expected_revision,
    const OperationRecord& proposed) {
    bool publication_succeeded = false;
    try {
        const auto current_leaf = operation_current_leaf(proposed.operation_id);
        const auto previous_leaf = operation_previous_leaf(proposed.operation_id);
        auto current_bytes = impl_->directory->read_file(current_leaf);
        const bool current_exists = current_bytes.ok();
        if (!current_exists
            && current_bytes.error().code != static_cast<std::uint32_t>(
                profile::ProfileStoreError::path_not_found)) {
            return persistence_error(current_bytes.error());
        }

        if (!current_exists) {
            if (expected_revision != 0 || !valid_initial_record(proposed)) {
                return operation_error<void>(
                    OperationStoreError::revision_conflict,
                    L"operation_store.write", L"Initial operation revision is invalid");
            }
            auto refreshed = rebuild_index_locked();
            if (!refreshed.ok()) return refreshed;
            const auto key = request_index_key(proposed);
            if (const auto found = impl_->request_index.find(key);
                found != impl_->request_index.end()
                && found->second != proposed.operation_id) {
                return operation_error<void>(
                    OperationStoreError::request_id_conflict,
                    L"operation_store.write", L"Request identifier is already in use");
            }
        } else {
            auto current = decode_operation_record(current_bytes.value());
            if (!current.ok()
                || current.value().operation_id != proposed.operation_id) {
                return operation_error<void>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.write", L"Current operation record is corrupt");
            }
            if (current.value().revision != expected_revision) {
                return operation_error<void>(
                    OperationStoreError::revision_conflict,
                    L"operation_store.write", L"Expected operation revision is stale");
            }
            if (!valid_update(current.value(), proposed)) {
                return operation_error<void>(
                    OperationStoreError::invalid_record,
                    L"operation_store.write", L"Operation update is invalid");
            }
        }

        auto encoded = encode_operation_record(proposed);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto round_trip = decode_operation_record(encoded.value());
        if (!round_trip.ok() || round_trip.value() != proposed) {
            return operation_error<void>(
                OperationStoreError::invalid_record,
                L"operation_store.write", L"Operation round trip failed");
        }
        auto published = profile::publish_atomically_with_previous(
            *impl_->directory, current_leaf, previous_leaf, encoded.value());
        if (!published.ok()) {
            auto mapped = persistence_error(published.error());
            if (mapped.error().code == static_cast<std::uint32_t>(
                    OperationStoreError::commit_indeterminate)) {
                static_cast<void>(rebuild_index_locked());
            }
            return mapped;
        }
        publication_succeeded = true;
        impl_->request_index.insert_or_assign(
            request_index_key(proposed), proposed.operation_id);
        return Result<void>::success();
    } catch (...) {
        return operation_error<void>(
            publication_succeeded
                ? OperationStoreError::commit_indeterminate
                : OperationStoreError::io_failure,
            L"operation_store.write",
            publication_succeeded
                ? L"Operation record is durable but index update failed"
                : L"Operation write failed internally");
    }
}

Result<std::vector<OperationRecord>> OperationStore::list_records_locked() {
    try {
        auto names = impl_->directory->list_files();
        if (!names.ok()) {
            return operation_error<std::vector<OperationRecord>>(
                OperationStoreError::storage_corrupt,
                L"operation_store.list", L"Operation directory cannot be enumerated");
        }
        std::vector<OperationRecord> records;
        for (const auto& name : names.value()) {
            if (!name.ends_with(L".myop")) {
                if (ends_with_ignore_case(name, L".myop")) {
                    return operation_error<std::vector<OperationRecord>>(
                        OperationStoreError::storage_corrupt,
                        L"operation_store.list",
                        L"Operation filename casing is noncanonical");
                }
                continue;
            }
            if (name.size() != 41) {
                return operation_error<std::vector<OperationRecord>>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.list", L"Operation filename is invalid");
            }
            const auto id_text_wide = std::wstring_view(name).substr(0, 36);
            if (!std::all_of(
                    id_text_wide.begin(), id_text_wide.end(),
                    [](wchar_t value) { return value >= 0 && value <= 0x7f; })) {
                return operation_error<std::vector<OperationRecord>>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.list", L"Operation filename is noncanonical");
            }
            std::string id_text;
            id_text.reserve(id_text_wide.size());
            for (const auto value : id_text_wide) {
                id_text.push_back(static_cast<char>(value));
            }
            auto id = product::Uuid::parse(id_text);
            if (!id.ok() || id.value().to_string() != id_text) {
                return operation_error<std::vector<OperationRecord>>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.list", L"Operation filename UUID is invalid");
            }
            auto record = load_record_locked(id.value());
            if (!record.ok()) {
                return Result<std::vector<OperationRecord>>::failure(
                    record.error());
            }
            records.push_back(record.move_value());
        }
        std::sort(records.begin(), records.end(), [](const auto& left,
                                                     const auto& right) {
            if (left.created_unix_ms != right.created_unix_ms) {
                return left.created_unix_ms < right.created_unix_ms;
            }
            return left.operation_id.to_string()
                < right.operation_id.to_string();
        });
        return Result<std::vector<OperationRecord>>::success(
            std::move(records));
    } catch (...) {
        return operation_error<std::vector<OperationRecord>>(
            OperationStoreError::io_failure,
            L"operation_store.list", L"Failed to allocate operation list state");
    }
}

Result<std::vector<OperationRecord>> OperationStore::list_records() {
    try {
        // **The query the read/write separation requirement is really about.**
        //
        // Listing has to answer in about a second even while a create is in progress, so it must not hold the
        // gate the way a mutation does.  Taken shared, it proceeds alongside other queries and waits only for
        // a writer that is mid-record, which is bounded by construction.
        std::shared_lock lock(*impl_->gate);
        return list_records_locked();
    } catch (...) {
        return operation_error<std::vector<OperationRecord>>(
            OperationStoreError::io_failure,
            L"operation_store.list", L"Failed to lock operation list state");
    }
}

Result<void> OperationStore::rebuild_index_locked() {
    auto records = list_records_locked();
    if (!records.ok()) return Result<void>::failure(records.error());
    std::map<std::wstring, product::Uuid, std::less<>> replacement;
    try {
        for (const auto& record : records.value()) {
            const auto [unused, inserted] = replacement.emplace(
                request_index_key(record), record.operation_id);
            static_cast<void>(unused);
            if (!inserted) {
                return operation_error<void>(
                    OperationStoreError::storage_corrupt,
                    L"operation_store.index",
                    L"Durable operation request key is duplicated");
            }
        }
        impl_->request_index = std::move(replacement);
        return Result<void>::success();
    } catch (...) {
        return operation_error<void>(
            OperationStoreError::io_failure,
            L"operation_store.index", L"Failed to allocate request index");
    }
}

Result<std::optional<OperationRecord>> OperationStore::find_request_locked(
    const OperationRequest& request) {
    const auto key = request_index_key(request);
    auto found = impl_->request_index.find(key);
    if (found == impl_->request_index.end()) {
        auto rebuilt = rebuild_index_locked();
        if (!rebuilt.ok()) {
            return Result<std::optional<OperationRecord>>::failure(
                rebuilt.error());
        }
        found = impl_->request_index.find(key);
    }
    if (found == impl_->request_index.end()) {
        return Result<std::optional<OperationRecord>>::success(std::nullopt);
    }
    auto record = load_record_locked(found->second);
    if (!record.ok()
        && record.error().code == static_cast<std::uint32_t>(
            OperationStoreError::operation_not_found)) {
        auto rebuilt = rebuild_index_locked();
        if (!rebuilt.ok()) {
            return Result<std::optional<OperationRecord>>::failure(
                rebuilt.error());
        }
        found = impl_->request_index.find(key);
        if (found == impl_->request_index.end()) {
            return Result<std::optional<OperationRecord>>::success(
                std::nullopt);
        }
        record = load_record_locked(found->second);
    }
    if (!record.ok()) {
        if (record.error().code == static_cast<std::uint32_t>(
                OperationStoreError::operation_not_found)) {
            // The index entry pointed at a record that no longer exists, so
            // the request owns nothing and may be admitted again.
            return Result<std::optional<OperationRecord>>::success(
                std::nullopt);
        }
        return Result<std::optional<OperationRecord>>::failure(
            record.error());
    }
    if (record.value().profile_id != request.profile_id
        || record.value().argument_digest != request.argument_digest) {
        return operation_error<std::optional<OperationRecord>>(
            OperationStoreError::request_id_conflict,
            L"operation_store.find_request",
            L"Request identifier was reused with different arguments");
    }
    return Result<std::optional<OperationRecord>>::success(
        std::optional<OperationRecord>(record.move_value()));
}

Result<std::optional<OperationRecord>> OperationStore::find_request(
    const OperationRequest& request) {
    try {
        if (!valid_request(request)) {
            return operation_error<std::optional<OperationRecord>>(
                OperationStoreError::invalid_record,
                L"operation_store.find_request",
                L"Operation request is invalid");
        }
        // A query like `list_records`, and shared for the same reason: idempotency lookups must not queue
        // behind a mutation that happens to be in progress on the same profile.
        std::shared_lock lock(*impl_->gate);
        return find_request_locked(request);
    } catch (...) {
        return operation_error<std::optional<OperationRecord>>(
            OperationStoreError::io_failure,
            L"operation_store.find_request",
            L"Operation request lookup failed internally");
    }
}

Result<OperationAdmission> OperationStore::admit_with_id(
    const OperationRequest& request,
    const product::Uuid& candidate_operation_id) {
    try {
        if (!valid_request(request)) {
            return operation_error<OperationAdmission>(
                OperationStoreError::invalid_record,
                L"operation_store.admit_with_id",
                L"Operation request is invalid");
        }
        if (is_zero(candidate_operation_id)) {
            return operation_error<OperationAdmission>(
                OperationStoreError::invalid_record,
                L"operation_store.admit_with_id",
                L"Operation identifier candidate is invalid");
        }
        std::scoped_lock lock(*impl_->gate);
        auto existing = find_request_locked(request);
        if (!existing.ok()) {
            return Result<OperationAdmission>::failure(existing.error());
        }
        if (existing.value().has_value()) {
            return Result<OperationAdmission>::success(OperationAdmission{
                existing.move_value().value(), true});
        }
        auto collision = load_record_locked(candidate_operation_id);
        if (collision.ok()) {
            return operation_error<OperationAdmission>(
                OperationStoreError::operation_id_conflict,
                L"operation_store.admit_with_id",
                L"Operation identifier candidate is already in use");
        }
        if (collision.error().code != static_cast<std::uint32_t>(
                OperationStoreError::operation_not_found)) {
            return Result<OperationAdmission>::failure(collision.error());
        }
        OperationRecord record{
            1,
            request.request_id,
            candidate_operation_id,
            request.profile_id,
            request.caller_sid,
            request.command_scope,
            request.argument_digest,
            OperationState::queued,
            "queued",
            request.created_unix_ms,
            request.created_unix_ms,
            std::nullopt,
            true,
            false,
            {}};
        auto written = write_record_locked(0, record);
        if (!written.ok()) {
            return Result<OperationAdmission>::failure(written.error());
        }
        return Result<OperationAdmission>::success(OperationAdmission{
            std::move(record), false});
    } catch (...) {
        return operation_error<OperationAdmission>(
            OperationStoreError::io_failure,
            L"operation_store.admit_with_id",
            L"Operation admission failed internally");
    }
}

Result<OperationAdmission> OperationStore::admit(
    const OperationRequest& request) {
    for (unsigned attempt = 0; attempt != 8; ++attempt) {
        auto operation_id = product::Uuid::random();
        if (!operation_id.ok()) {
            return Result<OperationAdmission>::failure(operation_id.error());
        }
        auto admission = admit_with_id(request, operation_id.value());
        if (admission.ok()) return admission;
        if (admission.error().code != static_cast<std::uint32_t>(
                OperationStoreError::operation_id_conflict)) {
            return admission;
        }
    }
    return operation_error<OperationAdmission>(
        OperationStoreError::io_failure,
        L"operation_store.admit",
        L"Failed to allocate a unique operation identifier");
}

Result<std::vector<OperationRecord>> OperationStore::reconcile_after_restart(
    std::uint64_t now_unix_ms) {
    bool persisted_any_reconciliation = false;
    try {
        if (now_unix_ms == 0) {
            return operation_error<std::vector<OperationRecord>>(
                OperationStoreError::invalid_record,
                L"operation_store.reconcile",
                L"Restart reconciliation timestamp is invalid");
        }
        std::scoped_lock lock(*impl_->gate);
        auto records = list_records_locked();
        if (!records.ok()) return records;
        for (const auto& current : records.value()) {
            if (current.state != OperationState::running) continue;
            if (current.revision
                == (std::numeric_limits<std::uint64_t>::max)()) {
                if (persisted_any_reconciliation) {
                    return operation_error<std::vector<OperationRecord>>(
                        OperationStoreError::commit_indeterminate,
                        L"operation_store.reconcile",
                        L"Restart reconciliation outcome is partially committed");
                }
                return operation_error<std::vector<OperationRecord>>(
                    OperationStoreError::invalid_record,
                    L"operation_store.reconcile",
                    L"Operation revision is exhausted");
            }
            auto proposed = current;
            ++proposed.revision;
            proposed.state = OperationState::reconciling;
            // Preserve the last durable phase. Recovery needs the exact
            // boundary marker to distinguish a legitimate operation-store-
            // first crash from a forged non-cancellable pre-boundary record.
            proposed.updated_unix_ms = (std::max)(
                current.updated_unix_ms, now_unix_ms);
            auto written = write_record_locked(current.revision, proposed);
            if (!written.ok()) {
                if (persisted_any_reconciliation
                    && written.error().code != static_cast<std::uint32_t>(
                        OperationStoreError::commit_indeterminate)) {
                    return operation_error<std::vector<OperationRecord>>(
                        OperationStoreError::commit_indeterminate,
                        L"operation_store.reconcile",
                        L"Restart reconciliation outcome is partially committed");
                }
                return Result<std::vector<OperationRecord>>::failure(
                    written.error());
            }
            persisted_any_reconciliation = true;
        }
        auto refreshed = list_records_locked();
        if (!refreshed.ok() && persisted_any_reconciliation) {
            return operation_error<std::vector<OperationRecord>>(
                OperationStoreError::commit_indeterminate,
                L"operation_store.reconcile",
                L"Restart reconciliation committed but refresh failed");
        }
        return refreshed;
    } catch (...) {
        return operation_error<std::vector<OperationRecord>>(
            persisted_any_reconciliation
                ? OperationStoreError::commit_indeterminate
                : OperationStoreError::io_failure,
            L"operation_store.reconcile",
            persisted_any_reconciliation
                ? L"Restart reconciliation outcome is partially committed"
                : L"Restart reconciliation failed internally");
    }
}

Result<OperationRecord> OperationStore::request_cancel(
    const product::Uuid& operation_id,
    std::uint64_t now_unix_ms) {
    try {
        if (now_unix_ms == 0) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_record,
                L"operation_store.cancel",
                L"Cancellation timestamp is invalid");
        }
        std::scoped_lock lock(*impl_->gate);
        auto current = load_record_locked(operation_id);
        if (!current.ok()) return current;
        if (current.value().cancel_requested) return current;
        if (is_terminal(current.value().state)
            || !current.value().cancellable) {
            return operation_error<OperationRecord>(
                OperationStoreError::cancellation_too_late,
                L"operation_store.cancel",
                L"Operation has crossed its cancellable boundary");
        }
        if (current.value().revision
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return operation_error<OperationRecord>(
                OperationStoreError::invalid_record,
                L"operation_store.cancel",
                L"Operation revision is exhausted");
        }
        auto proposed = current.value();
        ++proposed.revision;
        proposed.cancel_requested = true;
        proposed.updated_unix_ms = (std::max)(
            current.value().updated_unix_ms, now_unix_ms);
        auto written = write_record_locked(current.value().revision, proposed);
        if (!written.ok()) {
            return Result<OperationRecord>::failure(written.error());
        }
        return Result<OperationRecord>::success(std::move(proposed));
    } catch (...) {
        return operation_error<OperationRecord>(
            OperationStoreError::io_failure,
            L"operation_store.cancel",
            L"Cancellation persistence failed internally");
    }
}

Result<OperationRetentionResult> OperationStore::cleanup_terminal_records(
    std::uint64_t now_unix_ms) {
    bool deletion_attempted = false;
    try {
        std::scoped_lock lock(*impl_->gate);
        auto records = list_records_locked();
        if (!records.ok()) {
            return Result<OperationRetentionResult>::failure(records.error());
        }
        auto selected = select_operation_retention_candidates(
            records.value(), now_unix_ms);
        if (!selected.ok()) {
            return Result<OperationRetentionResult>::failure(selected.error());
        }
        const auto terminal_count = static_cast<std::size_t>(std::count_if(
            records.value().begin(), records.value().end(),
            [](const auto& record) { return is_terminal(record.state); }));

        std::vector<std::wstring> orphan_previous;
        auto names = impl_->directory->list_files();
        if (!names.ok()) {
            return operation_error<OperationRetentionResult>(
                OperationStoreError::retention_blocked,
                L"operation_store.cleanup",
                L"Cleanup cannot enumerate operation metadata");
        }
        for (const auto& name : names.value()) {
            if (!ends_with_ignore_case(name, L".prev")) continue;
            if (!name.ends_with(L".prev") || name.size() != 41) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup encountered a noncanonical previous filename");
            }
            const auto wide_id = std::wstring_view(name).substr(0, 36);
            if (!std::all_of(
                    wide_id.begin(), wide_id.end(),
                    [](wchar_t value) { return value >= 0 && value <= 0x7f; })) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup previous filename is noncanonical");
            }
            std::string id_text;
            id_text.reserve(wide_id.size());
            for (const auto value : wide_id) {
                id_text.push_back(static_cast<char>(value));
            }
            auto id = product::Uuid::parse(id_text);
            if (!id.ok() || id.value().to_string() != id_text) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup previous filename UUID is invalid");
            }
            auto current = impl_->directory->read_file(
                operation_current_leaf(id.value()));
            if (current.ok()) continue;
            if (current.error().code != static_cast<std::uint32_t>(
                    profile::ProfileStoreError::path_not_found)) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup current metadata is unsafe");
            }
            auto previous = impl_->directory->read_file(name);
            if (!previous.ok()) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup orphan previous metadata is unsafe");
            }
            orphan_previous.push_back(name);
        }

        if (selected.value().empty() && orphan_previous.empty()) {
            return Result<OperationRetentionResult>::success(
                OperationRetentionResult{{}, terminal_count});
        }

        struct Candidate final {
            product::Uuid id;
            std::wstring current;
            std::wstring previous;
            bool previous_exists{};
        };
        std::vector<Candidate> candidates;
        candidates.reserve(selected.value().size());
        for (const auto& id : selected.value()) {
            auto current_name = operation_current_leaf(id);
            auto current_bytes = impl_->directory->read_file(current_name);
            if (!current_bytes.ok()) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup cannot safely reopen current operation metadata");
            }
            auto current = decode_operation_record(current_bytes.value());
            if (!current.ok() || current.value().operation_id != id
                || !is_terminal(current.value().state)) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup candidate changed during selection");
            }
            auto previous_name = operation_previous_leaf(id);
            auto previous = impl_->directory->read_file(previous_name);
            bool previous_exists = previous.ok();
            if (!previous_exists
                && previous.error().code != static_cast<std::uint32_t>(
                    profile::ProfileStoreError::path_not_found)) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::retention_blocked,
                    L"operation_store.cleanup",
                    L"Cleanup previous metadata is unsafe");
            }
            candidates.push_back(Candidate{
                id, std::move(current_name), std::move(previous_name),
                previous_exists});
        }

        for (const auto& candidate : candidates) {
            deletion_attempted = true;
            auto removed_current = impl_->directory->remove_file(
                candidate.current);
            if (!removed_current.ok()) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::commit_indeterminate,
                    L"operation_store.cleanup",
                    L"Cleanup deletion outcome is indeterminate");
            }
            if (candidate.previous_exists) {
                deletion_attempted = true;
                auto removed_previous = impl_->directory->remove_file(
                    candidate.previous);
                if (!removed_previous.ok()) {
                    return operation_error<OperationRetentionResult>(
                        OperationStoreError::commit_indeterminate,
                        L"operation_store.cleanup",
                        L"Cleanup outcome is partially committed");
                }
            }
        }
        for (const auto& name : orphan_previous) {
            deletion_attempted = true;
            auto removed = impl_->directory->remove_file(name);
            if (!removed.ok()) {
                return operation_error<OperationRetentionResult>(
                    OperationStoreError::commit_indeterminate,
                    L"operation_store.cleanup",
                    L"Cleanup orphan deletion outcome is indeterminate");
            }
        }
        auto flushed = impl_->directory->flush_directory();
        if (!flushed.ok()) {
            return operation_error<OperationRetentionResult>(
                OperationStoreError::commit_indeterminate,
                L"operation_store.cleanup",
                L"Cleanup directory flush is indeterminate");
        }
        auto rebuilt = rebuild_index_locked();
        if (!rebuilt.ok()) {
            return operation_error<OperationRetentionResult>(
                OperationStoreError::commit_indeterminate,
                L"operation_store.cleanup",
                L"Cleanup completed but request index rebuild failed");
        }
        return Result<OperationRetentionResult>::success(
            OperationRetentionResult{
                selected.move_value(),
                terminal_count - candidates.size()});
    } catch (...) {
        return operation_error<OperationRetentionResult>(
            deletion_attempted
                ? OperationStoreError::commit_indeterminate
                : OperationStoreError::io_failure,
            L"operation_store.cleanup",
            deletion_attempted
                ? L"Operation cleanup outcome is indeterminate"
                : L"Operation cleanup failed internally");
    }
}

} // namespace myrd::management
