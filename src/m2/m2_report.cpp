#include "m2_report.h"

#include "src/poc/json_writer.h"
#include "src/poc/report_publisher.h"
#include "src/proxy/object_names.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace myrd {
namespace {

using poc::JsonWriter;

Result<void> report_error(std::wstring message) {
    return Result<void>::failure(
        {ErrorDomain::evidence, 13, L"M2 report", std::move(message)});
}

Result<std::filesystem::path> path_error(std::wstring message) {
    return Result<std::filesystem::path>::failure(
        {ErrorDomain::evidence, 13, L"M2 report path", std::move(message)});
}

template <typename Enum>
std::optional<Enum> parse_exact(
    std::string_view value,
    std::initializer_list<std::pair<std::string_view, Enum>> entries) noexcept {
    for (const auto& [text, parsed] : entries) {
        if (value == text) {
            return parsed;
        }
    }
    return std::nullopt;
}

void write_driver_state(JsonWriter& writer, const M2DriverServiceState& state) {
    writer.start_object();
    writer.key("driver_state");
    writer.string_value(state.driver_state);
    writer.key("service_name");
    writer.string_value(state.service_name);
    writer.key("service_state");
    writer.string_value(state.service_state);
    writer.end_object();
}

void write_performance_raw(JsonWriter& writer, const M2PerformanceRaw& value) {
    writer.start_object();
    writer.key("operations");
    writer.uint_value(value.operations);
    writer.key("bytes");
    writer.uint_value(value.bytes);
    writer.key("elapsed_us");
    writer.uint_value(value.elapsed_us);
    writer.end_object();
}

enum class JsonType : std::uint8_t {
    null_value,
    boolean,
    string,
    number,
    array,
    object
};

struct JsonValue {
    JsonType type = JsonType::null_value;
    bool boolean = false;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output) {
        skip_whitespace();
        if (!parse_value(output, 0)) {
            return false;
        }
        skip_whitespace();
        return position_ == input_.size();
    }

private:
    static constexpr std::size_t kMaxDepth = 128;

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current != ' ' && current != '\t' && current != '\r' &&
                current != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool parse_value(JsonValue& output, std::size_t depth) {
        if (depth > kMaxDepth || position_ >= input_.size()) {
            return false;
        }
        switch (input_[position_]) {
        case '{':
            return parse_object(output, depth + 1);
        case '[':
            return parse_array(output, depth + 1);
        case '"':
            output.type = JsonType::string;
            return parse_string(output.text);
        case 't':
            output.type = JsonType::boolean;
            output.boolean = true;
            return consume_literal("true");
        case 'f':
            output.type = JsonType::boolean;
            output.boolean = false;
            return consume_literal("false");
        case 'n':
            output.type = JsonType::null_value;
            return consume_literal("null");
        default:
            output.type = JsonType::number;
            return parse_number(output.text);
        }
    }

