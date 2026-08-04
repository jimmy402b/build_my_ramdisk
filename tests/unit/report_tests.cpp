#include "test_framework.h"

#include "src/poc/json_writer.h"
#include "src/poc/poc_report.h"
#include "src/poc/report_publisher.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>

TEST_CASE(json_writer_escapes_quote_backslash_and_controls) {
    myrd::poc::JsonWriter w;
    w.start_object();
    w.key("test"); w.string_value("a\"b\\c\nd");
    w.end_object();
    auto s = w.finish();
    // Must contain escaped quote and backslash
    REQUIRE(s.find("\\\"") != std::string::npos);
    REQUIRE(s.find("\\\\") != std::string::npos);
    REQUIRE(s.find("\\n") != std::string::npos);
}

TEST_CASE(json_writer_emits_booleans_as_json_booleans) {
    myrd::poc::JsonWriter w;
    w.start_object();
    w.key("t"); w.boolean_value(true);
    w.key("f"); w.boolean_value(false);
    w.end_object();
    auto s = w.finish();
    REQUIRE(s.find("true") != std::string::npos);
    REQUIRE(s.find("false") != std::string::npos);
}

TEST_CASE(json_writer_emits_integers_without_quotes) {
    myrd::poc::JsonWriter w;
    w.start_object();
    w.key("n"); w.uint_value(42);
    w.end_object();
    auto s = w.finish();
    REQUIRE(s.find("42") != std::string::npos);
    REQUIRE(s.find("\"42\"") == std::string::npos);
}

TEST_CASE(json_writer_emits_null_optional_sections) {
    myrd::poc::JsonWriter w;
    w.start_object();
    w.key("maybe"); w.null_value();
    w.end_object();
    auto s = w.finish();
    REQUIRE(s.find("null") != std::string::npos);
}

TEST_CASE(report_contains_every_required_top_level_key) {
    myrd::poc::PocReport r;
    r.mode = "test";
    r.run_id = "deadbeef";
    auto s = myrd::poc::serialize_report(r);
    REQUIRE(s.find("\"schema_version\"") != std::string::npos);
    REQUIRE(s.find("\"mode\"") != std::string::npos);
    REQUIRE(s.find("\"environment\"") != std::string::npos);
    REQUIRE(s.find("\"dependency_audit\"") != std::string::npos);
    REQUIRE(s.find("\"driver\"") != std::string::npos);
    REQUIRE(s.find("\"baseline\"") != std::string::npos);
    REQUIRE(s.find("\"provider\"") != std::string::npos);
    REQUIRE(s.find("\"created_device\"") != std::string::npos);
    REQUIRE(s.find("\"cleanup\"") != std::string::npos);
}

TEST_CASE(report_preserves_cleanup_and_primary_error_together) {
    myrd::poc::PocReport r;
    r.mode = "test";
    r.primary_error = myrd::poc::ErrorEvidence{
        "protocol", 0x801, "op", "msg"};
    r.cleanup_errors.push_back(myrd::poc::ErrorEvidence{
        "win32", 5, "op2", "msg2"});
    auto s = myrd::poc::serialize_report(r);
    REQUIRE(s.find("\"primary_error\"") != std::string::npos);
    REQUIRE(s.find("\"cleanup_errors\"") != std::string::npos);
    REQUIRE(s.find("\"protocol\"") != std::string::npos);
}

TEST_CASE(report_utf8_encodes_non_ascii_signer) {
    // Test with a string that has non-ASCII characters
    myrd::poc::JsonWriter w;
    w.start_object();
    w.key("signer"); w.wstring_value(
        std::wstring_view(L"Test\U000000e9")); // e-acute
    w.end_object();
    auto s = w.finish();
    REQUIRE(s.find("\\u00e9") != std::string::npos ||
            s.find("é") != std::string::npos ||
            s.find("\xC3\xA9") != std::string::npos);
}

TEST_CASE(report_publisher_creates_parent_and_rejects_overwrite) {
    const auto unique_suffix = std::to_string(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("myrd-report-publisher-test-" + unique_suffix);
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    const auto target = root / "nested" / "report.json";

    auto first = myrd::poc::publish_report_atomic("{\"value\":1}\n", target);
    REQUIRE(first.ok());
    REQUIRE(std::filesystem::exists(target, filesystem_error));

    auto second = myrd::poc::publish_report_atomic("{\"value\":2}\n", target);
    REQUIRE(second.has_error());

    std::ifstream input(target, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    REQUIRE_EQ(content, std::string("{\"value\":1}\n"));
    std::filesystem::remove_all(root, filesystem_error);
}

// ── CLI tests (Task 18) ────────────────────────────────────────

TEST_CASE(report_publisher_supports_a_temporary_path_beyond_max_path) {
    const auto unique_suffix = std::to_string(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("myrd-report-long-path-test-" + unique_suffix);
    auto target = root;
    for (int index = 0; index < 6; ++index) {
        target /= "forty-character-directory-component-000000";
    }
    target /= "report.json";
    REQUIRE(target.wstring().size() > 260);

    const auto first =
        myrd::poc::publish_report_atomic("{\"long\":true}\n", target);
    REQUIRE(first.ok());
    const auto second =
        myrd::poc::publish_report_atomic("{\"long\":false}\n", target);
    REQUIRE(second.has_error());

    std::error_code filesystem_error;
    const auto extended_root = std::filesystem::path(
        L"\\\\?\\" + std::filesystem::absolute(root).wstring());
    std::filesystem::remove_all(extended_root, filesystem_error);
}

#include "src/poc/cli.h"

static std::vector<std::wstring_view> mk(std::initializer_list<const wchar_t*> il) {
    std::vector<std::wstring_view> v;
    for (auto p : il) v.push_back(std::wstring_view(p));
    return v;
}

TEST_CASE(cli_parses_query_driver_report) {
    auto av = mk({L"query-driver", L"--report", L"reports/t.json"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.ok());
    REQUIRE(r.value().mode == myrd::poc::CommandMode::query_driver);
}

TEST_CASE(cli_parses_run_report) {
    auto av = mk({L"run", L"--report", L"reports/t.json"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.ok());
}

TEST_CASE(cli_rejects_unknown_command) {
    auto av = mk({L"invalid", L"--report", L"r.json"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.has_error());
}

TEST_CASE(cli_rejects_missing_report) {
    auto av = mk({L"query-driver"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.has_error());
}

TEST_CASE(cli_rejects_duplicate_report) {
    auto av = mk({L"query-driver", L"--report", L"a.json", L"--report", L"b.json"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.has_error());
}

TEST_CASE(cli_rejects_unknown_option) {
    auto av = mk({L"query-driver", L"--report", L"r.json", L"--unknown"});
    auto r = myrd::poc::parse_command_line(av, false);
    REQUIRE(r.has_error());
}

TEST_CASE(cli_accepts_failure_injection_only_in_debug) {
    auto av = mk({L"run", L"--report", L"r.json",
        L"--inject-failure-after", L"create_returned"});
    auto r1 = myrd::poc::parse_command_line(av, true);
    REQUIRE(r1.ok());
    auto r2 = myrd::poc::parse_command_line(av, false);
    REQUIRE(r2.has_error());
}

TEST_CASE(cli_rejects_failure_injection_for_query_driver) {
    auto av = mk({L"query-driver", L"--report", L"r.json",
        L"--inject-failure-after", L"create_returned"});
    auto r = myrd::poc::parse_command_line(av, true);
    REQUIRE(r.has_error());
}
