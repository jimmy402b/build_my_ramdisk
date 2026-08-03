#include "test_framework.h"

#include "src/m2/m2_report.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

myrd::M2Report complete_report() {
    myrd::M2Report report;
    report.run = {"run-0123456789abcdef", myrd::M2RunMode::fake,
                  "2026-08-04T01:02:03Z", "2026-08-04T01:03:04Z"};
    report.build_toolchain = {
        "2.0.0", "5ee5a00", "Debug", "MSVC", "19.44", "4.1"};
    report.accepted_m1_baseline = {
        "m1-accepted.json", "abcdef0123456789", 1, "GO", true};
    report.machine_os = {
        "test-machine", "Windows", "11 24H2", "x64"};
    report.state_before = {"loaded", "deviosvc", "running"};
    report.state_after = {"loaded", "deviosvc", "running"};
    report.geometry_options = {
        1ULL << 40U, 64ULL << 10U, 1ULL << 30U, true};
    report.state = {myrd::M2InitialState::empty, 7, 9,
                    std::string(myrd::kM2NoDurableSuccessorStatement)};
    report.phases.push_back({
        "fake-acceptance", "2026-08-04T01:02:04Z",
        "2026-08-04T01:02:05Z", myrd::M2PhaseStatus::passed, "", 1000});
    report.io = {"write-sha256", "read-sha256",
                 (std::numeric_limits<std::uint64_t>::max)(), 1024, 512};
    report.store_stats = {11, 12, 13, 14};
    report.os_memory_samples.push_back({
        "2026-08-04T01:02:30Z", 100, 200, 300});
    report.performance.fixed = {1000, 64'000, 2'000};
    report.performance.dynamic = {1000, 64'000, 1'800};
    report.performance.ratio_numerator = 11;
    report.performance.ratio_denominator = 10;
    report.performance.ratio_basis_points = 11'000;
    report.performance.minimum_ratio_basis_points = 10'000;
    report.performance.gate_status = myrd::M2PerformanceGateStatus::passed;
    report.performance.comparable_policy = {
        "same-request-stream-v1", true, "identical geometry and workload"};
    report.device_identity = {
        "device-0001", 1, 2, 3, 42, R"(\\.\PhysicalDrive42)"};
    report.cleanup_actions.push_back({
        "remove-device", true, myrd::M2CleanupResult::succeeded, ""});
    report.final_baseline_comparison = {
        myrd::M2BaselineComparison::match, "before-hash", "before-hash"};
    report.verdict = myrd::M2Verdict::go;
    return report;
}

void mark_performance_failed(myrd::M2Report& report) {
    report.performance.ratio_numerator = 1;
    report.performance.ratio_denominator = 2;
    report.performance.ratio_basis_points = 5'000;
    report.performance.gate_status = myrd::M2PerformanceGateStatus::failed;
}

std::string replace_once(
    std::string input, std::string_view from, std::string_view to) {
    const auto position = input.find(from);
    REQUIRE(position != std::string::npos);
    input.replace(position, from.size(), to);
    return input;
}

std::string replace_last(
    std::string input, std::string_view from, std::string_view to) {
    const auto position = input.rfind(from);
    REQUIRE(position != std::string::npos);
    input.replace(position, from.size(), to);
    return input;
}

std::string omit_nth_key(
    std::string input, std::string_view key, std::size_t occurrence) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t position = 0;
    for (std::size_t index = 0; index <= occurrence; ++index) {
        position = input.find(needle, position);
        REQUIRE(position != std::string::npos);
        if (index != occurrence) {
            position += needle.size();
        }
    }
    input.replace(position + 1, key.size(), "omitted_" + std::string(key));
    return input;
}

void require_invalid(const std::string& json) {
    REQUIRE(myrd::validate_m2_report_json(json).has_error());
}

std::filesystem::path unique_test_directory() {
    const auto suffix = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("myrd-m2-report-test-" + suffix);
}

} // namespace