    bool parse_object(JsonValue& output, std::size_t depth) {
        output.type = JsonType::object;
        ++position_;
        skip_whitespace();
        if (consume('}')) {
            return true;
        }
        while (position_ < input_.size()) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (!consume(':')) {
                return false;
            }
            skip_whitespace();
            JsonValue value;
            if (!parse_value(value, depth)) {
                return false;
            }
            output.object.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skip_whitespace();
        }
        return false;
    }

    bool parse_array(JsonValue& output, std::size_t depth) {
        output.type = JsonType::array;
        ++position_;
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        while (position_ < input_.size()) {
            JsonValue value;
            if (!parse_value(value, depth)) {
                return false;
            }
            output.array.push_back(std::move(value));
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skip_whitespace();
        }
        return false;
    }

    static int hex_value(char value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return 10 + value - 'a';
        }
        if (value >= 'A' && value <= 'F') {
            return 10 + value - 'A';
        }
        return -1;
    }

    bool parse_hex_quad(std::uint32_t& code_point) {
        if (input_.size() - position_ < 4) {
            return false;
        }
        code_point = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = hex_value(input_[position_ + index]);
            if (digit < 0) {
                return false;
            }
            code_point = (code_point << 4U) |
                         static_cast<std::uint32_t>(digit);
        }
        position_ += 4;
        return true;
    }

    static void append_utf8(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7FU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            output.push_back(
                static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            output.push_back(
                static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            output.push_back(
                static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }

    bool copy_valid_utf8(std::string& output) {
        const auto first = static_cast<unsigned char>(input_[position_]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
            code_point = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
            code_point = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (input_.size() - position_ < length) {
            return false;
        }
        for (std::size_t index = 1; index < length; ++index) {
            const auto continuation =
                static_cast<unsigned char>(input_[position_ + index]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        output.append(input_.substr(position_, length));
        position_ += length;
        return true;
    }

    bool parse_string(std::string& output) {
        if (!consume('"')) {
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const auto current =
                static_cast<unsigned char>(input_[position_++]);
            if (current == static_cast<unsigned char>('"')) {
                return true;
            }
            if (current < 0x20U) {
                return false;
            }
            if (current >= 0x80U) {
                --position_;
                if (!copy_valid_utf8(output)) {
                    return false;
                }
                continue;
            }
            if (current != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(current));
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!parse_hex_quad(code_point)) {
                    return false;
                }
                if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                    if (input_.size() - position_ < 6 ||
                        input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex_quad(low) || low < 0xDC00U ||
                        low > 0xDFFFU) {
                        return false;
                    }
                    code_point = 0x10000U +
                                 ((code_point - 0xD800U) << 10U) +
                                 (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    return false;
                }
                append_utf8(output, code_point);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parse_number(std::string& output) {
        const std::size_t start = position_;
        consume('-');
        if (position_ >= input_.size()) {
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                return false;
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (fraction_start == position_) {
                return false;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (exponent_start == position_) {
                return false;
            }
        }
        output.assign(input_.substr(start, position_ - start));
        return true;
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

class ReportValidator {
public:
    bool validate(const JsonValue& root) {
        if (root.type != JsonType::object) {
            fail();
            return false;
        }

        const auto schema_version = uint_member(root, "schema_version");
        if (!schema_version || *schema_version != kM2ReportSchemaVersion) {
            fail();
        }

        const JsonValue* run = object_member(root, "run");
        if (run != nullptr) {
            string_member(*run, "run_id");
            const auto mode = string_member(*run, "mode");
            if (!mode || !parse_m2_run_mode(*mode)) fail();
            string_member(*run, "started_utc");
            const auto ended = string_member(*run, "ended_utc");
            if (!ended || ended->empty()) incomplete_ = true;
        }

        const JsonValue* build = object_member(root, "build_toolchain");
        if (build != nullptr) {
            for (const auto name : {"build_version", "git_commit",
                                    "configuration", "compiler",
                                    "compiler_version", "cmake_version"}) {
                string_member(*build, name);
            }
        }

        const JsonValue* baseline =
            object_member(root, "accepted_m1_baseline");
        if (baseline != nullptr) {
            string_member(*baseline, "report_path");
            string_member(*baseline, "report_sha256");
            uint_member(*baseline, "m1_schema_version");
            const auto m1_verdict = string_member(*baseline, "m1_verdict");
            if (!m1_verdict || *m1_verdict != "GO") fail();
            const auto accepted = bool_member(*baseline, "accepted");
            if (!accepted || !*accepted) failure_ = true;
        }

        const JsonValue* machine = object_member(root, "machine_os");
        if (machine != nullptr) {
            for (const auto name : {"machine_name", "os_name", "os_version",
                                    "architecture"}) {
                string_member(*machine, name);
            }
        }
        validate_driver_state(root, "state_before");
        validate_driver_state(root, "state_after");

        const JsonValue* geometry = object_member(root, "geometry_options");
        if (geometry != nullptr) {
            uint_member(*geometry, "disk_size_bytes");
            uint_member(*geometry, "block_size_bytes");
            uint_member(*geometry, "memory_limit_bytes");
            bool_member(*geometry, "fake_backing");
        }

        const JsonValue* state = object_member(root, "state");
        if (state != nullptr) {
            const auto initial = string_member(*state, "initial_state");
            if (!initial || !parse_m2_initial_state(*initial)) fail();
            uint_member(*state, "initial_backing_generation");
            uint_member(*state, "current_backing_generation");
            const auto statement =
                string_member(*state, "durability_scope_statement");
            if (!statement || *statement != kM2NoDurableSuccessorStatement) {
                fail();
            }
        }

        validate_phases(root);

        const JsonValue* io = object_member(root, "io");
        if (io != nullptr) {
            string_member(*io, "write_sha256");
            string_member(*io, "read_sha256");
            uint_member(*io, "bytes_written");
            uint_member(*io, "bytes_read");
            uint_member(*io, "bytes_deleted");
        }

        const JsonValue* stats = object_member(root, "store_stats");
        if (stats != nullptr) {
            for (const auto name : {"view_install_count",
                                    "generation_conflict_count",
                                    "stale_load_count",
                                    "stale_load_retry_count"}) {
                uint_member(*stats, name);
            }
        }

        validate_memory_samples(root);
        validate_performance(root);
        validate_device(root);
        validate_cleanup(root);
        validate_final_baseline(root);
        validate_failures(root);

        const auto verdict_text = string_member(root, "verdict");
        const auto verdict = verdict_text ? parse_m2_verdict(*verdict_text)
                                          : std::nullopt;
        if (!verdict) {
            fail();
        }

        if (!valid_ || !verdict) {
            return false;
        }
        if (incomplete_) {
            return *verdict == M2Verdict::incomplete;
        }
        if (failure_) {
            return *verdict == M2Verdict::no_go;
        }
        return *verdict == M2Verdict::go;
    }

private:
    void fail() noexcept { valid_ = false; }

    const JsonValue* required_member(
        const JsonValue& object, std::string_view name) {
        if (object.type != JsonType::object) {
            fail();
            return nullptr;
        }
        const JsonValue* result = nullptr;
        std::size_t count = 0;
        for (const auto& [member_name, value] : object.object) {
            if (member_name == name) {
                result = &value;
                ++count;
            }
        }
        if (count != 1) {
            fail();
            return nullptr;
        }
        return result;
    }

    const JsonValue* object_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = required_member(object, name);
        if (value != nullptr && value->type != JsonType::object) {
            fail();
            return nullptr;
        }
        return value;
    }

    const JsonValue* array_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = required_member(object, name);
        if (value != nullptr && value->type != JsonType::array) {
            fail();
            return nullptr;
        }
        return value;
    }

    std::optional<std::string_view> string_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = required_member(object, name);
        if (value == nullptr || value->type != JsonType::string) {
            fail();
            return std::nullopt;
        }
        return value->text;
    }

    std::optional<bool> bool_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = required_member(object, name);
        if (value == nullptr || value->type != JsonType::boolean) {
            fail();
            return std::nullopt;
        }
        return value->boolean;
    }

    std::optional<std::uint64_t> uint_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = required_member(object, name);
        if (value == nullptr || value->type != JsonType::number ||
            value->text.empty() || value->text.front() == '-') {
            fail();
            return std::nullopt;
        }
        if (!std::all_of(value->text.begin(), value->text.end(),
                         [](char current) {
                             return current >= '0' && current <= '9';
                         })) {
            fail();
            return std::nullopt;
        }
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(
            value->text.data(), value->text.data() + value->text.size(), parsed);
        if (result.ec != std::errc{} ||
            result.ptr != value->text.data() + value->text.size()) {
            fail();
            return std::nullopt;
        }
        return parsed;
    }

    void validate_driver_state(const JsonValue& root, std::string_view name) {
        const JsonValue* state = object_member(root, name);
        if (state == nullptr) return;
        string_member(*state, "driver_state");
        string_member(*state, "service_name");
        string_member(*state, "service_state");
    }

    void validate_phases(const JsonValue& root) {
        const JsonValue* phases = array_member(root, "phases");
        if (phases == nullptr) return;
        if (phases->array.empty()) incomplete_ = true;
        for (const auto& phase : phases->array) {
            if (phase.type != JsonType::object) {
                fail();
                continue;
            }
            string_member(phase, "phase_name");
            string_member(phase, "phase_started_utc");
            string_member(phase, "phase_ended_utc");
            const auto status_text = string_member(phase, "phase_status");
            const auto status = status_text
                                    ? parse_m2_phase_status(*status_text)
                                    : std::nullopt;
            if (!status) {
                fail();
            } else if (*status == M2PhaseStatus::not_run) {
                incomplete_ = true;
            } else if (*status == M2PhaseStatus::failed) {
                failure_ = true;
            }
            const auto error = string_member(phase, "phase_error");
            if (error && !error->empty()) failure_ = true;
            uint_member(phase, "phase_elapsed_ms");
        }
    }

    void validate_memory_samples(const JsonValue& root) {
        const JsonValue* samples = array_member(root, "os_memory_samples");
        if (samples == nullptr) return;
        if (samples->array.empty()) incomplete_ = true;
        for (const auto& sample : samples->array) {
            if (sample.type != JsonType::object) {
                fail();
                continue;
            }
            string_member(sample, "captured_utc");
            uint_member(sample, "working_set_bytes");
            uint_member(sample, "private_bytes");
            uint_member(sample, "commit_bytes");
        }
    }

    void validate_performance_raw(const JsonValue& performance,
                                  std::string_view name) {
        const JsonValue* raw = object_member(performance, name);
        if (raw == nullptr) return;
        uint_member(*raw, "operations");
        uint_member(*raw, "bytes");
        uint_member(*raw, "elapsed_us");
    }

    void validate_performance(const JsonValue& root) {
        const JsonValue* performance = object_member(root, "performance");
        if (performance == nullptr) return;
        validate_performance_raw(*performance, "fixed");
        validate_performance_raw(*performance, "dynamic");
        uint_member(*performance, "ratio_numerator");
        const auto denominator = uint_member(*performance, "ratio_denominator");
        uint_member(*performance, "ratio_basis_points");
        uint_member(*performance, "minimum_ratio_basis_points");
        const auto gate_text = string_member(*performance, "gate_status");
        const auto gate = gate_text
                              ? parse_m2_performance_gate_status(*gate_text)
                              : std::nullopt;
        if (!gate) {
            fail();
        } else if (*gate == M2PerformanceGateStatus::not_evaluated) {
            incomplete_ = true;
        } else if (*gate == M2PerformanceGateStatus::failed) {
            failure_ = true;
        }
        if (!denominator || *denominator == 0) incomplete_ = true;

        const JsonValue* policy =
            object_member(*performance, "comparable_policy");
        if (policy != nullptr) {
            string_member(*policy, "policy_name");
            const auto same = bool_member(*policy, "same_workload");
            if (!same || !*same) incomplete_ = true;
            string_member(*policy, "notes");
        }
    }

    void validate_device(const JsonValue& root) {
        const JsonValue* device = object_member(root, "device_identity");
        if (device == nullptr) return;
        string_member(*device, "device_id");
        uint_member(*device, "path_id");
        uint_member(*device, "target_id");
        uint_member(*device, "lun");
        uint_member(*device, "physical_drive_number");
        string_member(*device, "physical_drive_path");
    }

    void validate_cleanup(const JsonValue& root) {
        const JsonValue* actions = array_member(root, "cleanup_actions");
        if (actions == nullptr) return;
        if (actions->array.empty()) incomplete_ = true;
        for (const auto& action : actions->array) {
            if (action.type != JsonType::object) {
                fail();
                continue;
            }
            string_member(action, "cleanup_name");
            const auto attempted = bool_member(action, "cleanup_attempted");
            const auto result_text = string_member(action, "cleanup_result");
            const auto result = result_text
                                    ? parse_m2_cleanup_result(*result_text)
                                    : std::nullopt;
            const auto error = string_member(action, "cleanup_error");
            if (!result) {
                fail();
                continue;
            }
            if (!attempted || !*attempted ||
                *result == M2CleanupResult::not_attempted) {
                incomplete_ = true;
            }
            if (*result == M2CleanupResult::failed ||
                (error && !error->empty())) {
                failure_ = true;
            }
            if (attempted && ((*attempted &&
                               *result == M2CleanupResult::not_attempted) ||
                              (!*attempted &&
                               *result != M2CleanupResult::not_attempted))) {
                fail();
            }
        }
    }

    void validate_final_baseline(const JsonValue& root) {
        const JsonValue* baseline =
            object_member(root, "final_baseline_comparison");
        if (baseline == nullptr) return;
        const auto comparison_text = string_member(*baseline, "comparison");
        const auto comparison = comparison_text
                                    ? parse_m2_baseline_comparison(
                                          *comparison_text)
                                    : std::nullopt;
        if (!comparison) {
            fail();
        } else if (*comparison == M2BaselineComparison::not_compared) {
            incomplete_ = true;
        } else if (*comparison == M2BaselineComparison::mismatch) {
            failure_ = true;
        }
        string_member(*baseline, "before_fingerprint");
        string_member(*baseline, "after_fingerprint");
    }

    void validate_failures(const JsonValue& root) {
        const JsonValue* failures = array_member(root, "failures");
        if (failures == nullptr) return;
        if (!failures->array.empty()) failure_ = true;
        for (const auto& failure : failures->array) {
            if (failure.type != JsonType::object) {
                fail();
                continue;
            }
            string_member(failure, "failure_phase");
            uint_member(failure, "failure_code");
            string_member(failure, "failure_message");
        }
    }

    bool valid_ = true;
    bool incomplete_ = false;
    bool failure_ = false;
};

std::string sanitize_run_id(std::string_view run_id) {
    std::string sanitized;
    sanitized.reserve(run_id.size());
    for (const char current : run_id) {
        const auto byte = static_cast<unsigned char>(current);
        if (std::isalnum(byte) || current == '-' || current == '_' ||
            current == '.') {
            sanitized.push_back(current);
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) sanitized = "run";
    return sanitized;
}

} // namespace

std::string_view to_string(M2RunMode value) noexcept {
    switch (value) {
    case M2RunMode::fake: return "FAKE";
    }
    return {};
}

std::string_view to_string(M2InitialState value) noexcept {
    switch (value) {
    case M2InitialState::empty: return "EMPTY";
    case M2InitialState::seeded: return "SEEDED";
    }
    return {};
}

std::string_view to_string(M2PhaseStatus value) noexcept {
    switch (value) {
    case M2PhaseStatus::not_run: return "NOT-RUN";
    case M2PhaseStatus::passed: return "PASS";
    case M2PhaseStatus::failed: return "FAIL";
    }
    return {};
}

std::string_view to_string(M2CleanupResult value) noexcept {
    switch (value) {
    case M2CleanupResult::not_attempted: return "NOT-ATTEMPTED";
    case M2CleanupResult::succeeded: return "SUCCEEDED";
    case M2CleanupResult::failed: return "FAILED";
    }
    return {};
}

std::string_view to_string(M2PerformanceGateStatus value) noexcept {
    switch (value) {
    case M2PerformanceGateStatus::not_evaluated: return "NOT-EVALUATED";
    case M2PerformanceGateStatus::passed: return "PASS";
    case M2PerformanceGateStatus::failed: return "FAIL";
    }
    return {};
}

std::string_view to_string(M2BaselineComparison value) noexcept {
    switch (value) {
    case M2BaselineComparison::not_compared: return "NOT-COMPARED";
    case M2BaselineComparison::match: return "MATCH";
    case M2BaselineComparison::mismatch: return "MISMATCH";
    }
    return {};
}

std::string_view to_string(M2Verdict value) noexcept {
    switch (value) {
    case M2Verdict::incomplete: return "INCOMPLETE";
    case M2Verdict::go: return "GO";
    case M2Verdict::no_go: return "NO-GO";
    }
    return {};
}

std::optional<M2RunMode> parse_m2_run_mode(std::string_view value) noexcept {
    return parse_exact<M2RunMode>(value, {{"FAKE", M2RunMode::fake}});
}

std::optional<M2InitialState> parse_m2_initial_state(
    std::string_view value) noexcept {
    return parse_exact<M2InitialState>(
        value, {{"EMPTY", M2InitialState::empty},
                {"SEEDED", M2InitialState::seeded}});
}

std::optional<M2PhaseStatus> parse_m2_phase_status(
    std::string_view value) noexcept {
    return parse_exact<M2PhaseStatus>(
        value, {{"NOT-RUN", M2PhaseStatus::not_run},
                {"PASS", M2PhaseStatus::passed},
                {"FAIL", M2PhaseStatus::failed}});
}

std::optional<M2CleanupResult> parse_m2_cleanup_result(
    std::string_view value) noexcept {
    return parse_exact<M2CleanupResult>(
        value, {{"NOT-ATTEMPTED", M2CleanupResult::not_attempted},
                {"SUCCEEDED", M2CleanupResult::succeeded},
                {"FAILED", M2CleanupResult::failed}});
}

std::optional<M2PerformanceGateStatus> parse_m2_performance_gate_status(
    std::string_view value) noexcept {
    return parse_exact<M2PerformanceGateStatus>(
        value,
        {{"NOT-EVALUATED", M2PerformanceGateStatus::not_evaluated},
         {"PASS", M2PerformanceGateStatus::passed},
         {"FAIL", M2PerformanceGateStatus::failed}});
}

std::optional<M2BaselineComparison> parse_m2_baseline_comparison(
    std::string_view value) noexcept {
    return parse_exact<M2BaselineComparison>(
        value, {{"NOT-COMPARED", M2BaselineComparison::not_compared},
                {"MATCH", M2BaselineComparison::match},
                {"MISMATCH", M2BaselineComparison::mismatch}});
}

std::optional<M2Verdict> parse_m2_verdict(std::string_view value) noexcept {
    return parse_exact<M2Verdict>(
        value, {{"INCOMPLETE", M2Verdict::incomplete},
                {"GO", M2Verdict::go}, {"NO-GO", M2Verdict::no_go}});
}

std::string serialize_m2_report(const M2Report& report) {
    JsonWriter writer;
    writer.start_object();
    writer.key("schema_version");
    writer.uint_value(report.schema_version);

    writer.key("run");
    writer.start_object();
    writer.key("run_id"); writer.string_value(report.run.run_id);
    writer.key("mode"); writer.string_value(to_string(report.run.mode));
    writer.key("started_utc"); writer.string_value(report.run.started_utc);
    writer.key("ended_utc"); writer.string_value(report.run.ended_utc);
    writer.end_object();

    writer.key("build_toolchain");
    writer.start_object();
    writer.key("build_version");
    writer.string_value(report.build_toolchain.build_version);
    writer.key("git_commit"); writer.string_value(report.build_toolchain.git_commit);
    writer.key("configuration");
    writer.string_value(report.build_toolchain.configuration);
    writer.key("compiler"); writer.string_value(report.build_toolchain.compiler);
    writer.key("compiler_version");
    writer.string_value(report.build_toolchain.compiler_version);
    writer.key("cmake_version");
    writer.string_value(report.build_toolchain.cmake_version);
    writer.end_object();

    writer.key("accepted_m1_baseline");
    writer.start_object();
    writer.key("report_path");
    writer.string_value(report.accepted_m1_baseline.report_path);
    writer.key("report_sha256");
    writer.string_value(report.accepted_m1_baseline.report_sha256);
    writer.key("m1_schema_version");
    writer.uint_value(report.accepted_m1_baseline.m1_schema_version);
    writer.key("m1_verdict");
    writer.string_value(report.accepted_m1_baseline.m1_verdict);
    writer.key("accepted");
    writer.boolean_value(report.accepted_m1_baseline.accepted);
    writer.end_object();

    writer.key("machine_os");
    writer.start_object();
    writer.key("machine_name"); writer.string_value(report.machine_os.machine_name);
    writer.key("os_name"); writer.string_value(report.machine_os.os_name);
    writer.key("os_version"); writer.string_value(report.machine_os.os_version);
    writer.key("architecture");
    writer.string_value(report.machine_os.architecture);
    writer.end_object();

    writer.key("state_before"); write_driver_state(writer, report.state_before);
    writer.key("state_after"); write_driver_state(writer, report.state_after);

    writer.key("geometry_options");
    writer.start_object();
    writer.key("disk_size_bytes");
    writer.uint_value(report.geometry_options.disk_size_bytes);
    writer.key("block_size_bytes");
    writer.uint_value(report.geometry_options.block_size_bytes);
    writer.key("memory_limit_bytes");
    writer.uint_value(report.geometry_options.memory_limit_bytes);
    writer.key("fake_backing");
    writer.boolean_value(report.geometry_options.fake_backing);
    writer.end_object();

    writer.key("state");
    writer.start_object();
    writer.key("initial_state");
    writer.string_value(to_string(report.state.initial_state));
    writer.key("initial_backing_generation");
    writer.uint_value(report.state.initial_backing_generation);
    writer.key("current_backing_generation");
    writer.uint_value(report.state.current_backing_generation);
    writer.key("durability_scope_statement");
    writer.string_value(report.state.durability_scope_statement);
    writer.end_object();

    writer.key("phases");
    writer.start_array();
    for (const auto& phase : report.phases) {
        writer.start_object();
        writer.key("phase_name"); writer.string_value(phase.name);
        writer.key("phase_started_utc"); writer.string_value(phase.started_utc);
        writer.key("phase_ended_utc"); writer.string_value(phase.ended_utc);
        writer.key("phase_status"); writer.string_value(to_string(phase.status));
        writer.key("phase_error"); writer.string_value(phase.error);
        writer.key("phase_elapsed_ms"); writer.uint_value(phase.elapsed_ms);
        writer.end_object();
    }
    writer.end_array();

    writer.key("io");
    writer.start_object();
    writer.key("write_sha256"); writer.string_value(report.io.write_sha256);
    writer.key("read_sha256"); writer.string_value(report.io.read_sha256);
    writer.key("bytes_written"); writer.uint_value(report.io.bytes_written);
    writer.key("bytes_read"); writer.uint_value(report.io.bytes_read);
    writer.key("bytes_deleted"); writer.uint_value(report.io.bytes_deleted);
    writer.end_object();

    writer.key("store_stats");
    writer.start_object();
    writer.key("view_install_count");
    writer.uint_value(report.store_stats.view_install_count);
    writer.key("generation_conflict_count");
    writer.uint_value(report.store_stats.generation_conflict_count);
    writer.key("stale_load_count");
    writer.uint_value(report.store_stats.stale_load_count);
    writer.key("stale_load_retry_count");
    writer.uint_value(report.store_stats.stale_load_retry_count);
    writer.end_object();

    writer.key("os_memory_samples");
    writer.start_array();
    for (const auto& sample : report.os_memory_samples) {
        writer.start_object();
        writer.key("captured_utc"); writer.string_value(sample.captured_utc);
        writer.key("working_set_bytes");
        writer.uint_value(sample.working_set_bytes);
        writer.key("private_bytes"); writer.uint_value(sample.private_bytes);
        writer.key("commit_bytes"); writer.uint_value(sample.commit_bytes);
        writer.end_object();
    }
    writer.end_array();

    writer.key("performance");
    writer.start_object();
    writer.key("fixed"); write_performance_raw(writer, report.performance.fixed);
    writer.key("dynamic");
    write_performance_raw(writer, report.performance.dynamic);
    writer.key("ratio_numerator");
    writer.uint_value(report.performance.ratio_numerator);
    writer.key("ratio_denominator");
    writer.uint_value(report.performance.ratio_denominator);
    writer.key("ratio_basis_points");
    writer.uint_value(report.performance.ratio_basis_points);
    writer.key("minimum_ratio_basis_points");
    writer.uint_value(report.performance.minimum_ratio_basis_points);
    writer.key("gate_status");
    writer.string_value(to_string(report.performance.gate_status));
    writer.key("comparable_policy");
    writer.start_object();
    writer.key("policy_name");
    writer.string_value(report.performance.comparable_policy.policy_name);
    writer.key("same_workload");
    writer.boolean_value(report.performance.comparable_policy.same_workload);
    writer.key("notes");
    writer.string_value(report.performance.comparable_policy.notes);
    writer.end_object();
    writer.end_object();

    writer.key("device_identity");
    writer.start_object();
    writer.key("device_id"); writer.string_value(report.device_identity.device_id);
    writer.key("path_id"); writer.uint_value(report.device_identity.path_id);
    writer.key("target_id"); writer.uint_value(report.device_identity.target_id);
    writer.key("lun"); writer.uint_value(report.device_identity.lun);
    writer.key("physical_drive_number");
    writer.uint_value(report.device_identity.physical_drive_number);
    writer.key("physical_drive_path");
    writer.string_value(report.device_identity.physical_drive_path);
    writer.end_object();

    writer.key("cleanup_actions");
    writer.start_array();
    for (const auto& action : report.cleanup_actions) {
        writer.start_object();
        writer.key("cleanup_name"); writer.string_value(action.name);
        writer.key("cleanup_attempted"); writer.boolean_value(action.attempted);
        writer.key("cleanup_result"); writer.string_value(to_string(action.result));
        writer.key("cleanup_error"); writer.string_value(action.error);
        writer.end_object();
    }
    writer.end_array();

    writer.key("final_baseline_comparison");
    writer.start_object();
    writer.key("comparison");
    writer.string_value(to_string(report.final_baseline_comparison.comparison));
    writer.key("before_fingerprint");
    writer.string_value(report.final_baseline_comparison.before_fingerprint);
    writer.key("after_fingerprint");
    writer.string_value(report.final_baseline_comparison.after_fingerprint);
    writer.end_object();

    writer.key("failures");
    writer.start_array();
    for (const auto& failure : report.failures) {
        writer.start_object();
        writer.key("failure_phase"); writer.string_value(failure.phase);
        writer.key("failure_code"); writer.uint_value(failure.code);
        writer.key("failure_message"); writer.string_value(failure.message);
        writer.end_object();
    }
    writer.end_array();

    writer.key("verdict"); writer.string_value(to_string(report.verdict));
    writer.end_object();
    return writer.finish();
}

std::string m2_report_json_schema() {
    return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://myrd.invalid/schema/m2-report-v1.json",
  "title": "MYRD M2 acceptance report v1",
  "type": "object",
  "required": ["schema_version", "run", "build_toolchain", "accepted_m1_baseline", "machine_os", "state_before", "state_after", "geometry_options", "state", "phases", "io", "store_stats", "os_memory_samples", "performance", "device_identity", "cleanup_actions", "final_baseline_comparison", "failures", "verdict"],
  "properties": {
    "schema_version": {"type": "integer", "const": 1},
    "run": {"type": "object", "required": ["run_id", "mode", "started_utc", "ended_utc"], "properties": {"run_id": {"type": "string"}, "mode": {"enum": ["FAKE"]}, "started_utc": {"type": "string"}, "ended_utc": {"type": "string"}}},
    "build_toolchain": {"type": "object", "required": ["build_version", "git_commit", "configuration", "compiler", "compiler_version", "cmake_version"], "properties": {"build_version": {"type": "string"}, "git_commit": {"type": "string"}, "configuration": {"type": "string"}, "compiler": {"type": "string"}, "compiler_version": {"type": "string"}, "cmake_version": {"type": "string"}}},
    "accepted_m1_baseline": {"type": "object", "required": ["report_path", "report_sha256", "m1_schema_version", "m1_verdict", "accepted"], "properties": {"report_path": {"type": "string"}, "report_sha256": {"type": "string"}, "m1_schema_version": {"$ref": "#/$defs/uint64"}, "m1_verdict": {"const": "GO"}, "accepted": {"type": "boolean"}}},
    "machine_os": {"type": "object", "required": ["machine_name", "os_name", "os_version", "architecture"], "properties": {"machine_name": {"type": "string"}, "os_name": {"type": "string"}, "os_version": {"type": "string"}, "architecture": {"type": "string"}}},
    "state_before": {"$ref": "#/$defs/driver_service_state"},
    "state_after": {"$ref": "#/$defs/driver_service_state"},
    "geometry_options": {"type": "object", "required": ["disk_size_bytes", "block_size_bytes", "memory_limit_bytes", "fake_backing"], "properties": {"disk_size_bytes": {"$ref": "#/$defs/uint64"}, "block_size_bytes": {"$ref": "#/$defs/uint64"}, "memory_limit_bytes": {"$ref": "#/$defs/uint64"}, "fake_backing": {"type": "boolean"}}},
    "state": {"type": "object", "required": ["initial_state", "initial_backing_generation", "current_backing_generation", "durability_scope_statement"], "properties": {"initial_state": {"enum": ["EMPTY", "SEEDED"]}, "initial_backing_generation": {"$ref": "#/$defs/uint64"}, "current_backing_generation": {"$ref": "#/$defs/uint64"}, "durability_scope_statement": {"const": "M2 has no durable backing successor; all backing generations end with this run."}}},
    "phases": {"type": "array", "items": {"type": "object", "required": ["phase_name", "phase_started_utc", "phase_ended_utc", "phase_status", "phase_error", "phase_elapsed_ms"], "properties": {"phase_name": {"type": "string"}, "phase_started_utc": {"type": "string"}, "phase_ended_utc": {"type": "string"}, "phase_status": {"enum": ["NOT-RUN", "PASS", "FAIL"]}, "phase_error": {"type": "string"}, "phase_elapsed_ms": {"$ref": "#/$defs/uint64"}}}},
    "io": {"type": "object", "required": ["write_sha256", "read_sha256", "bytes_written", "bytes_read", "bytes_deleted"], "properties": {"write_sha256": {"type": "string"}, "read_sha256": {"type": "string"}, "bytes_written": {"$ref": "#/$defs/uint64"}, "bytes_read": {"$ref": "#/$defs/uint64"}, "bytes_deleted": {"$ref": "#/$defs/uint64"}}},
    "store_stats": {"type": "object", "required": ["view_install_count", "generation_conflict_count", "stale_load_count", "stale_load_retry_count"], "properties": {"view_install_count": {"$ref": "#/$defs/uint64"}, "generation_conflict_count": {"$ref": "#/$defs/uint64"}, "stale_load_count": {"$ref": "#/$defs/uint64"}, "stale_load_retry_count": {"$ref": "#/$defs/uint64"}}},
    "os_memory_samples": {"type": "array", "items": {"type": "object", "required": ["captured_utc", "working_set_bytes", "private_bytes", "commit_bytes"], "properties": {"captured_utc": {"type": "string"}, "working_set_bytes": {"$ref": "#/$defs/uint64"}, "private_bytes": {"$ref": "#/$defs/uint64"}, "commit_bytes": {"$ref": "#/$defs/uint64"}}}},
    "performance": {"type": "object", "required": ["fixed", "dynamic", "ratio_numerator", "ratio_denominator", "ratio_basis_points", "minimum_ratio_basis_points", "gate_status", "comparable_policy"], "properties": {"fixed": {"$ref": "#/$defs/performance_raw"}, "dynamic": {"$ref": "#/$defs/performance_raw"}, "ratio_numerator": {"$ref": "#/$defs/uint64"}, "ratio_denominator": {"$ref": "#/$defs/uint64"}, "ratio_basis_points": {"$ref": "#/$defs/uint64"}, "minimum_ratio_basis_points": {"$ref": "#/$defs/uint64"}, "gate_status": {"enum": ["NOT-EVALUATED", "PASS", "FAIL"]}, "comparable_policy": {"type": "object", "required": ["policy_name", "same_workload", "notes"], "properties": {"policy_name": {"type": "string"}, "same_workload": {"type": "boolean"}, "notes": {"type": "string"}}}}},
    "device_identity": {"type": "object", "required": ["device_id", "path_id", "target_id", "lun", "physical_drive_number", "physical_drive_path"], "properties": {"device_id": {"type": "string"}, "path_id": {"$ref": "#/$defs/uint64"}, "target_id": {"$ref": "#/$defs/uint64"}, "lun": {"$ref": "#/$defs/uint64"}, "physical_drive_number": {"$ref": "#/$defs/uint64"}, "physical_drive_path": {"type": "string"}}},
    "cleanup_actions": {"type": "array", "items": {"type": "object", "required": ["cleanup_name", "cleanup_attempted", "cleanup_result", "cleanup_error"], "properties": {"cleanup_name": {"type": "string"}, "cleanup_attempted": {"type": "boolean"}, "cleanup_result": {"enum": ["NOT-ATTEMPTED", "SUCCEEDED", "FAILED"]}, "cleanup_error": {"type": "string"}}}},
    "final_baseline_comparison": {"type": "object", "required": ["comparison", "before_fingerprint", "after_fingerprint"], "properties": {"comparison": {"enum": ["NOT-COMPARED", "MATCH", "MISMATCH"]}, "before_fingerprint": {"type": "string"}, "after_fingerprint": {"type": "string"}}},
    "failures": {"type": "array", "items": {"type": "object", "required": ["failure_phase", "failure_code", "failure_message"], "properties": {"failure_phase": {"type": "string"}, "failure_code": {"$ref": "#/$defs/uint64"}, "failure_message": {"type": "string"}}}},
    "verdict": {"enum": ["INCOMPLETE", "GO", "NO-GO"]}
  },
  "$defs": {
    "uint64": {"type": "integer", "minimum": 0, "maximum": 18446744073709551615},
    "driver_service_state": {"type": "object", "required": ["driver_state", "service_name", "service_state"], "properties": {"driver_state": {"type": "string"}, "service_name": {"type": "string"}, "service_state": {"type": "string"}}},
    "performance_raw": {"type": "object", "required": ["operations", "bytes", "elapsed_us"], "properties": {"operations": {"$ref": "#/$defs/uint64"}, "bytes": {"$ref": "#/$defs/uint64"}, "elapsed_us": {"$ref": "#/$defs/uint64"}}}
  }
})json";
}

Result<void> validate_m2_report_json(std::string_view json) {
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root)) {
        return report_error(L"malformed JSON or trailing data");
    }
    ReportValidator validator;
    if (!validator.validate(root)) {
        return report_error(L"report violates the M2 v1 schema or semantics");
    }
    return Result<void>::success();
}

