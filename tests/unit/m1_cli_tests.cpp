#include "test_framework.h"

#include "src/m1/m1_cli.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace {

std::vector<std::wstring_view> args(
    std::initializer_list<const wchar_t*> values) {
    std::vector<std::wstring_view> result;
    for (const auto* value : values) result.emplace_back(value);
    return result;
}
} // namespace

TEST_CASE(m1_cli_accepts_new_report_below_repository_reports) {
    auto parsed = myrd::parse_m1_cli(
        args({L"run", L"--report", L"reports/m1-cli-new.json"}), true);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().report_path.is_absolute());
    REQUIRE(parsed.value().failure_point == myrd::M1FailurePoint::none);
}

TEST_CASE(m1_cli_rejects_missing_duplicate_and_unknown_arguments) {
    REQUIRE(myrd::parse_m1_cli(args({L"run"}), true).has_error());
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"--report", L"reports/a.json",
              L"--report", L"reports/b.json"}), true).has_error());
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"--report", L"reports/a.json", L"--wat"}),
        true).has_error());
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"run", L"--report", L"reports/a.json"}),
        true).has_error());
}

TEST_CASE(m1_cli_rejects_paths_outside_reports_and_device_namespaces) {
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"--report", L"outside.json"}), true).has_error());
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"--report", L"reports/../outside.json"}),
        true).has_error());
    REQUIRE(myrd::parse_m1_cli(
        args({L"run", L"--report", L"\\\\.\\PhysicalDrive0"}),
        true).has_error());
}

TEST_CASE(m1_cli_rejects_existing_report) {
    auto parsed = myrd::parse_m1_cli(
        args({L"run", L"--report", L"M1_TASKLIST.md"}), true);
    REQUIRE(parsed.has_error());
}

TEST_CASE(m1_cli_supports_all_debug_failure_points_and_rejects_release) {
    for (const auto* stage : {
             L"provider_ready", L"create_returned", L"device_confirmed",
             L"physical_drive_confirmed", L"write_verified",
             L"zero_verified", L"unmap_verified", L"device_removed"}) {
        auto values = args({L"run", L"--report", L"reports/m1-cli-new.json",
                            L"--inject-failure-after", stage});
        REQUIRE(myrd::parse_m1_cli(values, true).ok());
        REQUIRE(myrd::parse_m1_cli(values, false).has_error());
    }
}
