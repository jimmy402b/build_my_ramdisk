#include "cli.h"

#include <algorithm>
#include <string>

namespace myrd::poc {

namespace {
bool is_device_namespace(const std::filesystem::path& p) {
    auto s = p.wstring();
    return s.starts_with(L"\\\\.\\") || s.starts_with(L"\\\\?\\");
}
} // anonymous namespace

Result<CommandLine> parse_command_line(
    std::span<const std::wstring_view> args, bool debug_build) {

    if (args.empty()) {
        return Result<CommandLine>::failure(
            {ErrorDomain::usage, 2, L"parse_command_line",
             L"no command specified"});
    }

    CommandLine result{};
    bool mode_set = false;
    bool report_set = false;
    bool failure_set = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];

        if (arg == L"query-driver") {
            if (mode_set) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"duplicate command"});
            }
            result.mode = CommandMode::query_driver;
            mode_set = true;
        } else if (arg == L"run") {
            if (mode_set) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"duplicate command"});
            }
            result.mode = CommandMode::run;
            mode_set = true;
        } else if (arg == L"--report") {
            if (report_set) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"duplicate --report"});
            }
            if (i + 1 >= args.size()) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"--report requires a path"});
            }
            std::filesystem::path p(args[++i]);
            if (is_device_namespace(p)) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"device namespace path rejected"});
            }
            result.report_path = std::filesystem::absolute(p);
            report_set = true;
        } else if (arg == L"--inject-failure-after") {
            if (failure_set) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"duplicate --inject-failure-after"});
            }
            if (!debug_build) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"--inject-failure-after is only available in debug builds"});
            }
            if (i + 1 >= args.size()) {
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"--inject-failure-after requires a stage"});
            }
            auto stage = std::wstring(args[++i]);
            std::transform(stage.begin(), stage.end(), stage.begin(), ::towlower);
            if (stage == L"create_returned")
                result.injected_failure = FailurePoint::create_returned;
            else if (stage == L"device_confirmed")
                result.injected_failure = FailurePoint::device_confirmed;
            else if (stage == L"physical_drive_confirmed")
                result.injected_failure = FailurePoint::physical_drive_confirmed;
            else if (stage == L"round_trip_verified")
                result.injected_failure = FailurePoint::round_trip_verified;
            else
                return Result<CommandLine>::failure(
                    {ErrorDomain::usage, 2, L"parse_command_line",
                     L"unknown failure injection stage"});
            failure_set = true;
        } else {
            return Result<CommandLine>::failure(
                {ErrorDomain::usage, 2, L"parse_command_line",
                 L"unknown argument: " + std::wstring(arg)});
        }
    }

    if (!mode_set) {
        return Result<CommandLine>::failure(
            {ErrorDomain::usage, 2, L"parse_command_line",
             L"no command specified"});
    }
    if (!report_set) {
        return Result<CommandLine>::failure(
            {ErrorDomain::usage, 2, L"parse_command_line",
             L"--report is required"});
    }
    if (result.mode == CommandMode::query_driver &&
        result.injected_failure != FailurePoint::none) {
        return Result<CommandLine>::failure(
            {ErrorDomain::usage, 2, L"parse_command_line",
             L"failure injection not allowed for query-driver"});
    }

    return Result<CommandLine>::success(result);
}

} // namespace myrd::poc