Result<std::filesystem::path> make_unique_m2_report_path(
    const std::filesystem::path& evidence_directory,
    std::string_view run_id) {
    std::error_code filesystem_error;
    if (!std::filesystem::exists(evidence_directory, filesystem_error) ||
        filesystem_error ||
        !std::filesystem::is_directory(evidence_directory, filesystem_error) ||
        filesystem_error) {
        return path_error(L"evidence directory must already exist");
    }

    const std::string safe_run_id = sanitize_run_id(run_id);
    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        auto unique_id = proxy::generate_session_id();
        if (!unique_id) {
            return Result<std::filesystem::path>::failure(unique_id.error());
        }
        const std::wstring filename =
            L"m2-report-" +
            std::wstring(safe_run_id.begin(), safe_run_id.end()) + L"-" +
            unique_id.value() + L".json";
        const auto candidate = evidence_directory / filename;
        filesystem_error.clear();
        if (!std::filesystem::exists(candidate, filesystem_error) &&
            !filesystem_error) {
            return Result<std::filesystem::path>::success(candidate);
        }
        if (filesystem_error) {
            return path_error(L"failed to inspect generated report path");
        }
    }
    return path_error(L"could not generate a non-existing report path");
}

Result<void> publish_m2_report(
    const M2Report& report,
    const std::filesystem::path& final_path) {
    const std::string json = serialize_m2_report(report);
    auto validation = validate_m2_report_json(json);
    if (!validation) return validation;
    return poc::publish_report_atomic(json, final_path);
}

Result<void> validate_published_m2_report(
    const std::filesystem::path& final_path) {
    std::ifstream input(final_path, std::ios::binary);
    if (!input) {
        return report_error(L"failed to open published report");
    }
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        return report_error(L"failed to read published report");
    }
    return validate_m2_report_json(json);
}

} // namespace myrd
