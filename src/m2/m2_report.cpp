#include "m2_report.h"

#include "src/poc/json_writer.h"
#include "src/poc/report_publisher.h"
#include "src/proxy/object_names.h"
#include "src/driver/aim_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
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
    writer.key("service_start_type");
    writer.string_value(state.service_start_type);
    writer.key("driver_file_path");
    writer.string_value(state.driver_file_path);
    writer.key("driver_file_version");
    writer.string_value(state.driver_file_version);
    writer.key("driver_sha256");
    writer.string_value(state.driver_sha256);
    writer.end_object();
}

void write_performance_policy(
    JsonWriter& writer, const M2PerformancePolicy& value) {
    writer.start_object();
    writer.key("machine_fingerprint");
    writer.string_value(value.machine_fingerprint);
    writer.key("build_fingerprint");
    writer.string_value(value.build_fingerprint);
    writer.key("process_policy");
    writer.string_value(value.process_policy);
    writer.key("raw_io_flags");
    writer.string_value(value.raw_io_flags);
    writer.end_object();
}

bool is_sha256(std::string_view value) noexcept {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char current) {
            return (current >= '0' && current <= '9') ||
                (current >= 'a' && current <= 'f') ||
                (current >= 'A' && current <= 'F');
        });
}

std::optional<std::int64_t> parse_utc_seconds(
    std::string_view value) noexcept {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return std::nullopt;
    }
    const auto component = [value](std::size_t offset, std::size_t length)
        -> std::optional<unsigned> {
        unsigned parsed = 0;
        for (std::size_t index = 0; index < length; ++index) {
            const char current = value[offset + index];
            if (current < '0' || current > '9') return std::nullopt;
            parsed = parsed * 10U + static_cast<unsigned>(current - '0');
        }
        return parsed;
    };
    const auto year_value = component(0, 4);
    const auto month_value = component(5, 2);
    const auto day_value = component(8, 2);
    const auto hour_value = component(11, 2);
    const auto minute_value = component(14, 2);
    const auto second_value = component(17, 2);
    if (!year_value || !month_value || !day_value || !hour_value ||
        !minute_value || !second_value || *year_value == 0 ||
        *month_value == 0 || *month_value > 12 || *day_value == 0 ||
        *hour_value > 23 || *minute_value > 59 || *second_value > 59) {
        return std::nullopt;
    }
    const auto date = std::chrono::year_month_day{
        std::chrono::year{static_cast<int>(*year_value)},
        std::chrono::month{*month_value}, std::chrono::day{*day_value}};
    if (!date.ok()) return std::nullopt;
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(days).count() +
        static_cast<std::int64_t>(*hour_value) * 3'600 +
        static_cast<std::int64_t>(*minute_value) * 60 +
        static_cast<std::int64_t>(*second_value);
}

void write_string_array(
    JsonWriter& writer, const std::vector<std::string>& values) {
    writer.start_array();
    for (const auto& value : values) writer.string_value(value);
    writer.end_array();
}

void write_inventory(
    JsonWriter& writer, const M2InventorySnapshot& inventory) {
    writer.start_object();
    writer.key("physical_disks");
    write_string_array(writer, inventory.physical_disks);
    writer.key("volumes");
    write_string_array(writer, inventory.volumes);
    writer.key("mount_points");
    write_string_array(writer, inventory.mount_points);
    writer.key("m2_device_ids");
    write_string_array(writer, inventory.m2_device_ids);
    writer.key("service_state");
    writer.string_value(inventory.service_state);
    writer.key("service_start_type");
    writer.string_value(inventory.service_start_type);
    writer.key("process_handle_count");
    writer.uint_value(inventory.process_handle_count);
    writer.key("process_thread_count");
    writer.uint_value(inventory.process_thread_count);
    writer.key("process_private_bytes");
    writer.uint_value(inventory.process_private_bytes);
    writer.end_object();
}

void write_inventory_diff(JsonWriter& writer, const M2InventoryDiff& diff) {
    writer.start_object();
    writer.key("added");
    write_string_array(writer, diff.added);
    writer.key("removed");
    write_string_array(writer, diff.removed);
    writer.key("changed");
    write_string_array(writer, diff.changed);
    writer.end_object();
}

