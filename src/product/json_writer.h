#pragma once

#include "src/common/result.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace myrd::product {

using JsonScalar = std::variant<std::string, bool, std::uint64_t>;

struct JsonField {
    std::string name;
    JsonScalar value;
};

using JsonObject = std::vector<JsonField>;

struct SuccessEnvelope {
    std::string schema;
    std::string request_id;
    std::string command;
    JsonObject result;
    std::vector<std::string> warnings;
};

struct ErrorDetail {
    std::string code;
    std::string category;
    std::string message;
    bool retryable{};
    std::string recommended_action;
};

struct ErrorEnvelope {
    std::string schema;
    std::string request_id;
    ErrorDetail error;
};

Result<std::string> escape_json_string(std::string_view value);
Result<std::string> json_from_utf16(std::wstring_view value);
std::string format_rfc3339_utc(std::chrono::system_clock::time_point value);
std::string render_uint64_as_json(std::uint64_t value);

bool validate_schema(std::string_view schema) noexcept;
bool validate_envelope_header(
    std::string_view schema,
    std::span<const std::string_view> unknown_additive_fields) noexcept;

Result<std::string> write_success_envelope(const SuccessEnvelope& envelope);
Result<std::string> write_error_envelope(const ErrorEnvelope& envelope);

} // namespace myrd::product
