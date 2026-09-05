#include "src/cli/cli_parser.h"
#include "src/product/quantity_parser.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace myrd::cli {

namespace {

Result<ParsedCommand> usage_error(const wchar_t* message) {
    return Result<ParsedCommand>::failure(Error{
        ErrorDomain::usage, 1, L"parse_cli", message});
}

Result<product::Uuid> parse_wide_uuid(std::wstring_view text) {
    std::string canonical;
    canonical.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch > 0x7f) {
            return Result<product::Uuid>::failure(Error{
                ErrorDomain::usage, 1, L"parse_cli", L"UUID must contain ASCII characters only"});
        }
        canonical.push_back(static_cast<char>(ch));
    }
    return product::Uuid::parse(canonical);
}

Result<std::string> narrow_ascii(std::wstring_view text) {
    std::string value;
    value.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch > 0x7f) {
            return Result<std::string>::failure(Error{
                ErrorDomain::usage, 1, L"parse_cli", L"Value must use ASCII syntax"});
        }
        value.push_back(static_cast<char>(ch));
    }
    return Result<std::string>::success(std::move(value));
}

Result<std::chrono::seconds> parse_timeout(std::wstring_view text) {
    if (text.size() < 2) {
        return Result<std::chrono::seconds>::failure(Error{
            ErrorDomain::usage, 1, L"parse_cli", L"Timeout requires an integer and unit"});
    }
    std::uint64_t multiplier = 0;
    switch (text.back()) {
    case L's': multiplier = 1; break;
    case L'm': multiplier = 60; break;
    case L'h': multiplier = 3600; break;
    case L'd': multiplier = 86400; break;
    default:
        return Result<std::chrono::seconds>::failure(Error{
            ErrorDomain::usage, 1, L"parse_cli", L"Timeout unit must be s, m, h, or d"});
    }
    std::uint64_t value = 0;
    const auto max_seconds = static_cast<std::uint64_t>(
        std::numeric_limits<std::chrono::seconds::rep>::max());
    for (const wchar_t ch : text.substr(0, text.size() - 1)) {
        if (ch < L'0' || ch > L'9') {
            return Result<std::chrono::seconds>::failure(Error{
                ErrorDomain::usage, 1, L"parse_cli", L"Timeout must use decimal digits"});
        }
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (value > (max_seconds - digit) / 10) {
            return Result<std::chrono::seconds>::failure(Error{
                ErrorDomain::usage, 1, L"parse_cli", L"Timeout is too large"});
        }
        value = value * 10 + digit;
    }
    if (value == 0 || value > max_seconds / multiplier) {
        return Result<std::chrono::seconds>::failure(Error{
            ErrorDomain::usage, 1, L"parse_cli", L"Timeout must be positive and representable"});
    }
    return Result<std::chrono::seconds>::success(std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(value * multiplier)));
}

} // namespace