TEST_CASE(m2_report_enum_strings_are_stable_and_unknown_values_are_rejected) {
    REQUIRE_EQ(myrd::to_string(myrd::M2RunMode::fake), "FAKE");
    REQUIRE_EQ(myrd::to_string(myrd::M2InitialState::empty), "EMPTY");
    REQUIRE_EQ(myrd::to_string(myrd::M2InitialState::seeded), "SEEDED");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::not_run), "NOT-RUN");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::passed), "PASS");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::failed), "FAIL");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::not_attempted),
               "NOT-ATTEMPTED");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::succeeded), "SUCCEEDED");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::failed), "FAILED");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::not_evaluated),
               "NOT-EVALUATED");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::passed), "PASS");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::failed), "FAIL");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::not_compared),
               "NOT-COMPARED");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::match), "MATCH");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::mismatch),
               "MISMATCH");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::incomplete), "INCOMPLETE");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::go), "GO");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::no_go), "NO-GO");

    REQUIRE(!myrd::parse_m2_run_mode("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_initial_state("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_phase_status("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_cleanup_result("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_performance_gate_status("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_baseline_comparison("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_verdict("UNKNOWN"));
}

TEST_CASE(m2_report_serializes_and_schema_names_every_required_field) {
    const auto json = myrd::serialize_m2_report(complete_report());
    auto no_go_report = complete_report();
    mark_performance_failed(no_go_report);
    no_go_report.failures.push_back({"performance", 23, "threshold missed"});
    no_go_report.verdict = myrd::M2Verdict::no_go;
    const auto serialized_examples =
        json + myrd::serialize_m2_report(no_go_report);
    const auto schema = myrd::m2_report_json_schema();
    REQUIRE(!schema.empty());
    REQUIRE(schema.front() == '{');
    REQUIRE(schema.find("\"$schema\"") != std::string::npos);
    REQUIRE(schema.find("\"required\"") != std::string::npos);

    const std::vector<std::string_view> required_keys = {
        "schema_version", "run", "run_id", "mode", "started_utc",
        "ended_utc", "build_toolchain", "build_version", "git_commit",
        "configuration", "compiler", "compiler_version", "cmake_version",
        "accepted_m1_baseline", "report_path", "report_sha256",
        "m1_schema_version", "m1_verdict", "accepted", "machine_os",
        "machine_name", "os_name", "os_version", "architecture",
        "state_before", "state_after", "driver_state", "service_name",
        "service_state", "geometry_options", "disk_size_bytes",
        "block_size_bytes", "memory_limit_bytes", "fake_backing",
        "state", "initial_state", "initial_backing_generation",
        "current_backing_generation", "durability_scope_statement",
        "phases", "phase_name", "phase_started_utc", "phase_ended_utc",
        "phase_status", "phase_error", "phase_elapsed_ms", "io",
        "write_sha256", "read_sha256", "bytes_written", "bytes_read",
        "bytes_deleted", "store_stats", "view_install_count",
        "generation_conflict_count", "stale_load_count",
        "stale_load_retry_count", "os_memory_samples", "captured_utc",
        "working_set_bytes", "private_bytes", "commit_bytes",
        "performance", "fixed", "dynamic", "operations", "bytes",
        "elapsed_us", "ratio_numerator", "ratio_denominator",
        "ratio_basis_points", "minimum_ratio_basis_points", "gate_status",
        "comparable_policy", "policy_name", "same_workload", "notes",
        "device_identity", "device_id", "path_id", "target_id", "lun",
        "physical_drive_number", "physical_drive_path", "cleanup_actions",
        "cleanup_name", "cleanup_attempted", "cleanup_result",
        "cleanup_error", "final_baseline_comparison", "comparison",
        "before_fingerprint", "after_fingerprint", "failures",
        "failure_phase", "failure_code", "failure_message", "verdict"};

    for (const auto key : required_keys) {
        const std::string quoted = "\"" + std::string(key) + "\"";
        REQUIRE(serialized_examples.find(quoted) != std::string::npos);
        REQUIRE(schema.find(quoted) != std::string::npos);
    }
    REQUIRE(myrd::validate_m2_report_json(json).ok());
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(no_go_report)).ok());
}

TEST_CASE(m2_report_preserves_uint64_max_as_an_unquoted_decimal_integer) {
    const auto json = myrd::serialize_m2_report(complete_report());
    REQUIRE(json.find("\"bytes_written\": 18446744073709551615") !=
            std::string::npos);
    REQUIRE(json.find("\"18446744073709551615\"") == std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(json).ok());
}

