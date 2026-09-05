#include "test_framework.h"

#include "src/control/command.h"
#include "src/cli/cli_parser.h"
#include "src/product/json_writer.h"
#include "src/product/product_error.h"
#include "src/cli/cli_renderer.h"

#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace {

struct CommandForm {
    std::string_view path;
    std::string_view positionals;
    std::string_view required_flags;
    std::string_view optional_flags;
    bool mutating;
    bool long_operation;
};

constexpr std::array<CommandForm, 21> kCommandForms{{
    {"status", "", "", "", false, false},
    {"disk.list", "", "", "", false, false},
    {"disk.show", "name-or-uuid", "", "", false, false},
    {"disk.create", "", "name,bundle,size,mount", "owner-sid,no-start,memory-mode,memory-hard-limit,checkpoint-interval,dirty-threshold", true, true},
    {"disk.import", "", "name,bundle,mount", "owner-sid", true, true},
    {"disk.start", "name-or-uuid", "", "", true, true},
    {"disk.stop", "name-or-uuid", "", "", true, true},
    {"disk.stop", "name-or-uuid", "force,accept-volatile-data-loss", "", true, true},
    {"disk.checkpoint", "name-or-uuid", "", "", true, true},
    {"disk.compact", "name-or-uuid", "", "", true, true},
    {"disk.config.get", "name-or-uuid", "", "", false, false},
    {"disk.config.set", "name-or-uuid,key=value...", "", "", true, false},
    {"disk.repair-profile", "uuid", "from-previous", "", true, true},
    {"disk.rebind", "uuid", "bundle,expect-image-uuid", "", true, true},
    {"disk.forget", "uuid", "confirm-profile", "", true, true},
    {"operation.list", "", "", "", false, false},
    {"operation.show", "operation-id", "", "", false, false},
    {"operation.wait", "operation-id", "", "", false, false},
    {"operation.cancel", "operation-id", "", "", true, false},
    {"diagnostics.collect", "", "", "disk,output,include-paths", false, false},
    {"driver.status", "", "", "", false, false},
}};

constexpr std::array<std::string_view, 5> kGlobalFlags{{
    "json", "timeout", "request-id", "no-wait", "dry-run",
}};

constexpr std::array<int, 15> kStableExitCodes{{
    0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
}};

constexpr std::array<std::string_view, 3> kIdempotentWarnings{{
    "AlreadyRunning", "AlreadyStopped", "NoChanges",
}};

} // namespace

TEST_CASE(cli_contract_table_covers_all_frozen_command_forms) {
    REQUIRE_EQ(kCommandForms.size(), 21U);
    std::size_t long_operations = 0;
    std::size_t force_stop_forms = 0;
    for (const auto& form : kCommandForms) {
        REQUIRE(!form.path.empty());
        if (form.long_operation) {
            REQUIRE(form.mutating);
            ++long_operations;
        }
        if (form.path == "disk.stop" && !form.required_flags.empty()) {
            ++force_stop_forms;
        }
    }
    REQUIRE_EQ(long_operations, 10U); // normal and force stop are two grammar forms
    REQUIRE_EQ(force_stop_forms, 1U);
}

TEST_CASE(cli_contract_table_freezes_globals_exits_and_idempotent_warnings) {
    REQUIRE_EQ(kGlobalFlags.size(), 5U);
    REQUIRE_EQ(kStableExitCodes.front(), 0);
    REQUIRE_EQ(kStableExitCodes[1], 2);
    REQUIRE_EQ(kStableExitCodes.back(), 15);
    for (const int code : kStableExitCodes) {
        REQUIRE(code != 1);
    }
    REQUIRE_EQ(kIdempotentWarnings.size(), 3U);
    REQUIRE_EQ(kIdempotentWarnings[0], std::string_view("AlreadyRunning"));
    REQUIRE_EQ(kIdempotentWarnings[1], std::string_view("AlreadyStopped"));
    REQUIRE_EQ(kIdempotentWarnings[2], std::string_view("NoChanges"));
}

