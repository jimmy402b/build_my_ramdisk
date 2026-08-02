#include "m1_cli.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace myrd {

namespace {

Result<M1CliConfig> usage_error(const wchar_t* message) {
    return Result<M1CliConfig>::failure(
        {ErrorDomain::usage, 2, L"parse_m1_cli", message});
}

bool is_device_namespace(const std::filesystem::path& path) {
    const auto value = path.wstring();
    return value.starts_with(L"\\\\.\\") || value.starts_with(L"\\\\?\\");
}

bool is_strict_descendant(const std::filesystem::path& child,
                          const std::filesystem::path& parent) {
    auto child_it = child.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end();
         ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) return false;
    }
    return child_it != child.end();
}

Result<M1FailurePoint> parse_failure_point(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    if (value == L"provider_ready")
        return Result<M1FailurePoint>::success(M1FailurePoint::provider_ready);
    if (value == L"create_returned")
        return Result<M1FailurePoint>::success(M1FailurePoint::create_returned);
    if (value == L"device_confirmed")
        return Result<M1FailurePoint>::success(M1FailurePoint::device_confirmed);
    if (value == L"physical_drive_confirmed")
        return Result<M1FailurePoint>::success(
            M1FailurePoint::physical_drive_confirmed);
    if (value == L"write_verified")
        return Result<M1FailurePoint>::success(M1FailurePoint::write_verified);
    if (value == L"zero_verified")
        return Result<M1FailurePoint>::success(M1FailurePoint::zero_verified);
    if (value == L"unmap_verified")
        return Result<M1FailurePoint>::success(M1FailurePoint::unmap_verified);
    if (value == L"device_removed")
        return Result<M1FailurePoint>::success(M1FailurePoint::device_removed);
    return Result<M1FailurePoint>::failure(
        {ErrorDomain::usage, 2, L"parse_m1_cli",
         L"unknown failure injection stage"});
}

} // namespace

Result<M1CliConfig> parse_m1_cli(
    std::span<const std::wstring_view> args, bool debug_build) {
    M1CliConfig result;
    bool command_seen = false;
    bool report_seen = false;
    bool failure_seen = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto argument = args[index];
        if (argument == L"run") {
            if (command_seen) return usage_error(L"duplicate command");
            command_seen = true;
        } else if (argument == L"--report") {
            if (report_seen) return usage_error(L"duplicate --report");
            if (++index >= args.size()) {
                return usage_error(L"--report requires a path");
            }
            std::filesystem::path path(args[index]);
            if (path.empty() || is_device_namespace(path)) {
                return usage_error(L"invalid report path");
            }
            std::error_code error;
            const auto reports_root = std::filesystem::absolute(
                L"reports", error).lexically_normal();
            if (error) return usage_error(L"cannot resolve reports root");
            const auto absolute_path = std::filesystem::absolute(
                path, error).lexically_normal();
            if (error || !is_strict_descendant(absolute_path, reports_root)) {
                return usage_error(L"report path must be below repository reports");
            }
            if (std::filesystem::exists(absolute_path, error)) {
                return usage_error(L"refusing to overwrite existing report");
            }
            if (error) return usage_error(L"cannot inspect report path");
            result.report_path = absolute_path;
            report_seen = true;
        } else if (argument == L"--inject-failure-after") {
            if (failure_seen) {
                return usage_error(L"duplicate --inject-failure-after");
            }
            if (!debug_build) {
                return usage_error(
                    L"failure injection is unavailable in Release builds");
            }
            if (++index >= args.size()) {
                return usage_error(L"--inject-failure-after requires a stage");
            }
            auto point = parse_failure_point(std::wstring(args[index]));
            if (!point) return Result<M1CliConfig>::failure(point.error());
            result.failure_point = point.value();
            failure_seen = true;
        } else {
            return usage_error(L"unknown argument");
        }
    }

    if (!command_seen) return usage_error(L"run command is required");
    if (!report_seen) return usage_error(L"--report is required");
    return Result<M1CliConfig>::success(std::move(result));
}

} // namespace myrd