TEST_CASE(m2_report_validator_rejects_each_missing_required_member) {
    const auto json = myrd::serialize_m2_report(complete_report());
    const std::vector<std::string_view> once = {
        "schema_version", "run", "run_id", "mode", "started_utc",
        "ended_utc", "build_toolchain", "build_version", "git_commit",
        "configuration", "compiler", "compiler_version", "cmake_version",
        "accepted_m1_baseline", "report_path", "report_sha256",
        "m1_schema_version", "m1_verdict", "accepted", "machine_os",
        "machine_name", "os_name", "os_version", "architecture",
        "state_before", "state_after", "geometry_options", "disk_size_bytes",
        "block_size_bytes", "memory_limit_bytes", "fake_backing", "state",
        "initial_state", "initial_backing_generation",
        "current_backing_generation", "durability_scope_statement", "phases",
        "phase_name", "phase_started_utc", "phase_ended_utc", "phase_status",
        "phase_error", "phase_elapsed_ms", "io", "write_sha256",
        "read_sha256", "bytes_written", "bytes_read", "bytes_deleted",
        "store_stats", "view_install_count", "generation_conflict_count",
        "stale_load_count", "stale_load_retry_count", "os_memory_samples",
        "captured_utc", "working_set_bytes", "private_bytes", "commit_bytes",
        "performance", "ratio_numerator", "ratio_denominator",
        "ratio_basis_points", "minimum_ratio_basis_points", "gate_status",
        "comparable_policy", "policy_name", "same_workload", "notes",
        "device_identity", "device_id", "path_id", "target_id", "lun",
        "physical_drive_number", "physical_drive_path", "cleanup_actions",
        "cleanup_name", "cleanup_attempted", "cleanup_result", "cleanup_error",
        "final_baseline_comparison", "comparison", "before_fingerprint",
        "after_fingerprint", "failures", "verdict"};
    for (const auto key : once) {
        require_invalid(omit_nth_key(json, key, 0));
    }

    for (const auto key : {"driver_state", "service_name", "service_state"}) {
        require_invalid(omit_nth_key(json, key, 0));
        require_invalid(omit_nth_key(json, key, 1));
    }
    for (const auto key : {"fixed", "dynamic", "operations", "bytes",
                           "elapsed_us"}) {
        require_invalid(omit_nth_key(json, key, 0));
        if (key == std::string_view("operations") ||
            key == std::string_view("bytes") ||
            key == std::string_view("elapsed_us")) {
            require_invalid(omit_nth_key(json, key, 1));
        }
    }

    auto no_go = complete_report();
    mark_performance_failed(no_go);
    no_go.failures.push_back({"performance", 23, "threshold missed"});
    no_go.verdict = myrd::M2Verdict::no_go;
    const auto no_go_json = myrd::serialize_m2_report(no_go);
    for (const auto key : {
             "failure_phase", "failure_code", "failure_message"}) {
        require_invalid(omit_nth_key(no_go_json, key, 0));
    }
}

TEST_CASE(m2_report_validator_rejects_malformed_duplicate_and_trailing_json) {
    require_invalid("{");
    require_invalid(myrd::serialize_m2_report(complete_report()) + " garbage");
    auto duplicate = myrd::serialize_m2_report(complete_report());
    duplicate = replace_once(duplicate, "\"schema_version\": 1",
                             "\"schema_version\": 1, \"schema_version\": 1");
    require_invalid(duplicate);
}

TEST_CASE(m2_report_validator_rejects_unknown_enums_and_bad_integer_spellings) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_last(
        json, "\"verdict\": \"GO\"", "\"verdict\": \"MAYBE\""));
    require_invalid(replace_once(
        json, "\"phase_status\": \"PASS\"",
        "\"phase_status\": \"UNKNOWN\""));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024", "\"bytes_read\": -1"));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024", "\"bytes_read\": \"1024\""));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024",
        "\"bytes_read\": 18446744073709551616"));
    require_invalid(replace_once(
        json, "\"schema_version\": 1", "\"schema_version\": 2"));
    require_invalid(replace_once(
        json, "\"schema_version\": 1",
        "\"schema_version\": 4294967296"));
}

TEST_CASE(m2_report_validator_rejects_every_inconsistent_go_verdict) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json, "\"phase_status\": \"PASS\"",
        "\"phase_status\": \"NOT-RUN\""));

    auto with_failure = complete_report();
    with_failure.failures.push_back({"fake-acceptance", 5, "failed"});
    require_invalid(myrd::serialize_m2_report(with_failure));

    require_invalid(replace_once(
        json, "\"cleanup_result\": \"SUCCEEDED\"",
        "\"cleanup_result\": \"FAILED\""));
    require_invalid(replace_once(
        json, "\"comparison\": \"MATCH\"",
        "\"comparison\": \"MISMATCH\""));
    require_invalid(replace_once(
        json, "\"gate_status\": \"PASS\"",
        "\"gate_status\": \"FAIL\""));
}