TEST_CASE(cli_parser_parses_read_only_disk_show_to_typed_variant) {
    constexpr std::array<std::wstring_view, 3> args{{L"disk", L"show", L"WorkDisk"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(std::holds_alternative<myrd::control::DiskShowCommand>(result.value().command));
    const auto& command = std::get<myrd::control::DiskShowCommand>(result.value().command);
    REQUIRE_EQ(command.target, std::wstring(L"WorkDisk"));
    REQUIRE_EQ(myrd::control::command_name(result.value().command), std::string_view("disk.show"));
    REQUIRE(!myrd::control::is_mutating(result.value().command));
    REQUIRE(!myrd::control::is_long_operation(result.value().command));
}

TEST_CASE(cli_parser_parses_mutating_disk_start_to_typed_variant) {
    constexpr std::array<std::wstring_view, 3> args{{L"disk", L"start", L"WorkDisk"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(std::holds_alternative<myrd::control::DiskStartCommand>(result.value().command));
    const auto& command = std::get<myrd::control::DiskStartCommand>(result.value().command);
    REQUIRE_EQ(command.target, std::wstring(L"WorkDisk"));
    REQUIRE_EQ(myrd::control::command_name(result.value().command), std::string_view("disk.start"));
    REQUIRE(myrd::control::is_mutating(result.value().command));
    REQUIRE(myrd::control::is_long_operation(result.value().command));
}

TEST_CASE(cli_parser_rejects_missing_or_extra_target_for_initial_variants) {
    constexpr std::array<std::wstring_view, 2> missing_show{{L"disk", L"show"}};
    constexpr std::array<std::wstring_view, 4> extra_start{{L"disk", L"start", L"one", L"two"}};
    constexpr std::array<std::wstring_view, 3> empty_show{{L"disk", L"show", L""}};
    REQUIRE(!myrd::cli::parse_cli(missing_show).ok());
    REQUIRE(!myrd::cli::parse_cli(extra_start).ok());
    REQUIRE(!myrd::cli::parse_cli(empty_show).ok());
}

TEST_CASE(cli_parser_group_one_parses_five_typed_commands) {
    constexpr std::array<std::wstring_view, 1> status_args{{L"status"}};
    constexpr std::array<std::wstring_view, 2> list_args{{L"disk", L"list"}};
    constexpr std::array<std::wstring_view, 3> checkpoint_args{{L"disk", L"checkpoint", L"WorkDisk"}};
    constexpr std::array<std::wstring_view, 3> compact_args{{L"disk", L"compact", L"WorkDisk"}};
    constexpr std::array<std::wstring_view, 4> config_get_args{{L"disk", L"config", L"get", L"WorkDisk"}};

    auto status = myrd::cli::parse_cli(status_args);
    auto list = myrd::cli::parse_cli(list_args);
    auto checkpoint = myrd::cli::parse_cli(checkpoint_args);
    auto compact = myrd::cli::parse_cli(compact_args);
    auto config_get = myrd::cli::parse_cli(config_get_args);
    REQUIRE(status.ok());
    REQUIRE(list.ok());
    REQUIRE(checkpoint.ok());
    REQUIRE(compact.ok());
    REQUIRE(config_get.ok());
    REQUIRE(std::holds_alternative<myrd::control::StatusCommand>(status.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskListCommand>(list.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskCheckpointCommand>(checkpoint.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskCompactCommand>(compact.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskConfigGetCommand>(config_get.value().command));
    REQUIRE_EQ(myrd::control::command_name(status.value().command), std::string_view("status"));
    REQUIRE_EQ(myrd::control::command_name(list.value().command), std::string_view("disk.list"));
    REQUIRE_EQ(myrd::control::command_name(checkpoint.value().command), std::string_view("disk.checkpoint"));
    REQUIRE_EQ(myrd::control::command_name(compact.value().command), std::string_view("disk.compact"));
    REQUIRE_EQ(myrd::control::command_name(config_get.value().command), std::string_view("disk.config.get"));
    REQUIRE(!myrd::control::is_mutating(status.value().command));
    REQUIRE(!myrd::control::is_mutating(list.value().command));
    REQUIRE(myrd::control::is_long_operation(checkpoint.value().command));
    REQUIRE(myrd::control::is_long_operation(compact.value().command));
    REQUIRE(!myrd::control::is_mutating(config_get.value().command));
}

TEST_CASE(cli_parser_group_two_parses_operations_and_driver_status) {
    constexpr std::wstring_view id = L"01234567-89ab-cdef-0123-456789abcdef";
    constexpr std::array<std::wstring_view, 2> list_args{{L"operation", L"list"}};
    constexpr std::array<std::wstring_view, 3> show_args{{L"operation", L"show", id}};
    constexpr std::array<std::wstring_view, 3> wait_args{{L"operation", L"wait", id}};
    constexpr std::array<std::wstring_view, 3> cancel_args{{L"operation", L"cancel", id}};
    constexpr std::array<std::wstring_view, 2> driver_args{{L"driver", L"status"}};
    auto list = myrd::cli::parse_cli(list_args);
    auto show = myrd::cli::parse_cli(show_args);
    auto wait = myrd::cli::parse_cli(wait_args);
    auto cancel = myrd::cli::parse_cli(cancel_args);
    auto driver = myrd::cli::parse_cli(driver_args);
    REQUIRE(list.ok()); REQUIRE(show.ok()); REQUIRE(wait.ok()); REQUIRE(cancel.ok()); REQUIRE(driver.ok());
    REQUIRE(std::holds_alternative<myrd::control::OperationListCommand>(list.value().command));
    REQUIRE(std::holds_alternative<myrd::control::OperationShowCommand>(show.value().command));
    REQUIRE(std::holds_alternative<myrd::control::OperationWaitCommand>(wait.value().command));
    REQUIRE(std::holds_alternative<myrd::control::OperationCancelCommand>(cancel.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DriverStatusCommand>(driver.value().command));
    REQUIRE_EQ(std::get<myrd::control::OperationShowCommand>(show.value().command).operation_id.to_string(), std::string("01234567-89ab-cdef-0123-456789abcdef"));
    REQUIRE(!myrd::control::is_mutating(wait.value().command));
    REQUIRE(myrd::control::is_mutating(cancel.value().command));
    REQUIRE(!myrd::control::is_long_operation(cancel.value().command));
}

TEST_CASE(cli_parser_group_two_rejects_malformed_operation_id) {
    constexpr std::array<std::wstring_view, 3> args{{L"operation", L"show", L"not-a-uuid"}};
    REQUIRE(!myrd::cli::parse_cli(args).ok());
}

TEST_CASE(cli_parser_group_three_parses_config_assignments) {
    constexpr std::array<std::wstring_view, 6> args{{
        L"disk", L"config", L"set", L"WorkDisk",
        L"checkpoint.interval.duration=5m", L"checkpoint.on_memory_pressure=false"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(std::holds_alternative<myrd::control::DiskConfigSetCommand>(result.value().command));
    const auto& command = std::get<myrd::control::DiskConfigSetCommand>(result.value().command);
    REQUIRE_EQ(command.target, std::wstring(L"WorkDisk"));
    REQUIRE_EQ(command.assignments.size(), 2U);
    REQUIRE_EQ(command.assignments[0].key, std::wstring(L"checkpoint.interval.duration"));
    REQUIRE_EQ(command.assignments[0].value, std::wstring(L"5m"));
    REQUIRE(myrd::control::is_mutating(result.value().command));
    REQUIRE(!myrd::control::is_long_operation(result.value().command));
}

TEST_CASE(cli_parser_group_three_rejects_missing_or_malformed_config_assignments) {
    constexpr std::array<std::wstring_view, 4> missing{{L"disk", L"config", L"set", L"WorkDisk"}};
    constexpr std::array<std::wstring_view, 5> no_equals{{L"disk", L"config", L"set", L"WorkDisk", L"broken"}};
    constexpr std::array<std::wstring_view, 5> empty_key{{L"disk", L"config", L"set", L"WorkDisk", L"=5m"}};
    constexpr std::array<std::wstring_view, 5> empty_value{{L"disk", L"config", L"set", L"WorkDisk", L"key="}};
    REQUIRE(!myrd::cli::parse_cli(missing).ok());
    REQUIRE(!myrd::cli::parse_cli(no_equals).ok());
    REQUIRE(!myrd::cli::parse_cli(empty_key).ok());
    REQUIRE(!myrd::cli::parse_cli(empty_value).ok());
}

TEST_CASE(cli_parser_group_three_parses_diagnostics_options) {
    constexpr std::array<std::wstring_view, 8> args{{
        L"diagnostics", L"collect", L"--disk", L"WorkDisk",
        L"--output", L"C:\\Temp\\myrd.zip", L"--include-paths", L"--json"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(result.value().options.json);
    REQUIRE(std::holds_alternative<myrd::control::DiagnosticsCollectCommand>(result.value().command));
    const auto& command = std::get<myrd::control::DiagnosticsCollectCommand>(result.value().command);
    REQUIRE(command.disk.has_value());
    REQUIRE(command.output.has_value());
    REQUIRE_EQ(*command.disk, std::wstring(L"WorkDisk"));
    REQUIRE_EQ(*command.output, std::wstring(L"C:\\Temp\\myrd.zip"));
    REQUIRE(command.include_paths);
    REQUIRE(!myrd::control::is_mutating(result.value().command));
}

TEST_CASE(cli_parser_group_four_parses_five_lifecycle_commands) {
    constexpr std::wstring_view id = L"01234567-89ab-cdef-0123-456789abcdef";
    constexpr std::wstring_view image = L"11111111-2222-4333-8444-555555555555";
    constexpr std::array<std::wstring_view, 8> import_args{{L"disk", L"import", L"--name", L"Imported", L"--bundle", L"D:\\Images\\Disk", L"--mount", L"R:"}};
    constexpr std::array<std::wstring_view, 3> stop_args{{L"disk", L"stop", L"WorkDisk"}};
    constexpr std::array<std::wstring_view, 4> repair_args{{L"disk", L"repair-profile", id, L"--from-previous"}};
    constexpr std::array<std::wstring_view, 7> rebind_args{{L"disk", L"rebind", id, L"--bundle", L"D:\\New\\Disk", L"--expect-image-uuid", image}};
    constexpr std::array<std::wstring_view, 5> forget_args{{L"disk", L"forget", id, L"--confirm-profile", id}};
    auto import_result = myrd::cli::parse_cli(import_args);
    auto stop = myrd::cli::parse_cli(stop_args);
    auto repair = myrd::cli::parse_cli(repair_args);
    auto rebind = myrd::cli::parse_cli(rebind_args);
    auto forget = myrd::cli::parse_cli(forget_args);
    REQUIRE(import_result.ok()); REQUIRE(stop.ok()); REQUIRE(repair.ok()); REQUIRE(rebind.ok()); REQUIRE(forget.ok());
    REQUIRE(std::holds_alternative<myrd::control::DiskImportCommand>(import_result.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskStopCommand>(stop.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskRepairProfileCommand>(repair.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskRebindCommand>(rebind.value().command));
    REQUIRE(std::holds_alternative<myrd::control::DiskForgetCommand>(forget.value().command));
    REQUIRE_EQ(std::get<myrd::control::DiskImportCommand>(import_result.value().command).bundle, std::wstring(L"D:\\Images\\Disk"));
    REQUIRE(!std::get<myrd::control::DiskStopCommand>(stop.value().command).force);
    REQUIRE(myrd::control::is_long_operation(forget.value().command));
}

TEST_CASE(cli_parser_group_four_rejects_mismatched_forget_confirmation) {
    constexpr std::array<std::wstring_view, 5> args{{L"disk", L"forget",
        L"01234567-89ab-cdef-0123-456789abcdef", L"--confirm-profile",
        L"11111111-2222-4333-8444-555555555555"}};
    REQUIRE(!myrd::cli::parse_cli(args).ok());
}

TEST_CASE(cli_parser_group_five_parses_full_disk_create) {
    constexpr std::array<std::wstring_view, 19> args{{
        L"disk", L"create", L"--name", L"FastDisk", L"--bundle", L"D:\\Images\\FastDisk",
        L"--size", L"8GiB", L"--mount", L"R:", L"--no-start",
        L"--memory-mode", L"fixed", L"--memory-hard-limit", L"2GiB",
        L"--checkpoint-interval", L"5m", L"--dirty-threshold", L"off"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(std::holds_alternative<myrd::control::DiskCreateCommand>(result.value().command));
    const auto& command = std::get<myrd::control::DiskCreateCommand>(result.value().command);
    REQUIRE_EQ(command.name, std::wstring(L"FastDisk"));
    REQUIRE_EQ(command.bundle, std::wstring(L"D:\\Images\\FastDisk"));
    REQUIRE_EQ(command.size_bytes, 8ULL << 30);
    REQUIRE(command.no_start);
    REQUIRE(command.memory_mode.has_value());
    REQUIRE_EQ(*command.memory_mode, myrd::control::MemoryMode::fixed);
    REQUIRE_EQ(*command.memory_hard_limit_bytes, 2ULL << 30);
    REQUIRE(command.checkpoint_interval.has_value());
    REQUIRE(command.checkpoint_interval->enabled);
    REQUIRE(command.dirty_threshold.has_value());
    REQUIRE(!command.dirty_threshold->enabled);
    REQUIRE(myrd::control::is_long_operation(result.value().command));
}

TEST_CASE(cli_parser_group_five_rejects_missing_create_required_flags_and_bad_size) {
    constexpr std::array<std::wstring_view, 8> missing_mount{{
        L"disk", L"create", L"--name", L"FastDisk", L"--bundle", L"D:\\Images\\FastDisk", L"--size", L"8GiB"}};
    constexpr std::array<std::wstring_view, 10> bad_size{{
        L"disk", L"create", L"--name", L"FastDisk", L"--bundle", L"D:\\Images\\FastDisk", L"--size", L"8GB", L"--mount", L"R:"}};
    REQUIRE(!myrd::cli::parse_cli(missing_mount).ok());
    REQUIRE(!myrd::cli::parse_cli(bad_size).ok());
}

TEST_CASE(cli_parser_global_options_work_before_and_after_command) {
    constexpr std::array<std::wstring_view, 10> args{{
        L"--json", L"--request-id", L"01234567-89ab-cdef-0123-456789abcdef",
        L"disk", L"start", L"WorkDisk", L"--timeout", L"10s", L"--dry-run", L""}};
    auto result = myrd::cli::parse_cli(std::span(args).first(9));
    REQUIRE(result.ok());
    REQUIRE(result.value().options.json);
    REQUIRE(result.value().options.request_id.has_value());
    REQUIRE_EQ(*result.value().options.timeout, std::chrono::seconds(10));
    REQUIRE(result.value().options.dry_run);
}

TEST_CASE(cli_parser_force_stop_requires_both_confirmations) {
    constexpr std::array<std::wstring_view, 5> valid{{L"disk", L"stop", L"WorkDisk", L"--force", L"--accept-volatile-data-loss"}};
    constexpr std::array<std::wstring_view, 4> force_only{{L"disk", L"stop", L"WorkDisk", L"--force"}};
    constexpr std::array<std::wstring_view, 4> accept_only{{L"disk", L"stop", L"WorkDisk", L"--accept-volatile-data-loss"}};
    auto result = myrd::cli::parse_cli(valid);
    REQUIRE(result.ok());
    const auto& command =
        std::get<myrd::control::DiskStopCommand>(result.value().command);
    REQUIRE(command.force);
    REQUIRE(command.accept_volatile_data_loss);
    REQUIRE(!myrd::cli::parse_cli(force_only).ok());
    REQUIRE(!myrd::cli::parse_cli(accept_only).ok());
}

TEST_CASE(cli_parser_rejects_no_wait_for_nonlong_and_read_only_commands) {
    constexpr std::array<std::wstring_view, 4> start{{L"disk", L"start", L"WorkDisk", L"--no-wait"}};
    constexpr std::array<std::wstring_view, 3> status{{L"status", L"--no-wait", L""}};
    constexpr std::array<std::wstring_view, 6> config{{L"disk", L"config", L"set", L"WorkDisk", L"x=y", L"--no-wait"}};
    auto start_result = myrd::cli::parse_cli(start);
    REQUIRE(start_result.ok());
    REQUIRE(start_result.value().options.no_wait);
    REQUIRE(!myrd::cli::parse_cli(std::span(status).first(2)).ok());
    REQUIRE(!myrd::cli::parse_cli(config).ok());
}

TEST_CASE(cli_parser_rejects_dry_run_for_read_only_and_conflicting_wait_options) {
    constexpr std::array<std::wstring_view, 2> read_only{{L"status", L"--dry-run"}};
    constexpr std::array<std::wstring_view, 5> dry_no_wait{{L"disk", L"start", L"WorkDisk", L"--dry-run", L"--no-wait"}};
    constexpr std::array<std::wstring_view, 6> timeout_no_wait{{L"disk", L"start", L"WorkDisk", L"--timeout", L"10s", L"--no-wait"}};
    REQUIRE(!myrd::cli::parse_cli(read_only).ok());
    REQUIRE(!myrd::cli::parse_cli(dry_no_wait).ok());
    REQUIRE(!myrd::cli::parse_cli(timeout_no_wait).ok());
}

TEST_CASE(cli_parser_rejects_invalid_memory_mode_coupling) {
    constexpr std::array<std::wstring_view, 12> automatic_with_hard{{L"disk", L"create", L"--name", L"x", L"--bundle", L"D:\\x", L"--size", L"1GiB", L"--mount", L"R:", L"--memory-hard-limit", L"512MiB"}};
    constexpr std::array<std::wstring_view, 12> fixed_without_hard{{L"disk", L"create", L"--name", L"x", L"--bundle", L"D:\\x", L"--size", L"1GiB", L"--mount", L"R:", L"--memory-mode", L"fixed"}};
    REQUIRE(!myrd::cli::parse_cli(automatic_with_hard).ok());
    REQUIRE(!myrd::cli::parse_cli(fixed_without_hard).ok());
}

TEST_CASE(cli_parser_rejects_malformed_or_duplicate_global_options) {
    constexpr std::array<std::wstring_view, 5> bad_timeout{{L"disk", L"show", L"x", L"--timeout", L"1.5s"}};
    constexpr std::array<std::wstring_view, 3> duplicate_json{{L"status", L"--json", L"--json"}};
    constexpr std::array<std::wstring_view, 5> duplicate_request{{L"status", L"--request-id", L"01234567-89ab-cdef-0123-456789abcdef", L"--request-id", L"01234567-89ab-cdef-0123-456789abcdef"}};
    REQUIRE(!myrd::cli::parse_cli(bad_timeout).ok());
    REQUIRE(!myrd::cli::parse_cli(duplicate_json).ok());
    REQUIRE(!myrd::cli::parse_cli(duplicate_request).ok());
}

// ===== M4 Task 03.5 — JSON writer and CLI renderer RED contract tests =====

namespace {

using namespace std::chrono;

std::string read_golden(const std::string& name) {
    std::ifstream file(std::string(MYRD_CLI_GOLDEN_DIR) + "/" + name);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open golden fixture: " + name);
    }
    std::ostringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

myrd::product::SuccessEnvelope golden_success_envelope() {
    return myrd::product::SuccessEnvelope{
        "myrd.cli/v1",
        "550e8400-e29b-41d4-a716-446655440000",
        "disk.show",
        {},
        {}};
}

myrd::product::ErrorEnvelope golden_error_envelope() {
    return myrd::product::ErrorEnvelope{
        "myrd.cli/v1",
        "550e8400-e29b-41d4-a716-446655440000",
        myrd::product::ErrorDetail{
            "PersistenceBlocked",
            "PersistenceBlocked",
            "Checkpoint cannot continue because free space is insufficient.",
            false,
            "Free space on the bundle volume and retry checkpoint."}};
}

} // namespace

TEST_CASE(json_writer_success_envelope_matches_golden_byte_for_byte) {
    auto actual = myrd::product::write_success_envelope(golden_success_envelope());
    REQUIRE(actual.ok());
    REQUIRE_EQ(actual.value(), read_golden("success.json"));
}

TEST_CASE(json_writer_failure_envelope_matches_golden_byte_for_byte) {
    auto actual = myrd::product::write_error_envelope(golden_error_envelope());
    REQUIRE(actual.ok());
    REQUIRE_EQ(actual.value(), read_golden("error.json"));
}

TEST_CASE(json_writer_rejects_malformed_utf8_fields) {
    auto envelope = golden_success_envelope();
    envelope.command = std::string("\xC3\x28", 2);
    REQUIRE(!myrd::product::write_success_envelope(envelope).ok());
}

TEST_CASE(escape_json_string_handles_quote_backslash_and_controls) {
    using myrd::product::escape_json_string;
    const auto quote = escape_json_string("\"");
    const auto backslash = escape_json_string("\\");
    const auto controls = escape_json_string("a\tb\nc\rd");
    const auto nul = escape_json_string(std::string(1, '\0'));
    REQUIRE(quote.ok());
    REQUIRE(backslash.ok());
    REQUIRE(controls.ok());
    REQUIRE(nul.ok());
    REQUIRE_EQ(quote.value(), "\\\"");
    REQUIRE_EQ(backslash.value(), "\\\\");
    REQUIRE_EQ(controls.value(), "a\\tb\\nc\\rd");
    REQUIRE_EQ(nul.value(), "\\u0000");
    REQUIRE_EQ(escape_json_string(std::string(1, '\b')).value(), "\\b");
    REQUIRE_EQ(escape_json_string(std::string(1, '\f')).value(), "\\f");
    REQUIRE_EQ(escape_json_string(std::string(1, '\x1F')).value(), "\\u001f");
    REQUIRE_EQ(escape_json_string("hello world/").value(), "hello world/");
    REQUIRE(!escape_json_string(std::string("\xC3\x28", 2)).ok());
}

TEST_CASE(json_from_utf16_strictly_converts_valid_unicode) {
    const std::wstring basic{static_cast<wchar_t>(0x4E2D),
                             static_cast<wchar_t>(0x6587)};
    auto basic_result = myrd::product::json_from_utf16(basic);
    REQUIRE(basic_result.ok());
    REQUIRE_EQ(basic_result.value(), "\xE4\xB8\xAD\xE6\x96\x87");

    const std::wstring supplementary{static_cast<wchar_t>(0xD83D),
                                     static_cast<wchar_t>(0xDE00)};
    auto supplementary_result = myrd::product::json_from_utf16(supplementary);
    REQUIRE(supplementary_result.ok());
    REQUIRE_EQ(supplementary_result.value(), "\xF0\x9F\x98\x80");
}

TEST_CASE(json_from_utf16_rejects_invalid_surrogate_sequences) {
    const std::wstring high_only{static_cast<wchar_t>(0xD800)};
    const std::wstring low_only{static_cast<wchar_t>(0xDC00)};
    const std::wstring high_then_ascii{static_cast<wchar_t>(0xD800), L'a'};
    REQUIRE(!myrd::product::json_from_utf16(high_only).ok());
    REQUIRE(!myrd::product::json_from_utf16(low_only).ok());
    REQUIRE(!myrd::product::json_from_utf16(high_then_ascii).ok());
}

TEST_CASE(rfc3339_utc_format_is_exact_and_timezone_independent) {
    const auto timestamp = sys_days(2026y / January / 15) + 12h + 30min;
    REQUIRE_EQ(myrd::product::format_rfc3339_utc(timestamp),
               "2026-01-15T12:30:00Z");
    const auto end_of_day = sys_days(2026y / January / 1) + 23h + 59min + 59s;
    REQUIRE_EQ(myrd::product::format_rfc3339_utc(end_of_day),
               "2026-01-01T23:59:59Z");
}

TEST_CASE(render_uint64_uses_strings_outside_javascript_safe_integer_range) {
    using myrd::product::render_uint64_as_json;
    REQUIRE_EQ(render_uint64_as_json(0ULL), "0");
    REQUIRE_EQ(render_uint64_as_json(9007199254740991ULL),
               "9007199254740991");
    REQUIRE_EQ(render_uint64_as_json(9007199254740992ULL),
               "\"9007199254740992\"");
    REQUIRE_EQ(render_uint64_as_json(std::numeric_limits<std::uint64_t>::max()),
               "\"18446744073709551615\"");
}

TEST_CASE(schema_validation_rejects_unknown_major_but_ignores_additive_fields) {
    constexpr std::array<std::string_view, 2> additive_fields{{
        "future_field", "another_future_field"}};
    REQUIRE(myrd::product::validate_schema("myrd.cli/v1"));
    REQUIRE(!myrd::product::validate_schema("myrd.cli/v2"));
    REQUIRE(!myrd::product::validate_schema("myrd.cli/v1;foo=bar"));
    REQUIRE(myrd::product::validate_envelope_header("myrd.cli/v1",
                                                     additive_fields));
    REQUIRE(!myrd::product::validate_envelope_header("myrd.cli/v2",
                                                      additive_fields));
}

TEST_CASE(cli_renderer_json_success_emits_exactly_one_document_on_stdout) {
    auto output = myrd::cli::render_json_success(golden_success_envelope());
    REQUIRE(output.ok());
    REQUIRE_EQ(output.value().exit_code, 0);
    REQUIRE_EQ(output.value().stdout_text, read_golden("success.json"));
    REQUIRE(output.value().stderr_text.empty());
}

TEST_CASE(cli_renderer_json_failure_emits_exactly_one_document_on_stdout) {
    auto output = myrd::cli::render_json_failure(8, golden_error_envelope());
    REQUIRE(output.ok());
    REQUIRE_EQ(output.value().exit_code, 8);
    REQUIRE_EQ(output.value().stdout_text, read_golden("error.json"));
    REQUIRE(output.value().stderr_text.empty());
}

TEST_CASE(cli_renderer_human_failure_uses_stderr_only) {
    auto output = myrd::cli::render_human_failure(
        8, "Checkpoint blocked: out of free space");
    REQUIRE_EQ(output.exit_code, 8);
    REQUIRE(output.stdout_text.empty());
    REQUIRE(output.stderr_text.find("8") != std::string::npos);
    REQUIRE(output.stderr_text.find("blocked") != std::string::npos);
}

TEST_CASE(cli_exit_mapping_covers_success_and_every_stable_failure_category) {
    using myrd::product::ErrorCategory;
    constexpr std::array mappings{
        std::pair{ErrorCategory::usage, 2},
        std::pair{ErrorCategory::not_found, 3},
        std::pair{ErrorCategory::conflict, 4},
        std::pair{ErrorCategory::permission, 5},
        std::pair{ErrorCategory::unavailable, 6},
        std::pair{ErrorCategory::transient, 7},
        std::pair{ErrorCategory::persistence_blocked, 8},
        std::pair{ErrorCategory::needs_attention, 9},
        std::pair{ErrorCategory::commit_indeterminate, 10},
        std::pair{ErrorCategory::timeout, 11},
        std::pair{ErrorCategory::cancelled, 12},
        std::pair{ErrorCategory::incompatible, 13},
        std::pair{ErrorCategory::integrity, 14},
        std::pair{ErrorCategory::fatal, 15},
    };
    auto success = myrd::cli::render_json_success(golden_success_envelope());
    REQUIRE(success.ok());
    REQUIRE_EQ(success.value().exit_code, 0);
    for (const auto& [category, expected] : mappings) {
        REQUIRE_EQ(myrd::product::cli_exit_code(category), expected);
    }
}

TEST_CASE(profile_names_are_unique_using_windows_case_insensitive_semantics) {
    constexpr std::array<std::wstring_view, 3> unique{{
        L"WorkDisk", L"CacheDisk", L"Scratch"}};
    constexpr std::array<std::wstring_view, 3> duplicate{{
        L"WorkDisk", L"cache", L"workdisk"}};
    REQUIRE(myrd::control::profile_names_are_unique_case_insensitive(unique));
    REQUIRE(!myrd::control::profile_names_are_unique_case_insensitive(duplicate));
}

TEST_CASE(idempotent_warning_strings_are_exact_and_exhaustive) {
    using myrd::control::IdempotentWarning;
    constexpr std::array warnings{
        IdempotentWarning::already_running,
        IdempotentWarning::already_stopped,
        IdempotentWarning::no_changes,
    };
    REQUIRE_EQ(warnings.size(), 3U);
    REQUIRE_EQ(myrd::control::to_string(warnings[0]),
               std::string_view("AlreadyRunning"));
    REQUIRE_EQ(myrd::control::to_string(warnings[1]),
               std::string_view("AlreadyStopped"));
    REQUIRE_EQ(myrd::control::to_string(warnings[2]),
               std::string_view("NoChanges"));
}

TEST_CASE(cli_parser_accepts_request_id_on_read_only_commands) {
    constexpr std::array<std::wstring_view, 3> args{{
        L"status", L"--request-id",
        L"01234567-89ab-cdef-0123-456789abcdef"}};
    auto result = myrd::cli::parse_cli(args);
    REQUIRE(result.ok());
    REQUIRE(result.value().options.request_id.has_value());
}