static Result<ParsedCommand> parse_command(
    std::span<const std::wstring_view> args,
    const GlobalOptions& options) {
    if (args.size() == 1 && args[0] == L"status") {
        return Result<ParsedCommand>::success(ParsedCommand{
            options, control::StatusCommand{}});
    }
    if (args.size() == 2 && args[0] == L"disk" && args[1] == L"list") {
        return Result<ParsedCommand>::success(ParsedCommand{
            options, control::DiskListCommand{}});
    }
    if (args.size() == 2 && args[0] == L"operation" && args[1] == L"list") {
        return Result<ParsedCommand>::success(ParsedCommand{
            options, control::OperationListCommand{}});
    }
    if (args.size() == 2 && args[0] == L"driver" && args[1] == L"status") {
        return Result<ParsedCommand>::success(ParsedCommand{
            options, control::DriverStatusCommand{}});
    }
    if (args.size() >= 5 && args[0] == L"disk" && args[1] == L"config"
        && args[2] == L"set" && !args[3].empty()) {
        std::vector<control::ConfigAssignment> assignments;
        assignments.reserve(args.size() - 4);
        for (std::size_t i = 4; i < args.size(); ++i) {
            const auto separator = args[i].find(L'=');
            if (separator == std::wstring_view::npos || separator == 0
                || separator + 1 == args[i].size()) {
                return usage_error(L"Configuration values must use non-empty key=value syntax");
            }
            assignments.push_back(control::ConfigAssignment{
                std::wstring(args[i].substr(0, separator)),
                std::wstring(args[i].substr(separator + 1))});
        }
        return Result<ParsedCommand>::success(ParsedCommand{
            options, control::DiskConfigSetCommand{
                std::wstring(args[3]), std::move(assignments)}});
    }
    if (args.size() >= 2 && args[0] == L"diagnostics" && args[1] == L"collect") {
        control::DiagnosticsCollectCommand command;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--include-paths" && !command.include_paths) {
                command.include_paths = true;
            } else if (args[i] == L"--disk" && !command.disk.has_value()
                       && i + 1 < args.size() && !args[i + 1].empty()) {
                command.disk = std::wstring(args[++i]);
            } else if (args[i] == L"--output" && !command.output.has_value()
                       && i + 1 < args.size() && !args[i + 1].empty()) {
                command.output = std::wstring(args[++i]);
            } else {
                return usage_error(L"Invalid or duplicate diagnostics option");
            }
        }
        return Result<ParsedCommand>::success(ParsedCommand{
            options, std::move(command)});
    }
    if (args.size() >= 2 && args[0] == L"disk" && args[1] == L"import") {
        std::optional<std::wstring> name;
        std::optional<std::wstring> bundle_text;
        std::optional<std::wstring> mount;
        std::optional<std::wstring> owner_sid;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                return usage_error(L"Import options require non-empty values");
            }
            if (args[i] == L"--name" && !name) name = std::wstring(args[++i]);
            else if (args[i] == L"--bundle" && !bundle_text) bundle_text = std::wstring(args[++i]);
            else if (args[i] == L"--mount" && !mount) mount = std::wstring(args[++i]);
            else if (args[i] == L"--owner-sid" && !owner_sid) owner_sid = std::wstring(args[++i]);
            else return usage_error(L"Invalid or duplicate import option");
        }
        if (!name || !bundle_text || !mount) {
            return usage_error(L"Import requires name, bundle, and mount");
        }
        auto bundle = product::normalize_local_absolute_path(*bundle_text);
        if (!bundle.ok()) return usage_error(L"Import bundle path is invalid");
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskImportCommand{std::move(*name), bundle.move_value(),
                std::move(*mount), std::move(owner_sid)}});
    }
    if (args.size() >= 2 && args[0] == L"disk" && args[1] == L"create") {
        std::optional<std::wstring> name;
        std::optional<std::wstring> bundle_text;
        std::optional<std::wstring> size_text;
        std::optional<std::wstring> mount;
        std::optional<std::wstring> owner_sid;
        bool no_start = false;
        std::optional<control::MemoryMode> memory_mode;
        std::optional<std::wstring> hard_limit_text;
        std::optional<std::wstring> interval_text;
        std::optional<std::wstring> dirty_text;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--no-start" && !no_start) {
                no_start = true;
                continue;
            }
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                return usage_error(L"Create options require non-empty values");
            }
            if (args[i] == L"--name" && !name) name = std::wstring(args[++i]);
            else if (args[i] == L"--bundle" && !bundle_text) bundle_text = std::wstring(args[++i]);
            else if (args[i] == L"--size" && !size_text) size_text = std::wstring(args[++i]);
            else if (args[i] == L"--mount" && !mount) mount = std::wstring(args[++i]);
            else if (args[i] == L"--owner-sid" && !owner_sid) owner_sid = std::wstring(args[++i]);
            else if (args[i] == L"--memory-hard-limit" && !hard_limit_text) hard_limit_text = std::wstring(args[++i]);
            else if (args[i] == L"--checkpoint-interval" && !interval_text) interval_text = std::wstring(args[++i]);
            else if (args[i] == L"--dirty-threshold" && !dirty_text) dirty_text = std::wstring(args[++i]);
            else if (args[i] == L"--memory-mode" && !memory_mode) {
                const auto value = args[++i];
                if (value == L"automatic") memory_mode = control::MemoryMode::automatic;
                else if (value == L"fixed") memory_mode = control::MemoryMode::fixed;
                else return usage_error(L"Memory mode must be automatic or fixed");
            } else return usage_error(L"Invalid or duplicate create option");
        }
        if (!name || !bundle_text || !size_text || !mount) {
            return usage_error(L"Create requires name, bundle, size, and mount");
        }
        auto bundle = product::normalize_local_absolute_path(*bundle_text);
        auto size_ascii = narrow_ascii(*size_text);
        if (!bundle.ok() || !size_ascii.ok()) return usage_error(L"Invalid create bundle or size");
        auto size = product::parse_binary_size(size_ascii.value());
        if (!size.ok()) return usage_error(L"Invalid create size");

        std::optional<std::uint64_t> hard_limit;
        if (hard_limit_text) {
            auto ascii = narrow_ascii(*hard_limit_text);
            if (!ascii.ok()) return usage_error(L"Invalid hard limit");
            auto parsed = product::parse_binary_size(ascii.value());
            if (!parsed.ok()) return usage_error(L"Invalid hard limit");
            hard_limit = parsed.value();
        }
        std::optional<product::CheckpointInterval> interval;
        if (interval_text) {
            auto ascii = narrow_ascii(*interval_text);
            if (!ascii.ok()) return usage_error(L"Invalid checkpoint interval");
            auto parsed = product::parse_checkpoint_interval(ascii.value());
            if (!parsed.ok()) return usage_error(L"Invalid checkpoint interval");
            interval = parsed.value();
        }
        std::optional<control::OptionalSize> dirty;
        if (dirty_text) {
            if (*dirty_text == L"off") {
                dirty = control::OptionalSize{false, 0};
            } else {
                auto ascii = narrow_ascii(*dirty_text);
                if (!ascii.ok()) return usage_error(L"Invalid dirty threshold");
                auto parsed = product::parse_binary_size(ascii.value());
                if (!parsed.ok()) return usage_error(L"Invalid dirty threshold");
                dirty = control::OptionalSize{true, parsed.value()};
            }
        }
        if ((memory_mode == control::MemoryMode::fixed) != hard_limit.has_value()) {
            return usage_error(L"Fixed memory mode and hard limit must be specified together");
        }
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskCreateCommand{std::move(*name), bundle.move_value(), size.value(),
                std::move(*mount), std::move(owner_sid), no_start, memory_mode,
                hard_limit, interval, dirty}});
    }
    if (args.size() == 5 && args[0] == L"disk" && args[1] == L"stop"
        && !args[2].empty()) {
        const bool force_first = args[3] == L"--force"
            && args[4] == L"--accept-volatile-data-loss";
        const bool accept_first = args[3] == L"--accept-volatile-data-loss"
            && args[4] == L"--force";
        if (!force_first && !accept_first) {
            return usage_error(L"Force stop requires both confirmation flags");
        }
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskStopCommand{std::wstring(args[2]), true, true}});
    }
    if (args.size() == 3 && args[0] == L"disk" && args[1] == L"stop" && !args[2].empty()) {
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskStopCommand{std::wstring(args[2]), false}});
    }
    if (args.size() == 4 && args[0] == L"disk" && args[1] == L"repair-profile"
        && args[3] == L"--from-previous") {
        auto profile = parse_wide_uuid(args[2]);
        if (!profile.ok()) return usage_error(L"Profile ID must be a UUID");
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskRepairProfileCommand{profile.move_value(), true}});
    }
    if (args.size() == 7 && args[0] == L"disk" && args[1] == L"rebind") {
        auto profile = parse_wide_uuid(args[2]);
        if (!profile.ok()) return usage_error(L"Profile ID must be a UUID");
        std::optional<std::wstring_view> bundle_text;
        std::optional<std::wstring_view> image_text;
        for (std::size_t i = 3; i < args.size(); i += 2) {
            if (i + 1 >= args.size()) return usage_error(L"Rebind option needs a value");
            if (args[i] == L"--bundle" && !bundle_text) bundle_text = args[i + 1];
            else if (args[i] == L"--expect-image-uuid" && !image_text) image_text = args[i + 1];
            else return usage_error(L"Invalid or duplicate rebind option");
        }
        if (!bundle_text || !image_text) return usage_error(L"Rebind requires bundle and expected image UUID");
        auto bundle = product::normalize_local_absolute_path(*bundle_text);
        auto image = parse_wide_uuid(*image_text);
        if (!bundle.ok() || !image.ok()) return usage_error(L"Invalid rebind identity or path");
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskRebindCommand{profile.move_value(), bundle.move_value(), image.move_value()}});
    }
    if (args.size() == 5 && args[0] == L"disk" && args[1] == L"forget"
        && args[3] == L"--confirm-profile") {
        auto profile = parse_wide_uuid(args[2]);
        auto confirmation = parse_wide_uuid(args[4]);
        if (!profile.ok() || !confirmation.ok()
            || profile.value() != confirmation.value()) {
            return usage_error(L"Forget confirmation must match the profile UUID");
        }
        return Result<ParsedCommand>::success(ParsedCommand{options,
            control::DiskForgetCommand{profile.move_value(), confirmation.move_value()}});
    }
    if (args.size() == 3 && args[0] == L"operation") {
        auto operation_id = parse_wide_uuid(args[2]);
        if (!operation_id.ok()) {
            return usage_error(L"Operation ID must be a canonical lowercase UUID");
        }
        if (args[1] == L"show") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::OperationShowCommand{operation_id.move_value()}});
        }
        if (args[1] == L"wait") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::OperationWaitCommand{operation_id.move_value()}});
        }
        if (args[1] == L"cancel") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::OperationCancelCommand{operation_id.move_value()}});
        }
    }
    if ((args.size() == 3 || args.size() == 4) && args[0] == L"disk"
        && !args[2].empty()) {
        if (args.size() == 3 && args[1] == L"show") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::DiskShowCommand{std::wstring(args[2])}});
        }
        if (args.size() == 3 && args[1] == L"start") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::DiskStartCommand{std::wstring(args[2])}});
        }
        if (args.size() == 3 && args[1] == L"checkpoint") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::DiskCheckpointCommand{std::wstring(args[2])}});
        }
        if (args.size() == 3 && args[1] == L"compact") {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::DiskCompactCommand{std::wstring(args[2])}});
        }
        if (args.size() == 4 && args[1] == L"config" && args[2] == L"get"
            && !args[3].empty()) {
            return Result<ParsedCommand>::success(ParsedCommand{
                options, control::DiskConfigGetCommand{std::wstring(args[3])}});
        }
    }
    if (args.size() != 3 || args[0] != L"disk" || args[2].empty()) {
        return usage_error(L"Expected a supported command and exactly one non-empty target");
    }
    return usage_error(L"Unknown command");
}