TEST_CASE(m2_report_validator_rejects_go_when_match_fingerprints_differ) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json, "\"after_fingerprint\": \"before-hash\"",
        "\"after_fingerprint\": \"different-hash\""));
}

TEST_CASE(m2_report_validator_cross_checks_performance_gate_and_ratio) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json, "\"ratio_basis_points\": 11000",
        "\"ratio_basis_points\": 9999"));
    require_invalid(replace_once(
        json, "\"minimum_ratio_basis_points\": 10000",
        "\"minimum_ratio_basis_points\": 12000"));

    auto failed_above_threshold = complete_report();
    failed_above_threshold.performance.gate_status =
        myrd::M2PerformanceGateStatus::failed;
    failed_above_threshold.failures.push_back(
        {"performance", 23, "threshold missed"});
    failed_above_threshold.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(failed_above_threshold));
}

TEST_CASE(m2_report_failure_forces_no_go_even_if_other_work_is_incomplete) {
    auto report = complete_report();
    report.phases.front().status = myrd::M2PhaseStatus::not_run;
    report.cleanup_actions.front() = {
        "remove-device", true, myrd::M2CleanupResult::failed,
        "cleanup failed"};
    report.failures.push_back({"cleanup", 31, "cleanup failed"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_accepts_consistent_incomplete_and_no_go_reports) {
    auto incomplete = complete_report();
    incomplete.phases.front().status = myrd::M2PhaseStatus::not_run;
    incomplete.cleanup_actions.front() = {
        "remove-device", false, myrd::M2CleanupResult::not_attempted, ""};
    incomplete.performance.gate_status =
        myrd::M2PerformanceGateStatus::not_evaluated;
    incomplete.final_baseline_comparison.comparison =
        myrd::M2BaselineComparison::not_compared;
    incomplete.verdict = myrd::M2Verdict::incomplete;
    const auto incomplete_json = myrd::serialize_m2_report(incomplete);
    REQUIRE(incomplete_json.find("\"verdict\": \"INCOMPLETE\"") !=
            std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(incomplete_json).ok());

    auto no_go = complete_report();
    mark_performance_failed(no_go);
    no_go.failures.push_back({"performance", 23, "threshold missed"});
    no_go.verdict = myrd::M2Verdict::no_go;
    const auto no_go_json = myrd::serialize_m2_report(no_go);
    REQUIRE(no_go_json.find("\"verdict\": \"NO-GO\"") !=
            std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(no_go_json).ok());
}

TEST_CASE(m2_report_paths_are_unique_and_require_an_existing_evidence_directory) {
    const auto root = unique_test_directory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    REQUIRE(!error);

    const auto first = myrd::make_unique_m2_report_path(root, "run/id");
    const auto second = myrd::make_unique_m2_report_path(root, "run/id");
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.value() != second.value());
    REQUIRE_EQ(first.value().parent_path(), root);
    REQUIRE_EQ(second.value().parent_path(), root);
    REQUIRE(first.value().extension() == ".json");
    REQUIRE(first.value().filename().wstring().find(L"run_id") !=
            std::wstring::npos);

    const auto missing = myrd::make_unique_m2_report_path(
        root / "does-not-exist", "run");
    REQUIRE(missing.has_error());
    std::filesystem::remove_all(root, error);
}

TEST_CASE(m2_report_publication_is_atomic_no_overwrite_and_valid_after_readback) {
    const auto root = unique_test_directory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    REQUIRE(!error);
    const auto path = myrd::make_unique_m2_report_path(root, "run");
    REQUIRE(path.ok());

    const auto first = myrd::publish_m2_report(complete_report(), path.value());
    REQUIRE(first.ok());
    REQUIRE(std::filesystem::exists(path.value(), error));
    REQUIRE(myrd::validate_published_m2_report(path.value()).ok());

    const auto second = myrd::publish_m2_report(complete_report(), path.value());
    REQUIRE(second.has_error());
    REQUIRE(myrd::validate_published_m2_report(path.value()).ok());

    std::ifstream input(path.value(), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    REQUIRE_EQ(content, myrd::serialize_m2_report(complete_report()));

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        REQUIRE(entry.path().filename().wstring().find(L".tmp-") ==
                std::wstring::npos);
    }
    std::filesystem::remove_all(root, error);
}