void write_store_stats(JsonWriter& writer, const DynamicBlockStoreStats& value) {
    writer.start_object();
#define MYRD_WRITE_STAT(name) \
    writer.key(#name); writer.uint_value(value.name)
    MYRD_WRITE_STAT(logical_size_bytes);
    MYRD_WRITE_STAT(reserved_bytes);
    MYRD_WRITE_STAT(committed_bytes);
    MYRD_WRITE_STAT(resident_clean_blocks);
    MYRD_WRITE_STAT(resident_dirty_blocks);
    MYRD_WRITE_STAT(zero_clean_blocks);
    MYRD_WRITE_STAT(zero_dirty_blocks);
    MYRD_WRITE_STAT(loading_blocks);
    MYRD_WRITE_STAT(evicted_blocks_total);
    MYRD_WRITE_STAT(backing_reads_total);
    MYRD_WRITE_STAT(backing_read_bytes_total);
    MYRD_WRITE_STAT(uncached_reads_total);
    MYRD_WRITE_STAT(unmap_bytes_total);
    MYRD_WRITE_STAT(zero_bytes_total);
    MYRD_WRITE_STAT(hard_limit_rejections);
    MYRD_WRITE_STAT(commit_failures);
    MYRD_WRITE_STAT(decommit_failures);
    MYRD_WRITE_STAT(synchronous_pressure_passes_total);
    MYRD_WRITE_STAT(synchronous_pressure_waits_total);
    MYRD_WRITE_STAT(synchronous_pressure_no_progress_total);
    MYRD_WRITE_STAT(synchronous_pressure_cancellations_total);
    MYRD_WRITE_STAT(synchronous_pressure_deadlines_total);
    MYRD_WRITE_STAT(commit_reservation_rollbacks_total);
    MYRD_WRITE_STAT(snapshot_records_total);
    MYRD_WRITE_STAT(snapshot_version_conflicts);
    MYRD_WRITE_STAT(backing_generation);
    MYRD_WRITE_STAT(backing_view_installs_total);
    MYRD_WRITE_STAT(backing_generation_conflicts);
    MYRD_WRITE_STAT(stale_backing_load_retries);
#undef MYRD_WRITE_STAT
    writer.end_object();
}

void write_sequential_sample(
    JsonWriter& writer, const M2SequentialPerformanceSample& value) {
    writer.start_object();
    writer.key("bytes"); writer.uint_value(value.bytes);
    writer.key("operations"); writer.uint_value(value.operations);
    writer.key("elapsed_ticks"); writer.uint_value(value.elapsed_ticks);
    writer.key("clock_frequency_hz");
    writer.uint_value(value.clock_frequency_hz);
    writer.key("bytes_per_second");
    writer.uint_value(value.bytes_per_second);
    writer.end_object();
}

void write_random_sample(
    JsonWriter& writer, const M2RandomPerformanceSample& value) {
    writer.start_object();
    writer.key("bytes"); writer.uint_value(value.bytes);
    writer.key("operations"); writer.uint_value(value.operations);
    writer.key("elapsed_ticks"); writer.uint_value(value.elapsed_ticks);
    writer.key("clock_frequency_hz");
    writer.uint_value(value.clock_frequency_hz);
    writer.key("operations_per_second");
    writer.uint_value(value.operations_per_second);
    writer.end_object();
}

void write_performance_result(
    JsonWriter& writer, const M2PerformanceProbeResult& value) {
    writer.start_object();
    writer.key("methodology_id"); writer.string_value(value.methodology_id);
    writer.key("operation"); writer.string_value(to_string(value.operation));
    writer.key("queue_depth"); writer.uint_value(value.queue_depth);
    writer.key("region_offset"); writer.uint_value(value.region_offset);
    writer.key("region_bytes"); writer.uint_value(value.region_bytes);
    writer.key("sequential_io_bytes");
    writer.uint_value(value.sequential_io_bytes);
    writer.key("random_io_bytes"); writer.uint_value(value.random_io_bytes);
    writer.key("random_seed"); writer.uint_value(value.random_seed);
    writer.key("sequential_warmup_operations");
    writer.uint_value(value.sequential_warmup_operations);
    writer.key("random_warmup_operations");
    writer.uint_value(value.random_warmup_operations);
    writer.key("policy"); write_performance_policy(writer, value.policy);
    writer.key("sequential"); write_sequential_sample(writer, value.sequential);
    writer.key("random"); write_random_sample(writer, value.random);
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
            const auto run_id = string_member(*run, "run_id");
            if (!run_id || run_id->empty()) fail();
            const auto mode = string_member(*run, "mode");
            run_mode_ = mode ? parse_m2_run_mode(*mode) : std::nullopt;
            if (!run_mode_) fail();
            const auto started = string_member(*run, "started_utc");
            const auto ended = string_member(*run, "ended_utc");
            const auto started_seconds =
                started ? parse_utc_seconds(*started) : std::nullopt;
            const auto ended_seconds =
                ended ? parse_utc_seconds(*ended) : std::nullopt;
            if (!started_seconds || !ended_seconds ||
                *ended_seconds < *started_seconds) {
                fail();
            } else {
                run_started_seconds_ = *started_seconds;
                run_ended_seconds_ = *ended_seconds;
            }
        }

        const JsonValue* build = object_member(root, "build_toolchain");
        if (build != nullptr) {
            for (const auto name : {
                     "build_version", "git_commit", "configuration",
                     "compiler", "compiler_version", "cmake_version",
                     "executable_path"}) {
                const auto value = string_member(*build, name);
                if (!value || value->empty()) fail();
            }
            const auto executable_hash =
                string_member(*build, "executable_sha256");
            const auto build_fingerprint =
                string_member(*build, "build_fingerprint");
            if (!executable_hash || !is_sha256(*executable_hash) ||
                !build_fingerprint || !is_sha256(*build_fingerprint)) {
                fail();
            } else {
                build_fingerprint_ = *build_fingerprint;
            }
        }

        const JsonValue* baseline =
            object_member(root, "accepted_m1_baseline");
        if (baseline != nullptr) {
            const auto report_path = string_member(*baseline, "report_path");
            const auto report_sha = string_member(*baseline, "report_sha256");
            const auto baseline_schema =
                uint_member(*baseline, "m1_schema_version");
            if (!report_path || report_path->empty() || !report_sha ||
                !is_sha256(*report_sha) || !baseline_schema ||
                *baseline_schema == 0) {
                fail();
            }
            const auto m1_verdict = string_member(*baseline, "m1_verdict");
            if (!m1_verdict || *m1_verdict != "GO") fail();
            const auto accepted = bool_member(*baseline, "accepted");
            if (!accepted || !*accepted) failure_ = true;
        }

        const JsonValue* machine = object_member(root, "machine_os");
        if (machine != nullptr) {
            for (const auto name : {
                     "machine_name", "os_name", "os_version",
                     "architecture", "windows_build", "cpu_model"}) {
                const auto value = string_member(*machine, name);
                if (!value || value->empty()) fail();
            }
            const auto processors =
                uint_member(*machine, "logical_processor_count");
            const auto memory = uint_member(*machine, "physical_memory_bytes");
            const auto fingerprint =
                string_member(*machine, "machine_fingerprint");
            if (!processors || *processors == 0 || !memory || *memory == 0 ||
                !fingerprint || !is_sha256(*fingerprint)) {
                fail();
            } else {
                machine_fingerprint_ = *fingerprint;
            }
        }
        const auto before_driver = validate_driver_state(root, "state_before");
        const auto after_driver = validate_driver_state(root, "state_after");
        if (before_driver && after_driver && *before_driver != *after_driver) {
            failure_ = true;
        }

        const JsonValue* geometry = object_member(root, "geometry_options");
        if (geometry != nullptr) {
            const auto disk = uint_member(*geometry, "disk_size_bytes");
            const auto sector = uint_member(*geometry, "sector_size_bytes");
            const auto block = uint_member(*geometry, "block_size_bytes");
            const auto soft = uint_member(*geometry, "soft_limit_bytes");
            const auto hard = uint_member(*geometry, "hard_limit_bytes");
            const auto low =
                uint_member(*geometry, "eviction_low_watermark_bytes");
            const auto snapshot_bytes =
                uint_member(*geometry, "max_snapshot_batch_bytes");
            const auto snapshot_records =
                uint_member(*geometry, "max_snapshot_batch_records");
            const auto stripes = uint_member(*geometry, "lock_stripe_count");
            bool_member(*geometry, "test_policy_selected");
            const auto backing = string_member(*geometry, "backing_kind");
            if (disk && sector && block && soft && hard && low &&
                snapshot_bytes && snapshot_records && stripes && backing) {
                const bool supported_sector = *sector == 512 || *sector == 4096;
                const bool power_of_two_stripes = *stripes != 0 &&
                    (*stripes & (*stripes - 1)) == 0;
                if (*disk == 0 || !supported_sector ||
                    (*disk % *sector) != 0 || *block != (64ull << 10) ||
                    *low == 0 || *low > *soft || *soft >= *hard ||
                    (*low % *block) != 0 || (*soft % *block) != 0 ||
                    (*hard % *block) != 0 || *snapshot_bytes < *block ||
                    (*snapshot_bytes % *block) != 0 ||
                    *snapshot_records == 0 || *stripes < 64 ||
                    !power_of_two_stripes ||
                    (*backing != "zero" && *backing != "fake")) {
                    fail();
                }
                if (run_mode_ == M2RunMode::real &&
                    (*disk != (64ull << 30) || *backing != "zero")) {
                    fail();
                }
                disk_size_bytes_ = *disk;
                hard_limit_bytes_ = *hard;
            }
        }

        const JsonValue* state = object_member(root, "state");
        if (state != nullptr) {
            const auto initial = string_member(*state, "initial_state");
            if (!initial || !parse_m2_initial_state(*initial)) fail();
            const auto initial_generation =
                uint_member(*state, "initial_backing_generation");
            const auto current_generation =
                uint_member(*state, "current_backing_generation");
            const auto durable_installed =
                bool_member(*state, "durable_successor_installed");
            const auto durable_present =
                bool_member(*state, "durable_successor_generation_present");
            const auto durable_generation =
                uint_member(*state, "durable_successor_generation");
            const auto statement =
                string_member(*state, "durability_scope_statement");
            if (!statement || *statement != kM2NoDurableSuccessorStatement) {
                fail();
            }
            if (initial_generation && current_generation &&
                durable_installed && durable_present && durable_generation) {
                if (*current_generation < *initial_generation ||
                    *durable_installed || *durable_present ||
                    *durable_generation != 0) {
                    fail();
                }
                initial_backing_generation_ = *initial_generation;
                current_backing_generation_ = *current_generation;
            }
        }

        validate_phases(root);

        const JsonValue* io = object_member(root, "io");
        if (io != nullptr) {
            const auto write_hash = string_member(*io, "write_sha256");
            const auto read_hash = string_member(*io, "read_sha256");
            const auto written = uint_member(*io, "bytes_written");
            const auto read = uint_member(*io, "bytes_read");
            uint_member(*io, "bytes_deleted");
            if (write_hash && read_hash) {
                if (write_hash->empty() && read_hash->empty()) {
                    incomplete_ = true;
                } else if (!is_sha256(*write_hash) ||
                           !is_sha256(*read_hash) ||
                           *write_hash != *read_hash) {
                    fail();
                }
            }
            if (written && read && (*written == 0 || *read == 0)) {
                incomplete_ = true;
            }
        }

        validate_store_samples(root);

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
        if (failure_) {
            return *verdict == M2Verdict::no_go;
        }
        if (incomplete_) {
            return *verdict == M2Verdict::incomplete;
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

    std::optional<std::vector<std::string_view>> string_array_member(
        const JsonValue& object, std::string_view name) {
        const JsonValue* value = array_member(object, name);
        if (value == nullptr) return std::nullopt;
        std::vector<std::string_view> result;
        result.reserve(value->array.size());
        for (const auto& item : value->array) {
            if (item.type != JsonType::string || item.text.empty()) {
                fail();
                return std::nullopt;
            }
            result.push_back(item.text);
        }
        if (!std::is_sorted(result.begin(), result.end()) ||
            std::adjacent_find(result.begin(), result.end()) != result.end()) {
            fail();
            return std::nullopt;
        }
        return result;
    }

    struct ParsedInventory {
        std::vector<std::string_view> physical_disks;
        std::vector<std::string_view> volumes;
        std::vector<std::string_view> mount_points;
        std::vector<std::string_view> m2_device_ids;
        std::string_view service_state;
        std::string_view service_start_type;
        std::uint64_t process_handle_count = 0;
        std::uint64_t process_thread_count = 0;
        std::uint64_t process_private_bytes = 0;
    };

    std::optional<ParsedInventory> parse_inventory(
        const JsonValue& value, bool allow_empty = false) {
        if (value.type != JsonType::object) {
            fail();
            return std::nullopt;
        }
        ParsedInventory result;
        const auto physical = string_array_member(value, "physical_disks");
        const auto volumes = string_array_member(value, "volumes");
        const auto mounts = string_array_member(value, "mount_points");
        const auto devices = string_array_member(value, "m2_device_ids");
        const auto service = string_member(value, "service_state");
        const auto start_type = string_member(value, "service_start_type");
        const auto handles = uint_member(value, "process_handle_count");
        const auto threads = uint_member(value, "process_thread_count");
        const auto private_bytes = uint_member(value, "process_private_bytes");
        if (!physical || !volumes || !mounts || !devices || !service ||
            !start_type || !handles || !threads || !private_bytes) {
            return std::nullopt;
        }
        result.physical_disks = *physical;
        result.volumes = *volumes;
        result.mount_points = *mounts;
        result.m2_device_ids = *devices;
        result.service_state = *service;
        result.service_start_type = *start_type;
        result.process_handle_count = *handles;
        result.process_thread_count = *threads;
        result.process_private_bytes = *private_bytes;
        const bool empty_snapshot = result.physical_disks.empty() &&
            result.volumes.empty() && result.mount_points.empty() &&
            result.m2_device_ids.empty() && result.service_state.empty() &&
            result.service_start_type.empty() &&
            result.process_handle_count == 0 &&
            result.process_thread_count == 0 &&
            result.process_private_bytes == 0;
        if ((result.service_state.empty() ||
             result.service_start_type.empty()) &&
            !(allow_empty && empty_snapshot)) {
            fail();
            return std::nullopt;
        }
        return result;
    }

    static bool same_strings(
        const std::vector<std::string_view>& left,
        const std::vector<std::string_view>& right) noexcept {
        return left == right;
    }

    static bool same_inventory(
        const ParsedInventory& left,
        const ParsedInventory& right) noexcept {
        return same_strings(left.physical_disks, right.physical_disks) &&
            same_strings(left.volumes, right.volumes) &&
            same_strings(left.mount_points, right.mount_points) &&
            same_strings(left.m2_device_ids, right.m2_device_ids) &&
            left.service_state == right.service_state &&
            left.service_start_type == right.service_start_type;
    }

    struct ParsedDriverState {
        std::string_view driver_state;
        std::string_view service_name;
        std::string_view service_state;
        std::string_view service_start_type;
        std::string_view driver_file_path;
        std::string_view driver_file_version;
        std::string_view driver_sha256;

        bool operator==(const ParsedDriverState&) const noexcept = default;
    };

    std::optional<ParsedDriverState> validate_driver_state(
        const JsonValue& root, std::string_view name) {
        const JsonValue* state = object_member(root, name);
        if (state == nullptr) return std::nullopt;
        const auto driver_state = string_member(*state, "driver_state");
        const auto service_name = string_member(*state, "service_name");
        const auto service_state = string_member(*state, "service_state");
        const auto start_type = string_member(*state, "service_start_type");
        const auto file_path = string_member(*state, "driver_file_path");
        const auto file_version =
            string_member(*state, "driver_file_version");
        const auto file_hash = string_member(*state, "driver_sha256");
        if (driver_state && service_name && service_state && start_type &&
            file_path && file_version && file_hash &&
            driver_state->empty() && service_name->empty() &&
            service_state->empty() && start_type->empty() &&
            file_path->empty() && file_version->empty() &&
            file_hash->empty()) {
            incomplete_ = true;
            return std::nullopt;
        }
        if (!driver_state || !service_name || !service_state || !start_type ||
            !file_path || !file_version || !file_hash || driver_state->empty() ||
            service_name->empty() || service_state->empty() ||
            start_type->empty() || file_path->empty() ||
            file_version->empty() || !is_sha256(*file_hash)) {
            fail();
            return std::nullopt;
        }
        return ParsedDriverState{
            *driver_state, *service_name, *service_state, *start_type,
            *file_path, *file_version, *file_hash};
    }

    void validate_phases(const JsonValue& root) {
        const JsonValue* phases = array_member(root, "phases");
        if (phases == nullptr) return;
        constexpr M2PhaseId required_ids[] = {
            M2PhaseId::capture_baseline,
            M2PhaseId::construct_provider,
            M2PhaseId::start_provider,
            M2PhaseId::create_device,
            M2PhaseId::wait_for_disk,
            M2PhaseId::setup_write,
            M2PhaseId::sample_after_write,
            M2PhaseId::delete_retrim,
            M2PhaseId::sample_after_retrim,
            M2PhaseId::performance,
            M2PhaseId::remove_mounts,
            M2PhaseId::remove_device,
            M2PhaseId::stop_provider,
            M2PhaseId::restore_service,
            M2PhaseId::compare_baseline,
            M2PhaseId::publish_report};
        if (phases->array.size() != std::size(required_ids)) {
            incomplete_ = true;
        }
        for (std::size_t index = 0; index < phases->array.size(); ++index) {
            const auto& phase = phases->array[index];
            if (phase.type != JsonType::object) {
                fail();
                continue;
            }
            const auto id_text = string_member(phase, "phase_id");
            const auto id = id_text ? parse_m2_phase_id(*id_text)
                                    : std::nullopt;
            if (!id || index >= std::size(required_ids) ||
                *id != required_ids[index]) {
                fail();
            }
            const auto started = string_member(phase, "phase_started_utc");
            const auto ended = string_member(phase, "phase_ended_utc");
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
            if (status && *status == M2PhaseStatus::passed &&
                (error && !error->empty())) {
                fail();
            }
            if (status && *status == M2PhaseStatus::failed &&
                (!error || error->empty())) {
                fail();
            }
            if (status && *status == M2PhaseStatus::not_run &&
                (!error || !error->empty())) {
                fail();
            }
            const auto elapsed = uint_member(phase, "phase_elapsed_ms");
            if (status && *status == M2PhaseStatus::not_run) {
                if (!started || !started->empty() || !ended ||
                    !ended->empty() || !elapsed || *elapsed != 0) {
                    fail();
                }
                continue;
            }
            const auto started_seconds =
                started ? parse_utc_seconds(*started) : std::nullopt;
            const auto ended_seconds =
                ended ? parse_utc_seconds(*ended) : std::nullopt;
            if (!started_seconds || !ended_seconds ||
                *ended_seconds < *started_seconds ||
                (run_started_seconds_ &&
                 *started_seconds < *run_started_seconds_) ||
                (run_ended_seconds_ &&
                 *ended_seconds > *run_ended_seconds_)) {
                fail();
            }
        }
    }

    void validate_memory_samples(const JsonValue& root) {
        const JsonValue* samples = array_member(root, "os_memory_samples");
        if (samples == nullptr) return;
        constexpr M2StoreSamplePoint required_points[] = {
            M2StoreSamplePoint::ready,
            M2StoreSamplePoint::after_write,
            M2StoreSamplePoint::after_retrim,
            M2StoreSamplePoint::final};
        constexpr M2PhaseId required_phases[] = {
            M2PhaseId::start_provider,
            M2PhaseId::sample_after_write,
            M2PhaseId::sample_after_retrim,
            M2PhaseId::stop_provider};
        if (samples->array.size() != std::size(required_points)) {
            incomplete_ = true;
        }
        std::optional<std::uint64_t> provider_process_id;
        std::optional<std::uint64_t> previous_elapsed;
        std::array<std::optional<std::uint64_t>, 4> private_bytes{};
        std::array<std::optional<std::uint64_t>, 4> commit_bytes{};
        std::array<std::optional<std::uint64_t>, 4> elapsed_ms{};
        for (std::size_t index = 0; index < samples->array.size(); ++index) {
            const auto& sample = samples->array[index];
            if (sample.type != JsonType::object) {
                fail();
                continue;
            }
            const auto point_text =
                string_member(sample, "memory_sample_point");
            const auto point = point_text
                                   ? parse_m2_store_sample_point(*point_text)
                                   : std::nullopt;
            const auto phase_text = string_member(sample, "memory_phase_id");
            const auto phase = phase_text ? parse_m2_phase_id(*phase_text)
                                          : std::nullopt;
            const auto captured = string_member(sample, "captured_utc");
            const auto process_id =
                uint_member(sample, "provider_process_id");
            const auto elapsed = uint_member(sample, "elapsed_ms");
            uint_member(sample, "working_set_bytes");
            const auto private_value = uint_member(sample, "private_bytes");
            const auto commit_value = uint_member(sample, "commit_bytes");
            if (!point || !phase || !captured || captured->empty() ||
                !process_id || *process_id == 0 || !elapsed ||
                !private_value || !commit_value) {
                fail();
                continue;
            }
            const auto captured_seconds = parse_utc_seconds(*captured);
            if (!captured_seconds ||
                (run_started_seconds_ &&
                 *captured_seconds < *run_started_seconds_) ||
                (run_ended_seconds_ &&
                 *captured_seconds > *run_ended_seconds_)) {
                fail();
                continue;
            }
            if (index >= std::size(required_points)) {
                fail();
                continue;
            }
            if (*point != required_points[index] ||
                *phase != required_phases[index]) {
                fail();
            }
            if (store_sample_timestamps_[index] &&
                *captured != *store_sample_timestamps_[index]) {
                fail();
            }
            if (provider_process_id &&
                *process_id != *provider_process_id) {
                fail();
            }
            provider_process_id = *process_id;
            if (previous_elapsed && *elapsed <= *previous_elapsed) fail();
            previous_elapsed = *elapsed;
            private_bytes[index] = *private_value;
            commit_bytes[index] = *commit_value;
            elapsed_ms[index] = *elapsed;
        }
        if (std::all_of(
                private_bytes.begin(), private_bytes.end(),
                [](const auto& value) { return value.has_value(); }) &&
            std::all_of(
                commit_bytes.begin(), commit_bytes.end(),
                [](const auto& value) { return value.has_value(); }) &&
            std::all_of(
                elapsed_ms.begin(), elapsed_ms.end(),
                [](const auto& value) { return value.has_value(); })) {
            constexpr std::uint64_t ready_limit = 128ULL << 20U;
            constexpr std::uint64_t write_growth_limit =
                (11ULL << 30U) / 10U;
            if (*private_bytes[0] > ready_limit ||
                *commit_bytes[0] > ready_limit ||
                *private_bytes[1] <= *private_bytes[0] ||
                *commit_bytes[1] <= *commit_bytes[0] ||
                *private_bytes[1] - *private_bytes[0] > write_growth_limit ||
                *commit_bytes[1] - *commit_bytes[0] > write_growth_limit ||
                *elapsed_ms[2] - *elapsed_ms[1] > 10'000) {
                failure_ = true;
            }
            const auto private_growth =
                *private_bytes[1] - *private_bytes[0];
            const auto commit_growth = *commit_bytes[1] - *commit_bytes[0];
            const auto private_remaining = *private_bytes[2] > *private_bytes[0]
                                               ? *private_bytes[2] -
                                                     *private_bytes[0]
                                               : 0;
            const auto commit_remaining = *commit_bytes[2] > *commit_bytes[0]
                                              ? *commit_bytes[2] -
                                                    *commit_bytes[0]
                                              : 0;
            if (private_remaining > private_growth / 10U ||
                commit_remaining > commit_growth / 10U) {
                failure_ = true;
            }
        }
    }

    struct ParsedPerformanceResult {
        std::string_view operation;
        std::string_view machine_fingerprint;
        std::string_view build_fingerprint;
        std::string_view process_policy;
        std::string_view raw_io_flags;
        std::uint64_t region_offset{};
        std::uint64_t sequential_rate{};
        std::uint64_t random_rate{};
    };

    bool raw_rate_matches(
        std::uint64_t units,
        std::uint64_t frequency,
        std::uint64_t elapsed,
        std::uint64_t reported) const noexcept {
        return frequency != 0 && elapsed != 0 &&
            units <= (std::numeric_limits<std::uint64_t>::max)() / frequency &&
            (units * frequency) / elapsed == reported;
    }

    void validate_store_samples(const JsonValue& root) {
        const JsonValue* samples = array_member(root, "store_stats_samples");
        if (samples == nullptr) return;
        constexpr M2StoreSamplePoint required_points[] = {
            M2StoreSamplePoint::ready,
            M2StoreSamplePoint::after_write,
            M2StoreSamplePoint::after_retrim,
            M2StoreSamplePoint::final};
        if (samples->array.size() != std::size(required_points)) {
            incomplete_ = true;
        }
        std::optional<std::uint64_t> ready_committed;
        std::optional<std::uint64_t> write_committed;
        std::optional<std::uint64_t> retrim_committed;
        std::optional<std::int64_t> previous_captured_seconds;
        for (std::size_t index = 0; index < samples->array.size(); ++index) {
            const auto& sample = samples->array[index];
            if (sample.type != JsonType::object) {
                fail();
                continue;
            }
            const auto point_text = string_member(sample, "sample_point");
            const auto point = point_text
                                   ? parse_m2_store_sample_point(*point_text)
                                   : std::nullopt;
            const auto captured = string_member(sample, "captured_utc");
            const JsonValue* stats = object_member(sample, "stats");
            if (!point || !captured || captured->empty() || stats == nullptr) {
                fail();
                continue;
            }
            const auto captured_seconds = parse_utc_seconds(*captured);
            if (!captured_seconds ||
                (previous_captured_seconds &&
                 *captured_seconds < *previous_captured_seconds) ||
                (run_started_seconds_ &&
                 *captured_seconds < *run_started_seconds_) ||
                (run_ended_seconds_ &&
                 *captured_seconds > *run_ended_seconds_)) {
                fail();
                continue;
            }
            previous_captured_seconds = *captured_seconds;
            if (index >= std::size(required_points) ||
                *point != required_points[index]) {
                fail();
            }
            if (index < store_sample_timestamps_.size()) {
                store_sample_timestamps_[index] = *captured;
            }
            const auto logical = uint_member(*stats, "logical_size_bytes");
            const auto reserved = uint_member(*stats, "reserved_bytes");
            const auto committed = uint_member(*stats, "committed_bytes");
            for (const auto name : {
                     "resident_clean_blocks", "resident_dirty_blocks",
                     "zero_clean_blocks", "zero_dirty_blocks",
                     "loading_blocks", "evicted_blocks_total",
                     "backing_reads_total", "backing_read_bytes_total",
                     "uncached_reads_total", "unmap_bytes_total",
                     "zero_bytes_total", "hard_limit_rejections",
                     "commit_failures", "decommit_failures",
                     "synchronous_pressure_passes_total",
                     "synchronous_pressure_waits_total",
                     "synchronous_pressure_no_progress_total",
                     "synchronous_pressure_cancellations_total",
                     "synchronous_pressure_deadlines_total",
                     "commit_reservation_rollbacks_total",
                     "snapshot_records_total", "snapshot_version_conflicts",
                     "backing_generation", "backing_view_installs_total",
                     "backing_generation_conflicts",
                     "stale_backing_load_retries"}) {
                uint_member(*stats, name);
            }
            const auto generation = uint_member(*stats, "backing_generation");
            const auto installs =
                uint_member(*stats, "backing_view_installs_total");
            if (logical && reserved && committed && disk_size_bytes_ &&
                hard_limit_bytes_ &&
                (*logical != *disk_size_bytes_ || *reserved != *logical ||
                 *committed > *hard_limit_bytes_)) {
                fail();
            }
            if (generation && current_backing_generation_ &&
                *generation != *current_backing_generation_) {
                fail();
            }
            if (installs && *installs != 0) fail();
            if (installs && initial_backing_generation_ &&
                current_backing_generation_ && *installs == 0 &&
                *current_backing_generation_ != *initial_backing_generation_) {
                fail();
            }
            if (*point == M2StoreSamplePoint::ready) ready_committed = committed;
            if (*point == M2StoreSamplePoint::after_write)
                write_committed = committed;
            if (*point == M2StoreSamplePoint::after_retrim)
                retrim_committed = committed;
        }
        if (ready_committed && write_committed && retrim_committed &&
            (*write_committed <= *ready_committed ||
             *retrim_committed >= *write_committed)) {
            failure_ = true;
        }
    }

    std::optional<std::uint64_t> basis_points(
        std::uint64_t candidate,
        std::uint64_t baseline) const noexcept {
        if (baseline == 0 ||
            candidate >
                (std::numeric_limits<std::uint64_t>::max)() / 10'000) {
            return std::nullopt;
        }
        return (candidate * 10'000) / baseline;
    }

    std::optional<ParsedPerformanceResult> validate_performance_result(
        const JsonValue& performance,
        std::string_view name,
        bool require_measurement) {
        const JsonValue* result = object_member(performance, name);
        if (result == nullptr) return std::nullopt;
        const auto methodology = string_member(*result, "methodology_id");
        const auto operation = string_member(*result, "operation");
        const auto queue_depth = uint_member(*result, "queue_depth");
        const auto region_offset = uint_member(*result, "region_offset");
        const auto region_bytes = uint_member(*result, "region_bytes");
        const auto sequential_io =
            uint_member(*result, "sequential_io_bytes");
        const auto random_io = uint_member(*result, "random_io_bytes");
        const auto seed = uint_member(*result, "random_seed");
        const auto sequential_warmup =
            uint_member(*result, "sequential_warmup_operations");
        const auto random_warmup =
            uint_member(*result, "random_warmup_operations");

        const JsonValue* policy = object_member(*result, "policy");
        std::optional<std::string_view> machine;
        std::optional<std::string_view> build;
        std::optional<std::string_view> process;
        std::optional<std::string_view> raw_io_flags;
        if (policy != nullptr) {
            machine = string_member(*policy, "machine_fingerprint");
            build = string_member(*policy, "build_fingerprint");
            process = string_member(*policy, "process_policy");
            raw_io_flags = string_member(*policy, "raw_io_flags");
        }

        const JsonValue* sequential = object_member(*result, "sequential");
        std::optional<std::uint64_t> sequential_bytes;
        std::optional<std::uint64_t> sequential_operations;
        std::optional<std::uint64_t> sequential_elapsed;
        std::optional<std::uint64_t> sequential_frequency;
        std::optional<std::uint64_t> sequential_rate;
        if (sequential != nullptr) {
            sequential_bytes = uint_member(*sequential, "bytes");
            sequential_operations = uint_member(*sequential, "operations");
            sequential_elapsed = uint_member(*sequential, "elapsed_ticks");
            sequential_frequency =
                uint_member(*sequential, "clock_frequency_hz");
            sequential_rate =
                uint_member(*sequential, "bytes_per_second");
        }

        const JsonValue* random = object_member(*result, "random");
        std::optional<std::uint64_t> random_bytes;
        std::optional<std::uint64_t> random_operations;
        std::optional<std::uint64_t> random_elapsed;
        std::optional<std::uint64_t> random_frequency;
        std::optional<std::uint64_t> random_rate;
        if (random != nullptr) {
            random_bytes = uint_member(*random, "bytes");
            random_operations = uint_member(*random, "operations");
            random_elapsed = uint_member(*random, "elapsed_ticks");
            random_frequency = uint_member(*random, "clock_frequency_hz");
            random_rate = uint_member(*random, "operations_per_second");
        }

        if (!methodology || !operation || !queue_depth || !region_offset ||
            !region_bytes || !sequential_io || !random_io || !seed ||
            !sequential_warmup || !random_warmup || !machine || !build ||
            !process || !raw_io_flags || !sequential_bytes ||
            !sequential_operations ||
            !sequential_elapsed || !sequential_frequency || !sequential_rate ||
            !random_bytes || !random_operations || !random_elapsed ||
            !random_frequency || !random_rate) {
            return std::nullopt;
        }

        if (require_measurement) {
            if (*methodology != kM2PerformanceMethodologyId ||
                (*operation != "read" && *operation != "write") ||
                *queue_depth != 1 || (*region_offset % (64ull << 10)) != 0 ||
                *region_bytes != kM2SequentialRegionBytes ||
                *sequential_io != kM2SequentialIoBytes ||
                *random_io != kM2RandomIoBytes ||
                *seed != kM2PerformanceRandomSeed ||
                *sequential_warmup != kM2SequentialWarmupOperationCount ||
                *random_warmup != kM2RandomWarmupOperationCount ||
                machine->empty() || build->empty() || process->empty() ||
                raw_io_flags->empty() ||
                *sequential_bytes != kM2SequentialRegionBytes ||
                *sequential_operations != kM2SequentialOperationCount ||
                *random_bytes !=
                    kM2RandomIoBytes * kM2RandomOperationCount ||
                *random_operations != kM2RandomOperationCount ||
                *sequential_frequency != *random_frequency ||
                !raw_rate_matches(*sequential_bytes, *sequential_frequency,
                                  *sequential_elapsed, *sequential_rate) ||
                !raw_rate_matches(*random_operations, *random_frequency,
                                  *random_elapsed, *random_rate)) {
                fail();
            }
        }

        return ParsedPerformanceResult{
            *operation, *machine, *build, *process, *raw_io_flags,
            *region_offset,
            *sequential_rate, *random_rate};
    }

    void validate_performance(const JsonValue& root) {
        const JsonValue* performance = object_member(root, "performance");
        if (performance == nullptr) return;
        const auto gate_text = string_member(*performance, "gate_status");
        const auto gate = gate_text
                              ? parse_m2_performance_gate_status(*gate_text)
                              : std::nullopt;
        if (!gate) {
            fail();
            return;
        }

        const bool measured = *gate != M2PerformanceGateStatus::not_evaluated;
        auto fixed = validate_performance_result(
            *performance, "fixed", measured);
        auto dynamic = validate_performance_result(
            *performance, "dynamic", measured);
        const JsonValue* comparison = object_member(*performance, "comparison");
        if (comparison == nullptr) return;
        const auto sequential_ratio =
            uint_member(*comparison, "sequential_ratio_basis_points");
        const auto random_ratio =
            uint_member(*comparison, "random_ratio_basis_points");
        const auto sequential_threshold =
            uint_member(*comparison, "sequential_threshold_basis_points");
        const auto random_threshold =
            uint_member(*comparison, "random_threshold_basis_points");
        const auto sequential_pass = bool_member(*comparison, "sequential_pass");
        const auto random_pass = bool_member(*comparison, "random_pass");
        const auto overall_pass = bool_member(*comparison, "overall_pass");

        if (!measured) {
            incomplete_ = true;
            return;
        }
        if (!fixed || !dynamic || !sequential_ratio || !random_ratio ||
            !sequential_threshold || !random_threshold || !sequential_pass ||
            !random_pass || !overall_pass) {
            return;
        }
        if (fixed->operation != dynamic->operation ||
            fixed->region_offset != dynamic->region_offset ||
            fixed->machine_fingerprint != dynamic->machine_fingerprint ||
            fixed->build_fingerprint != dynamic->build_fingerprint ||
            fixed->process_policy != dynamic->process_policy ||
            fixed->raw_io_flags != dynamic->raw_io_flags ||
            !machine_fingerprint_ || !build_fingerprint_ ||
            fixed->machine_fingerprint != *machine_fingerprint_ ||
            fixed->build_fingerprint != *build_fingerprint_) {
            fail();
        }
        const auto expected_sequential =
            basis_points(dynamic->sequential_rate, fixed->sequential_rate);
        const auto expected_random =
            basis_points(dynamic->random_rate, fixed->random_rate);
        if (!expected_sequential || !expected_random ||
            *sequential_ratio != *expected_sequential ||
            *random_ratio != *expected_random ||
            *sequential_threshold != kM2SequentialThresholdBasisPoints ||
            *random_threshold != kM2RandomThresholdBasisPoints) {
            fail();
            return;
        }
        const bool expected_sequential_pass =
            *sequential_ratio >= *sequential_threshold;
        const bool expected_random_pass = *random_ratio >= *random_threshold;
        const bool expected_overall =
            expected_sequential_pass && expected_random_pass;
        if (*sequential_pass != expected_sequential_pass ||
            *random_pass != expected_random_pass ||
            *overall_pass != expected_overall ||
            (*gate == M2PerformanceGateStatus::passed && !expected_overall) ||
            (*gate == M2PerformanceGateStatus::failed && expected_overall)) {
            fail();
        }
        if (*gate == M2PerformanceGateStatus::failed) failure_ = true;
    }

    void validate_device(const JsonValue& root) {
        const JsonValue* device = object_member(root, "device_identity");
        if (device == nullptr) return;
        const auto session = string_member(*device, "provider_session_id");
        const auto native_path = string_member(*device, "native_object_path");
        const auto raw_text = string_member(*device, "device_id_raw");
        const auto path = uint_member(*device, "path_id");
        const auto target = uint_member(*device, "target_id");
        const auto lun = uint_member(*device, "lun");
        const auto create_flags = uint_member(*device, "create_flags");
        const auto query_flags = uint_member(*device, "query_flags");
        const auto confirmed =
            bool_member(*device, "confirmed_by_query_device");
        const auto physical_number =
            uint_member(*device, "physical_drive_number");
        const auto physical_path =
            string_member(*device, "physical_drive_path");
        const auto disk_length = uint_member(*device, "disk_length_bytes");
        const auto bus_type = string_member(*device, "bus_type");
        const auto vendor = string_member(*device, "vendor_id");
        const auto product = string_member(*device, "product_id");
        const auto serial = string_member(*device, "serial_number");
        const auto unique_id = string_member(*device, "disk_unique_id");
        const auto volume_guids = string_array_member(*device, "volume_guids");
        const auto mount_points = string_array_member(*device, "mount_points");
        if (!session || !native_path || !raw_text || !path || !target || !lun ||
            !create_flags || !query_flags || !confirmed || !physical_number ||
            !physical_path || !disk_length || !bus_type || !vendor || !product ||
            !serial || !unique_id || !volume_guids || !mount_points) {
            return;
        }
        const bool no_identity_captured =
            session->empty() && native_path->empty() && raw_text->empty() &&
            *path == 0 && *target == 0 && *lun == 0 && *create_flags == 0 &&
            *query_flags == 0 && !*confirmed && *physical_number == 0 &&
            physical_path->empty() && *disk_length == 0 && bus_type->empty() &&
            vendor->empty() && product->empty() && serial->empty() &&
            unique_id->empty() && volume_guids->empty() &&
            mount_points->empty();
        if (no_identity_captured) {
            incomplete_ = true;
            return;
        }
        if (session->empty() || native_path->empty() || raw_text->empty() ||
            bus_type->empty() || vendor->empty() || product->empty() ||
            serial->empty() || unique_id->empty() || volume_guids->empty() ||
            mount_points->empty() || *path > 0xFF || *target > 0xFF ||
            *lun > 0xFF || *create_flags != aim::kM0CreateFlags ||
            *query_flags != aim::kM1QueryFlags || !*confirmed ||
            *physical_number == 0 || !disk_size_bytes_ ||
            *disk_length != *disk_size_bytes_) {
            fail();
            return;
        }
        std::uint32_t raw_id = 0;
        const auto parsed = std::from_chars(
            raw_text->data(), raw_text->data() + raw_text->size(), raw_id);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != raw_text->data() + raw_text->size() ||
            raw_id == 0 || raw_id == aim::kAllDevices) {
            fail();
            return;
        }
        const auto decoded = aim::decode_device_address({raw_id});
        if (decoded.path_id != *path || decoded.target_id != *target ||
            decoded.lun != *lun ||
            *physical_path !=
                std::string(R"(\\.\PhysicalDrive)") +
                    std::to_string(*physical_number)) {
            fail();
        }
    }

    void validate_cleanup(const JsonValue& root) {
        const JsonValue* actions = array_member(root, "cleanup_actions");
        if (actions == nullptr) return;
        constexpr M2CleanupActionId required_ids[] = {
            M2CleanupActionId::remove_mounts,
            M2CleanupActionId::remove_device,
            M2CleanupActionId::stop_provider,
            M2CleanupActionId::restore_service};
        if (actions->array.size() != std::size(required_ids)) {
            incomplete_ = true;
        }
        for (std::size_t index = 0; index < actions->array.size(); ++index) {
            const auto& action = actions->array[index];
            if (action.type != JsonType::object) {
                fail();
                continue;
            }
            const auto id_text = string_member(action, "cleanup_action_id");
            const auto id = id_text ? parse_m2_cleanup_action_id(*id_text)
                                    : std::nullopt;
            if (!id || index >= std::size(required_ids) ||
                *id != required_ids[index]) {
                fail();
            }
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
            if (result && *result == M2CleanupResult::succeeded &&
                error && !error->empty()) {
                fail();
            }
            if (result && *result == M2CleanupResult::failed &&
                (!error || error->empty())) {
                fail();
            }
            if (result && *result == M2CleanupResult::not_attempted &&
                (!error || !error->empty())) {
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
        const auto exact_match = bool_member(*baseline, "exact_match");
        const auto resources_ok =
            bool_member(*baseline, "resources_within_tolerance");
        const auto private_tolerance =
            uint_member(*baseline, "private_bytes_tolerance");
        const auto before = string_member(*baseline, "before_fingerprint");
        const auto after = string_member(*baseline, "after_fingerprint");
        const JsonValue* before_value = object_member(*baseline, "before");
        const JsonValue* after_value = object_member(*baseline, "after");
        const JsonValue* diff_value = object_member(*baseline, "diff");
        if (!comparison || !exact_match || !resources_ok ||
            !private_tolerance || !before || !after ||
            before_value == nullptr || after_value == nullptr ||
            diff_value == nullptr) {
            return;
        }
        const bool not_compared =
            *comparison == M2BaselineComparison::not_compared;
        const auto before_inventory = before_value
                                          ? parse_inventory(
                                                *before_value, not_compared)
                                          : std::nullopt;
        const auto after_inventory = after_value
                                         ? parse_inventory(
                                               *after_value, not_compared)
                                         : std::nullopt;
        std::optional<std::vector<std::string_view>> added;
        std::optional<std::vector<std::string_view>> removed;
        std::optional<std::vector<std::string_view>> changed;
        if (diff_value != nullptr) {
            added = string_array_member(*diff_value, "added");
            removed = string_array_member(*diff_value, "removed");
            changed = string_array_member(*diff_value, "changed");
        }
        if (!before_inventory ||
            !after_inventory || !added || !removed || !changed) {
            return;
        }
        if (not_compared) {
            incomplete_ = true;
            return;
        }
        const bool inventory_matches =
            same_inventory(*before_inventory, *after_inventory);
        const bool diff_empty =
            added->empty() && removed->empty() && changed->empty();
        const bool private_within =
            before_inventory->process_private_bytes <=
                (std::numeric_limits<std::uint64_t>::max)() -
                    *private_tolerance &&
            after_inventory->process_private_bytes <=
                before_inventory->process_private_bytes + *private_tolerance;
        const bool resources_match =
            after_inventory->process_handle_count <=
                before_inventory->process_handle_count &&
            after_inventory->process_thread_count <=
                before_inventory->process_thread_count &&
            private_within;
        if (*resources_ok != resources_match) fail();
        if (*comparison == M2BaselineComparison::match) {
            if (!*exact_match || !*resources_ok || before->empty() ||
                *before != *after || !inventory_matches || !diff_empty) {
                fail();
            }
            return;
        }
        failure_ = true;
        if (*exact_match || before->empty() || after->empty() ||
            (*before == *after && inventory_matches && diff_empty)) {
            fail();
        }
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
            const auto phase = string_member(failure, "failure_phase");
            uint_member(failure, "failure_code");
            const auto message = string_member(failure, "failure_message");
            if (!phase || phase->empty() || !message || message->empty()) {
                fail();
            }
        }
    }

    bool valid_ = true;
    bool incomplete_ = false;
    bool failure_ = false;
    std::optional<M2RunMode> run_mode_;
    std::optional<std::uint64_t> disk_size_bytes_;
    std::optional<std::uint64_t> hard_limit_bytes_;
    std::optional<std::uint64_t> initial_backing_generation_;
    std::optional<std::uint64_t> current_backing_generation_;
    std::optional<std::string_view> machine_fingerprint_;
    std::optional<std::string_view> build_fingerprint_;
    std::optional<std::int64_t> run_started_seconds_;
    std::optional<std::int64_t> run_ended_seconds_;
    std::array<std::optional<std::string_view>, 4>
        store_sample_timestamps_{};
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
    case M2RunMode::real: return "REAL";
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

std::string_view to_string(M2StoreSamplePoint value) noexcept {
    switch (value) {
    case M2StoreSamplePoint::ready: return "READY";
    case M2StoreSamplePoint::after_write: return "AFTER-WRITE";
    case M2StoreSamplePoint::after_retrim: return "AFTER-RETRIM";
    case M2StoreSamplePoint::final: return "FINAL";
    }
    return {};
}

std::string_view to_string(M2PhaseId value) noexcept {
    switch (value) {
    case M2PhaseId::capture_baseline: return "CAPTURE-BASELINE";
    case M2PhaseId::construct_provider: return "CONSTRUCT-PROVIDER";
    case M2PhaseId::start_provider: return "START-PROVIDER";
    case M2PhaseId::create_device: return "CREATE-DEVICE";
    case M2PhaseId::wait_for_disk: return "WAIT-FOR-DISK";
    case M2PhaseId::setup_write: return "SETUP-WRITE";
    case M2PhaseId::sample_after_write: return "SAMPLE-AFTER-WRITE";
    case M2PhaseId::delete_retrim: return "DELETE-RETRIM";
    case M2PhaseId::sample_after_retrim: return "SAMPLE-AFTER-RETRIM";
    case M2PhaseId::performance: return "PERFORMANCE";
    case M2PhaseId::remove_mounts: return "REMOVE-MOUNTS";
    case M2PhaseId::remove_device: return "REMOVE-DEVICE";
    case M2PhaseId::stop_provider: return "STOP-PROVIDER";
    case M2PhaseId::restore_service: return "RESTORE-SERVICE";
    case M2PhaseId::compare_baseline: return "COMPARE-BASELINE";
    case M2PhaseId::publish_report: return "PUBLISH-REPORT";
    }
    return {};
}

std::string_view to_string(M2CleanupActionId value) noexcept {
    switch (value) {
    case M2CleanupActionId::remove_mounts: return "REMOVE-MOUNTS";
    case M2CleanupActionId::remove_device: return "REMOVE-DEVICE";
    case M2CleanupActionId::stop_provider: return "STOP-PROVIDER";
    case M2CleanupActionId::restore_service: return "RESTORE-SERVICE";
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
    return parse_exact<M2RunMode>(
        value, {{"FAKE", M2RunMode::fake}, {"REAL", M2RunMode::real}});
}

std::optional<M2InitialState> parse_m2_initial_state(
    std::string_view value) noexcept {
    return parse_exact<M2InitialState>(
        value, {{"EMPTY", M2InitialState::empty},
                {"SEEDED", M2InitialState::seeded}});
}

std::optional<M2StoreSamplePoint> parse_m2_store_sample_point(
    std::string_view value) noexcept {
    return parse_exact<M2StoreSamplePoint>(
        value, {{"READY", M2StoreSamplePoint::ready},
                {"AFTER-WRITE", M2StoreSamplePoint::after_write},
                {"AFTER-RETRIM", M2StoreSamplePoint::after_retrim},
                {"FINAL", M2StoreSamplePoint::final}});
}

std::optional<M2PhaseId> parse_m2_phase_id(std::string_view value) noexcept {
    return parse_exact<M2PhaseId>(
        value,
        {{"CAPTURE-BASELINE", M2PhaseId::capture_baseline},
         {"CONSTRUCT-PROVIDER", M2PhaseId::construct_provider},
         {"START-PROVIDER", M2PhaseId::start_provider},
         {"CREATE-DEVICE", M2PhaseId::create_device},
         {"WAIT-FOR-DISK", M2PhaseId::wait_for_disk},
         {"SETUP-WRITE", M2PhaseId::setup_write},
         {"SAMPLE-AFTER-WRITE", M2PhaseId::sample_after_write},
         {"DELETE-RETRIM", M2PhaseId::delete_retrim},
         {"SAMPLE-AFTER-RETRIM", M2PhaseId::sample_after_retrim},
         {"PERFORMANCE", M2PhaseId::performance},
         {"REMOVE-MOUNTS", M2PhaseId::remove_mounts},
         {"REMOVE-DEVICE", M2PhaseId::remove_device},
         {"STOP-PROVIDER", M2PhaseId::stop_provider},
         {"RESTORE-SERVICE", M2PhaseId::restore_service},
         {"COMPARE-BASELINE", M2PhaseId::compare_baseline},
         {"PUBLISH-REPORT", M2PhaseId::publish_report}});
}

std::optional<M2CleanupActionId> parse_m2_cleanup_action_id(
    std::string_view value) noexcept {
    return parse_exact<M2CleanupActionId>(
        value,
        {{"REMOVE-MOUNTS", M2CleanupActionId::remove_mounts},
         {"REMOVE-DEVICE", M2CleanupActionId::remove_device},
         {"STOP-PROVIDER", M2CleanupActionId::stop_provider},
         {"RESTORE-SERVICE", M2CleanupActionId::restore_service}});
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
    writer.key("executable_path");
    writer.string_value(report.build_toolchain.executable_path);
    writer.key("executable_sha256");
    writer.string_value(report.build_toolchain.executable_sha256);
    writer.key("build_fingerprint");
    writer.string_value(report.build_toolchain.build_fingerprint);
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
    writer.key("windows_build");
    writer.string_value(report.machine_os.windows_build);
    writer.key("cpu_model");
    writer.string_value(report.machine_os.cpu_model);
    writer.key("logical_processor_count");
    writer.uint_value(report.machine_os.logical_processor_count);
    writer.key("physical_memory_bytes");
    writer.uint_value(report.machine_os.physical_memory_bytes);
    writer.key("machine_fingerprint");
    writer.string_value(report.machine_os.machine_fingerprint);
    writer.end_object();

    writer.key("state_before"); write_driver_state(writer, report.state_before);
    writer.key("state_after"); write_driver_state(writer, report.state_after);

    writer.key("geometry_options");
    writer.start_object();
    writer.key("disk_size_bytes");
    writer.uint_value(report.geometry_options.disk_size_bytes);
    writer.key("sector_size_bytes");
    writer.uint_value(report.geometry_options.sector_size_bytes);
    writer.key("block_size_bytes");
    writer.uint_value(report.geometry_options.block_size_bytes);
    writer.key("soft_limit_bytes");
    writer.uint_value(report.geometry_options.soft_limit_bytes);
    writer.key("hard_limit_bytes");
    writer.uint_value(report.geometry_options.hard_limit_bytes);
    writer.key("eviction_low_watermark_bytes");
    writer.uint_value(report.geometry_options.eviction_low_watermark_bytes);
    writer.key("max_snapshot_batch_bytes");
    writer.uint_value(report.geometry_options.max_snapshot_batch_bytes);
    writer.key("max_snapshot_batch_records");
    writer.uint_value(report.geometry_options.max_snapshot_batch_records);
    writer.key("lock_stripe_count");
    writer.uint_value(report.geometry_options.lock_stripe_count);
    writer.key("test_policy_selected");
    writer.boolean_value(report.geometry_options.test_policy_selected);
    writer.key("backing_kind");
    writer.string_value(report.geometry_options.backing_kind);
    writer.end_object();

    writer.key("state");
    writer.start_object();
    writer.key("initial_state");
    writer.string_value(to_string(report.state.initial_state));
    writer.key("initial_backing_generation");
    writer.uint_value(report.state.initial_backing_generation);
    writer.key("current_backing_generation");
    writer.uint_value(report.state.current_backing_generation);
    writer.key("durable_successor_installed");
    writer.boolean_value(report.state.durable_successor_installed);
    writer.key("durable_successor_generation_present");
    writer.boolean_value(report.state.durable_successor_generation_present);
    writer.key("durable_successor_generation");
    writer.uint_value(report.state.durable_successor_generation);
    writer.key("durability_scope_statement");
    writer.string_value(report.state.durability_scope_statement);
    writer.end_object();

    writer.key("phases");
    writer.start_array();
    for (const auto& phase : report.phases) {
        writer.start_object();
        writer.key("phase_id"); writer.string_value(to_string(phase.id));
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

    writer.key("store_stats_samples");
    writer.start_array();
    for (const auto& sample : report.store_stats_samples) {
        writer.start_object();
        writer.key("sample_point");
        writer.string_value(to_string(sample.point));
        writer.key("captured_utc");
        writer.string_value(sample.captured_utc);
        writer.key("stats");
        write_store_stats(writer, sample.stats);
        writer.end_object();
    }
    writer.end_array();

    writer.key("os_memory_samples");
    writer.start_array();
    for (const auto& sample : report.os_memory_samples) {
        writer.start_object();
        writer.key("memory_sample_point");
        writer.string_value(to_string(sample.point));
        writer.key("memory_phase_id");
        writer.string_value(to_string(sample.phase_id));
        writer.key("captured_utc"); writer.string_value(sample.captured_utc);
        writer.key("provider_process_id");
        writer.uint_value(sample.provider_process_id);
        writer.key("elapsed_ms"); writer.uint_value(sample.elapsed_ms);
        writer.key("working_set_bytes");
        writer.uint_value(sample.working_set_bytes);
        writer.key("private_bytes"); writer.uint_value(sample.private_bytes);
        writer.key("commit_bytes"); writer.uint_value(sample.commit_bytes);
        writer.end_object();
    }
    writer.end_array();

    writer.key("performance");
    writer.start_object();
    writer.key("fixed");
    write_performance_result(writer, report.performance.fixed);
    writer.key("dynamic");
    write_performance_result(writer, report.performance.dynamic);
    writer.key("comparison");
    writer.start_object();
    writer.key("sequential_ratio_basis_points");
    writer.uint_value(
        report.performance.comparison.sequential_ratio_basis_points);
    writer.key("random_ratio_basis_points");
    writer.uint_value(report.performance.comparison.random_ratio_basis_points);
    writer.key("sequential_threshold_basis_points");
    writer.uint_value(
        report.performance.comparison.sequential_threshold_basis_points);
    writer.key("random_threshold_basis_points");
    writer.uint_value(
        report.performance.comparison.random_threshold_basis_points);
    writer.key("sequential_pass");
    writer.boolean_value(report.performance.comparison.sequential_pass);
    writer.key("random_pass");
    writer.boolean_value(report.performance.comparison.random_pass);
    writer.key("overall_pass");
    writer.boolean_value(report.performance.comparison.overall_pass);
    writer.end_object();
    writer.key("gate_status");
    writer.string_value(to_string(report.performance.gate_status));
    writer.end_object();

    writer.key("device_identity");
    writer.start_object();
    writer.key("provider_session_id");
    writer.string_value(report.device_identity.provider_session_id);
    writer.key("native_object_path");
    writer.string_value(report.device_identity.native_object_path);
    writer.key("device_id_raw");
    writer.string_value(report.device_identity.device_id_raw);
    writer.key("path_id"); writer.uint_value(report.device_identity.path_id);
    writer.key("target_id"); writer.uint_value(report.device_identity.target_id);
    writer.key("lun"); writer.uint_value(report.device_identity.lun);
    writer.key("create_flags");
    writer.uint_value(report.device_identity.create_flags);
    writer.key("query_flags");
    writer.uint_value(report.device_identity.query_flags);
    writer.key("confirmed_by_query_device");
    writer.boolean_value(report.device_identity.confirmed_by_query_device);
    writer.key("physical_drive_number");
    writer.uint_value(report.device_identity.physical_drive_number);
    writer.key("physical_drive_path");
    writer.string_value(report.device_identity.physical_drive_path);
    writer.key("disk_length_bytes");
    writer.uint_value(report.device_identity.disk_length_bytes);
    writer.key("bus_type");
    writer.string_value(report.device_identity.bus_type);
    writer.key("vendor_id");
    writer.string_value(report.device_identity.vendor_id);
    writer.key("product_id");
    writer.string_value(report.device_identity.product_id);
    writer.key("serial_number");
    writer.string_value(report.device_identity.serial_number);
    writer.key("disk_unique_id");
    writer.string_value(report.device_identity.disk_unique_id);
    writer.key("volume_guids");
    write_string_array(writer, report.device_identity.volume_guids);
    writer.key("mount_points");
    write_string_array(writer, report.device_identity.mount_points);
    writer.end_object();

    writer.key("cleanup_actions");
    writer.start_array();
    for (const auto& action : report.cleanup_actions) {
        writer.start_object();
        writer.key("cleanup_action_id");
        writer.string_value(to_string(action.id));
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
    writer.key("exact_match");
    writer.boolean_value(report.final_baseline_comparison.exact_match);
    writer.key("resources_within_tolerance");
    writer.boolean_value(
        report.final_baseline_comparison.resources_within_tolerance);
    writer.key("private_bytes_tolerance");
    writer.uint_value(
        report.final_baseline_comparison.private_bytes_tolerance);
    writer.key("before_fingerprint");
    writer.string_value(report.final_baseline_comparison.before_fingerprint);
    writer.key("after_fingerprint");
    writer.string_value(report.final_baseline_comparison.after_fingerprint);
    writer.key("before");
    write_inventory(writer, report.final_baseline_comparison.before);
    writer.key("after");
    write_inventory(writer, report.final_baseline_comparison.after);
    writer.key("diff");
    write_inventory_diff(writer, report.final_baseline_comparison.diff);
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
  "required": ["schema_version", "run", "build_toolchain", "accepted_m1_baseline", "machine_os", "state_before", "state_after", "geometry_options", "state", "phases", "io", "store_stats_samples", "os_memory_samples", "performance", "device_identity", "cleanup_actions", "final_baseline_comparison", "failures", "verdict"],
  "properties": {
    "schema_version": {"type": "integer", "const": 1},
    "run": {"type": "object", "required": ["run_id", "mode", "started_utc", "ended_utc"], "properties": {"run_id": {"type": "string"}, "mode": {"enum": ["FAKE", "REAL"]}, "started_utc": {"type": "string"}, "ended_utc": {"type": "string"}}},
    "build_toolchain": {"type": "object", "required": ["build_version", "git_commit", "configuration", "compiler", "compiler_version", "cmake_version", "executable_path", "executable_sha256", "build_fingerprint"], "properties": {"build_version": {"type": "string"}, "git_commit": {"type": "string"}, "configuration": {"type": "string"}, "compiler": {"type": "string"}, "compiler_version": {"type": "string"}, "cmake_version": {"type": "string"}, "executable_path": {"type": "string"}, "executable_sha256": {"type": "string"}, "build_fingerprint": {"type": "string"}}},
    "accepted_m1_baseline": {"type": "object", "required": ["report_path", "report_sha256", "m1_schema_version", "m1_verdict", "accepted"], "properties": {"report_path": {"type": "string"}, "report_sha256": {"type": "string"}, "m1_schema_version": {"$ref": "#/$defs/uint64"}, "m1_verdict": {"const": "GO"}, "accepted": {"type": "boolean"}}},
    "machine_os": {"type": "object", "required": ["machine_name", "os_name", "os_version", "architecture", "windows_build", "cpu_model", "logical_processor_count", "physical_memory_bytes", "machine_fingerprint"], "properties": {"machine_name": {"type": "string"}, "os_name": {"type": "string"}, "os_version": {"type": "string"}, "architecture": {"type": "string"}, "windows_build": {"type": "string"}, "cpu_model": {"type": "string"}, "logical_processor_count": {"$ref": "#/$defs/uint64"}, "physical_memory_bytes": {"$ref": "#/$defs/uint64"}, "machine_fingerprint": {"type": "string"}}},
    "state_before": {"$ref": "#/$defs/driver_service_state"},
    "state_after": {"$ref": "#/$defs/driver_service_state"},
    "geometry_options": {"type": "object", "required": ["disk_size_bytes", "sector_size_bytes", "block_size_bytes", "soft_limit_bytes", "hard_limit_bytes", "eviction_low_watermark_bytes", "max_snapshot_batch_bytes", "max_snapshot_batch_records", "lock_stripe_count", "test_policy_selected", "backing_kind"], "properties": {"disk_size_bytes": {"$ref": "#/$defs/uint64"}, "sector_size_bytes": {"$ref": "#/$defs/uint64"}, "block_size_bytes": {"$ref": "#/$defs/uint64"}, "soft_limit_bytes": {"$ref": "#/$defs/uint64"}, "hard_limit_bytes": {"$ref": "#/$defs/uint64"}, "eviction_low_watermark_bytes": {"$ref": "#/$defs/uint64"}, "max_snapshot_batch_bytes": {"$ref": "#/$defs/uint64"}, "max_snapshot_batch_records": {"$ref": "#/$defs/uint64"}, "lock_stripe_count": {"$ref": "#/$defs/uint64"}, "test_policy_selected": {"type": "boolean"}, "backing_kind": {"enum": ["zero", "fake"]}}},
    "state": {"type": "object", "required": ["initial_state", "initial_backing_generation", "current_backing_generation", "durable_successor_installed", "durable_successor_generation_present", "durable_successor_generation", "durability_scope_statement"], "properties": {"initial_state": {"enum": ["EMPTY", "SEEDED"]}, "initial_backing_generation": {"$ref": "#/$defs/uint64"}, "current_backing_generation": {"$ref": "#/$defs/uint64"}, "durable_successor_installed": {"const": false}, "durable_successor_generation_present": {"const": false}, "durable_successor_generation": {"const": 0}, "durability_scope_statement": {"const": "M2 has no durable backing successor; all backing generations end with this run."}}},
    "phases": {"type": "array", "items": {"type": "object", "required": ["phase_id", "phase_started_utc", "phase_ended_utc", "phase_status", "phase_error", "phase_elapsed_ms"], "properties": {"phase_id": {"enum": ["CAPTURE-BASELINE", "CONSTRUCT-PROVIDER", "START-PROVIDER", "CREATE-DEVICE", "WAIT-FOR-DISK", "SETUP-WRITE", "SAMPLE-AFTER-WRITE", "DELETE-RETRIM", "SAMPLE-AFTER-RETRIM", "PERFORMANCE", "REMOVE-MOUNTS", "REMOVE-DEVICE", "STOP-PROVIDER", "RESTORE-SERVICE", "COMPARE-BASELINE", "PUBLISH-REPORT"]}, "phase_started_utc": {"type": "string"}, "phase_ended_utc": {"type": "string"}, "phase_status": {"enum": ["NOT-RUN", "PASS", "FAIL"]}, "phase_error": {"type": "string"}, "phase_elapsed_ms": {"$ref": "#/$defs/uint64"}}}},
    "io": {"type": "object", "required": ["write_sha256", "read_sha256", "bytes_written", "bytes_read", "bytes_deleted"], "properties": {"write_sha256": {"type": "string"}, "read_sha256": {"type": "string"}, "bytes_written": {"$ref": "#/$defs/uint64"}, "bytes_read": {"$ref": "#/$defs/uint64"}, "bytes_deleted": {"$ref": "#/$defs/uint64"}}},
    "store_stats_samples": {"type": "array", "items": {"type": "object", "required": ["sample_point", "captured_utc", "stats"], "properties": {"sample_point": {"enum": ["READY", "AFTER-WRITE", "AFTER-RETRIM", "FINAL"]}, "captured_utc": {"type": "string"}, "stats": {"$ref": "#/$defs/store_stats"}}}},
    "os_memory_samples": {"type": "array", "items": {"type": "object", "required": ["memory_sample_point", "memory_phase_id", "captured_utc", "provider_process_id", "elapsed_ms", "working_set_bytes", "private_bytes", "commit_bytes"], "properties": {"memory_sample_point": {"enum": ["READY", "AFTER-WRITE", "AFTER-RETRIM", "FINAL"]}, "memory_phase_id": {"enum": ["START-PROVIDER", "SAMPLE-AFTER-WRITE", "SAMPLE-AFTER-RETRIM", "STOP-PROVIDER"]}, "captured_utc": {"type": "string"}, "provider_process_id": {"$ref": "#/$defs/uint64"}, "elapsed_ms": {"$ref": "#/$defs/uint64"}, "working_set_bytes": {"$ref": "#/$defs/uint64"}, "private_bytes": {"$ref": "#/$defs/uint64"}, "commit_bytes": {"$ref": "#/$defs/uint64"}}}},
    "performance": {"type": "object", "required": ["fixed", "dynamic", "comparison", "gate_status"], "properties": {"fixed": {"$ref": "#/$defs/performance_result"}, "dynamic": {"$ref": "#/$defs/performance_result"}, "comparison": {"$ref": "#/$defs/performance_comparison"}, "gate_status": {"enum": ["NOT-EVALUATED", "PASS", "FAIL"]}}},
    "device_identity": {"type": "object", "required": ["provider_session_id", "native_object_path", "device_id_raw", "path_id", "target_id", "lun", "create_flags", "query_flags", "confirmed_by_query_device", "physical_drive_number", "physical_drive_path", "disk_length_bytes", "bus_type", "vendor_id", "product_id", "serial_number", "disk_unique_id", "volume_guids", "mount_points"], "properties": {"provider_session_id": {"type": "string"}, "native_object_path": {"type": "string"}, "device_id_raw": {"type": "string"}, "path_id": {"$ref": "#/$defs/uint64"}, "target_id": {"$ref": "#/$defs/uint64"}, "lun": {"$ref": "#/$defs/uint64"}, "create_flags": {"$ref": "#/$defs/uint64"}, "query_flags": {"$ref": "#/$defs/uint64"}, "confirmed_by_query_device": {"type": "boolean"}, "physical_drive_number": {"$ref": "#/$defs/uint64"}, "physical_drive_path": {"type": "string"}, "disk_length_bytes": {"$ref": "#/$defs/uint64"}, "bus_type": {"type": "string"}, "vendor_id": {"type": "string"}, "product_id": {"type": "string"}, "serial_number": {"type": "string"}, "disk_unique_id": {"type": "string"}, "volume_guids": {"$ref": "#/$defs/string_array"}, "mount_points": {"$ref": "#/$defs/string_array"}}},
    "cleanup_actions": {"type": "array", "items": {"type": "object", "required": ["cleanup_action_id", "cleanup_attempted", "cleanup_result", "cleanup_error"], "properties": {"cleanup_action_id": {"enum": ["REMOVE-MOUNTS", "REMOVE-DEVICE", "STOP-PROVIDER", "RESTORE-SERVICE"]}, "cleanup_attempted": {"type": "boolean"}, "cleanup_result": {"enum": ["NOT-ATTEMPTED", "SUCCEEDED", "FAILED"]}, "cleanup_error": {"type": "string"}}}},
    "final_baseline_comparison": {"type": "object", "required": ["comparison", "exact_match", "resources_within_tolerance", "private_bytes_tolerance", "before_fingerprint", "after_fingerprint", "before", "after", "diff"], "properties": {"comparison": {"enum": ["NOT-COMPARED", "MATCH", "MISMATCH"]}, "exact_match": {"type": "boolean"}, "resources_within_tolerance": {"type": "boolean"}, "private_bytes_tolerance": {"$ref": "#/$defs/uint64"}, "before_fingerprint": {"type": "string"}, "after_fingerprint": {"type": "string"}, "before": {"$ref": "#/$defs/inventory_snapshot"}, "after": {"$ref": "#/$defs/inventory_snapshot"}, "diff": {"$ref": "#/$defs/inventory_diff"}}},
    "failures": {"type": "array", "items": {"type": "object", "required": ["failure_phase", "failure_code", "failure_message"], "properties": {"failure_phase": {"type": "string"}, "failure_code": {"$ref": "#/$defs/uint64"}, "failure_message": {"type": "string"}}}},
    "verdict": {"enum": ["INCOMPLETE", "GO", "NO-GO"]}
  },
  "$defs": {
    "uint64": {"type": "integer", "minimum": 0, "maximum": 18446744073709551615},
    "string_array": {"type": "array", "items": {"type": "string"}},
    "inventory_snapshot": {"type": "object", "required": ["physical_disks", "volumes", "mount_points", "m2_device_ids", "service_state", "service_start_type", "process_handle_count", "process_thread_count", "process_private_bytes"], "properties": {"physical_disks": {"$ref": "#/$defs/string_array"}, "volumes": {"$ref": "#/$defs/string_array"}, "mount_points": {"$ref": "#/$defs/string_array"}, "m2_device_ids": {"$ref": "#/$defs/string_array"}, "service_state": {"type": "string"}, "service_start_type": {"type": "string"}, "process_handle_count": {"$ref": "#/$defs/uint64"}, "process_thread_count": {"$ref": "#/$defs/uint64"}, "process_private_bytes": {"$ref": "#/$defs/uint64"}}},
    "inventory_diff": {"type": "object", "required": ["added", "removed", "changed"], "properties": {"added": {"$ref": "#/$defs/string_array"}, "removed": {"$ref": "#/$defs/string_array"}, "changed": {"$ref": "#/$defs/string_array"}}},
    "store_stats": {"type": "object", "required": ["logical_size_bytes", "reserved_bytes", "committed_bytes", "resident_clean_blocks", "resident_dirty_blocks", "zero_clean_blocks", "zero_dirty_blocks", "loading_blocks", "evicted_blocks_total", "backing_reads_total", "backing_read_bytes_total", "uncached_reads_total", "unmap_bytes_total", "zero_bytes_total", "hard_limit_rejections", "commit_failures", "decommit_failures", "synchronous_pressure_passes_total", "synchronous_pressure_waits_total", "synchronous_pressure_no_progress_total", "synchronous_pressure_cancellations_total", "synchronous_pressure_deadlines_total", "commit_reservation_rollbacks_total", "snapshot_records_total", "snapshot_version_conflicts", "backing_generation", "backing_view_installs_total", "backing_generation_conflicts", "stale_backing_load_retries"], "properties": {"logical_size_bytes": {"$ref": "#/$defs/uint64"}, "reserved_bytes": {"$ref": "#/$defs/uint64"}, "committed_bytes": {"$ref": "#/$defs/uint64"}, "resident_clean_blocks": {"$ref": "#/$defs/uint64"}, "resident_dirty_blocks": {"$ref": "#/$defs/uint64"}, "zero_clean_blocks": {"$ref": "#/$defs/uint64"}, "zero_dirty_blocks": {"$ref": "#/$defs/uint64"}, "loading_blocks": {"$ref": "#/$defs/uint64"}, "evicted_blocks_total": {"$ref": "#/$defs/uint64"}, "backing_reads_total": {"$ref": "#/$defs/uint64"}, "backing_read_bytes_total": {"$ref": "#/$defs/uint64"}, "uncached_reads_total": {"$ref": "#/$defs/uint64"}, "unmap_bytes_total": {"$ref": "#/$defs/uint64"}, "zero_bytes_total": {"$ref": "#/$defs/uint64"}, "hard_limit_rejections": {"$ref": "#/$defs/uint64"}, "commit_failures": {"$ref": "#/$defs/uint64"}, "decommit_failures": {"$ref": "#/$defs/uint64"}, "synchronous_pressure_passes_total": {"$ref": "#/$defs/uint64"}, "synchronous_pressure_waits_total": {"$ref": "#/$defs/uint64"}, "synchronous_pressure_no_progress_total": {"$ref": "#/$defs/uint64"}, "synchronous_pressure_cancellations_total": {"$ref": "#/$defs/uint64"}, "synchronous_pressure_deadlines_total": {"$ref": "#/$defs/uint64"}, "commit_reservation_rollbacks_total": {"$ref": "#/$defs/uint64"}, "snapshot_records_total": {"$ref": "#/$defs/uint64"}, "snapshot_version_conflicts": {"$ref": "#/$defs/uint64"}, "backing_generation": {"$ref": "#/$defs/uint64"}, "backing_view_installs_total": {"$ref": "#/$defs/uint64"}, "backing_generation_conflicts": {"$ref": "#/$defs/uint64"}, "stale_backing_load_retries": {"$ref": "#/$defs/uint64"}}},
    "driver_service_state": {"type": "object", "required": ["driver_state", "service_name", "service_state", "service_start_type", "driver_file_path", "driver_file_version", "driver_sha256"], "properties": {"driver_state": {"type": "string"}, "service_name": {"type": "string"}, "service_state": {"type": "string"}, "service_start_type": {"type": "string"}, "driver_file_path": {"type": "string"}, "driver_file_version": {"type": "string"}, "driver_sha256": {"type": "string"}}},
    "performance_policy": {"type": "object", "required": ["machine_fingerprint", "build_fingerprint", "process_policy", "raw_io_flags"], "properties": {"machine_fingerprint": {"type": "string"}, "build_fingerprint": {"type": "string"}, "process_policy": {"type": "string"}, "raw_io_flags": {"type": "string"}}},
    "sequential_sample": {"type": "object", "required": ["bytes", "operations", "elapsed_ticks", "clock_frequency_hz", "bytes_per_second"], "properties": {"bytes": {"$ref": "#/$defs/uint64"}, "operations": {"$ref": "#/$defs/uint64"}, "elapsed_ticks": {"$ref": "#/$defs/uint64"}, "clock_frequency_hz": {"$ref": "#/$defs/uint64"}, "bytes_per_second": {"$ref": "#/$defs/uint64"}}},
    "random_sample": {"type": "object", "required": ["bytes", "operations", "elapsed_ticks", "clock_frequency_hz", "operations_per_second"], "properties": {"bytes": {"$ref": "#/$defs/uint64"}, "operations": {"$ref": "#/$defs/uint64"}, "elapsed_ticks": {"$ref": "#/$defs/uint64"}, "clock_frequency_hz": {"$ref": "#/$defs/uint64"}, "operations_per_second": {"$ref": "#/$defs/uint64"}}},
    "performance_result": {"type": "object", "required": ["methodology_id", "operation", "queue_depth", "region_offset", "region_bytes", "sequential_io_bytes", "random_io_bytes", "random_seed", "sequential_warmup_operations", "random_warmup_operations", "policy", "sequential", "random"], "properties": {"methodology_id": {"const": "m2-raw-qd1-v1"}, "operation": {"enum": ["read", "write"]}, "queue_depth": {"const": 1}, "region_offset": {"$ref": "#/$defs/uint64"}, "region_bytes": {"const": 33554432}, "sequential_io_bytes": {"const": 1048576}, "random_io_bytes": {"const": 4096}, "random_seed": {"$ref": "#/$defs/uint64"}, "sequential_warmup_operations": {"const": 1}, "random_warmup_operations": {"const": 128}, "policy": {"$ref": "#/$defs/performance_policy"}, "sequential": {"$ref": "#/$defs/sequential_sample"}, "random": {"$ref": "#/$defs/random_sample"}}},
    "performance_comparison": {"type": "object", "required": ["sequential_ratio_basis_points", "random_ratio_basis_points", "sequential_threshold_basis_points", "random_threshold_basis_points", "sequential_pass", "random_pass", "overall_pass"], "properties": {"sequential_ratio_basis_points": {"$ref": "#/$defs/uint64"}, "random_ratio_basis_points": {"$ref": "#/$defs/uint64"}, "sequential_threshold_basis_points": {"const": 7000}, "random_threshold_basis_points": {"const": 6000}, "sequential_pass": {"type": "boolean"}, "random_pass": {"type": "boolean"}, "overall_pass": {"type": "boolean"}}}
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
