#include "src/product/json_writer.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

namespace myrd::product {

namespace {

constexpr std::uint64_t kJavascriptMaxSafeInteger = 9007199254740991ULL;

template <class>
inline constexpr bool kUnsupportedJsonScalar = false;

Result<std::string> json_error(const wchar_t* message,
                               std::uint32_t native_code = 0) {
    return Result<std::string>::failure(Error{
        ErrorDomain::protocol, 1, L"json_writer", message, native_code});
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
            code_point = lead & 0x1fU;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            code_point = lead & 0x0fU;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const std::uint32_t minimum = continuation_count == 1 ? 0x80U
            : continuation_count == 2 ? 0x800U : 0x10000U;
        if (code_point < minimum || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

bool valid_text(std::string_view text) noexcept {
    return is_valid_utf8(text);
}

std::string escape_json_string_unchecked(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20U) {
                escaped += "\\u00";
                escaped.push_back(hex[(ch >> 4U) & 0x0fU]);
                escaped.push_back(hex[ch & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}

std::string quote_json_string(std::string_view value) {
    return "\"" + escape_json_string_unchecked(value) + "\"";
}

std::string render_scalar(const JsonScalar& scalar) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return quote_json_string(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            return render_uint64_as_json(value);
        } else {
            static_assert(kUnsupportedJsonScalar<T>,
                          "JSON scalar rendering is incomplete");
            return {};
        }
    }, scalar);
}

bool valid_result(const JsonObject& result) noexcept {
    return std::all_of(result.begin(), result.end(), [](const JsonField& field) {
        if (!valid_text(field.name)) {
            return false;
        }
        if (const auto* text = std::get_if<std::string>(&field.value)) {
            return valid_text(*text);
        }
        return true;
    });
}

} // namespace

Result<std::string> escape_json_string(std::string_view value) {
    if (!valid_text(value)) {
        return json_error(L"JSON string is not valid UTF-8");
    }
    return Result<std::string>::success(escape_json_string_unchecked(value));
}

Result<std::string> json_from_utf16(std::wstring_view value) {
    if (value.empty()) {
        return Result<std::string>::success({});
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return json_error(L"UTF-16 input is too large");
    }
    const int input_size = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return json_error(L"Invalid UTF-16 input", ::GetLastError());
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
        utf8.data(), required, nullptr, nullptr);
    if (written != required) {
        return json_error(L"UTF-16 conversion failed", ::GetLastError());
    }
    return Result<std::string>::success(std::move(utf8));
}

std::string format_rfc3339_utc(std::chrono::system_clock::time_point value) {
    using namespace std::chrono;
    const auto seconds_value = floor<seconds>(value);
    const auto day = floor<days>(seconds_value);
    const year_month_day calendar{day};
    const hh_mm_ss time{seconds_value - day};
    char formatted[32]{};
    std::snprintf(formatted, sizeof(formatted),
                  "%04d-%02u-%02uT%02u:%02u:%02uZ",
                  static_cast<int>(calendar.year()),
                  static_cast<unsigned>(calendar.month()),
                  static_cast<unsigned>(calendar.day()),
                  static_cast<unsigned>(time.hours().count()),
                  static_cast<unsigned>(time.minutes().count()),
                  static_cast<unsigned>(time.seconds().count()));
    return formatted;
}

std::string render_uint64_as_json(std::uint64_t value) {
    const std::string decimal = std::to_string(value);
    return value <= kJavascriptMaxSafeInteger
        ? decimal : quote_json_string(decimal);
}

bool validate_schema(std::string_view schema) noexcept {
    return schema == "myrd.cli/v1";
}

bool validate_envelope_header(
    std::string_view schema,
    std::span<const std::string_view> unknown_additive_fields) noexcept {
    (void)unknown_additive_fields;
    return validate_schema(schema);
}

Result<std::string> write_success_envelope(const SuccessEnvelope& envelope) {
    if (!validate_schema(envelope.schema) || !valid_text(envelope.request_id)
        || !valid_text(envelope.command) || !valid_result(envelope.result)
        || !std::all_of(envelope.warnings.begin(), envelope.warnings.end(),
                        [](const std::string& warning) { return valid_text(warning); })) {
        return json_error(L"Success envelope contains an invalid schema or UTF-8 field");
    }
    std::string result = "{\n  \"schema\": " + quote_json_string(envelope.schema)
        + ",\n  \"ok\": true,\n  \"request_id\": " + quote_json_string(envelope.request_id)
        + ",\n  \"command\": " + quote_json_string(envelope.command)
        + ",\n  \"result\": {";
    for (std::size_t index = 0; index < envelope.result.size(); ++index) {
        const auto& field = envelope.result[index];
        result += index == 0 ? "\n" : ",\n";
        result += "    " + quote_json_string(field.name) + ": "
            + render_scalar(field.value);
    }
    if (!envelope.result.empty()) {
        result += "\n  ";
    }
    result += "},\n  \"warnings\": [";
    for (std::size_t index = 0; index < envelope.warnings.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        result += quote_json_string(envelope.warnings[index]);
    }
    result += "]\n}\n";
    return Result<std::string>::success(std::move(result));
}

Result<std::string> write_error_envelope(const ErrorEnvelope& envelope) {
    const auto& error = envelope.error;
    if (!validate_schema(envelope.schema) || !valid_text(envelope.request_id)
        || !valid_text(error.code) || !valid_text(error.category)
        || !valid_text(error.message) || !valid_text(error.recommended_action)) {
        return json_error(L"Error envelope contains an invalid schema or UTF-8 field");
    }
    std::string result = "{\n  \"schema\": " + quote_json_string(envelope.schema)
        + ",\n  \"ok\": false,\n  \"request_id\": " + quote_json_string(envelope.request_id)
        + ",\n  \"error\": {\n    \"code\": " + quote_json_string(error.code)
        + ",\n    \"category\": " + quote_json_string(error.category)
        + ",\n    \"message\": " + quote_json_string(error.message)
        + ",\n    \"retryable\": " + (error.retryable ? "true" : "false")
        + ",\n    \"recommended_action\": "
        + quote_json_string(error.recommended_action)
        + "\n  }\n}\n";
    return Result<std::string>::success(std::move(result));
}

} // namespace myrd::product