Result<ParsedCommand> parse_cli(std::span<const std::wstring_view> args) {
    GlobalOptions options;
    std::vector<std::wstring_view> command_args;
    command_args.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--json") {
            if (options.json) return usage_error(L"Duplicate --json");
            options.json = true;
        } else if (args[i] == L"--no-wait") {
            if (options.no_wait) return usage_error(L"Duplicate --no-wait");
            options.no_wait = true;
        } else if (args[i] == L"--dry-run") {
            if (options.dry_run) return usage_error(L"Duplicate --dry-run");
            options.dry_run = true;
        } else if (args[i] == L"--timeout") {
            if (options.timeout || i + 1 >= args.size()) {
                return usage_error(L"Duplicate or missing --timeout value");
            }
            auto timeout = parse_timeout(args[++i]);
            if (!timeout.ok()) return usage_error(L"Invalid --timeout value");
            options.timeout = timeout.value();
        } else if (args[i] == L"--request-id") {
            if (options.request_id || i + 1 >= args.size()) {
                return usage_error(L"Duplicate or missing --request-id value");
            }
            auto request_id = parse_wide_uuid(args[++i]);
            if (!request_id.ok()) return usage_error(L"Invalid --request-id value");
            options.request_id = request_id.move_value();
        } else {
            command_args.push_back(args[i]);
        }
    }

    auto result = parse_command(command_args, options);
    if (!result.ok()) return result;
    const auto& command = result.value().command;
    if (options.no_wait && !control::is_long_operation(command)) {
        return usage_error(L"--no-wait is valid only for long mutating operations");
    }
    if (options.dry_run && !control::is_mutating(command)) {
        return usage_error(L"--dry-run is valid only for mutating commands");
    }
    if (options.no_wait && (options.dry_run || options.timeout.has_value())) {
        return usage_error(L"--no-wait cannot be combined with --dry-run or --timeout");
    }
    return result;
}

} // namespace myrd::cli
