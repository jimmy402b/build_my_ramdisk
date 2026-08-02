#include "test_framework.h"

#include "src/m1/m1_report.h"

#include <string>

TEST_CASE(m1_report_serializes_exact_required_top_level_fields) {
    myrd::M1Report report;
    report.mode = "run";
    report.run_id = "0123456789abcdef0123456789abcdef";
    const auto json = myrd::serialize_m1_report(report);

    for (const auto* key : {
             "schema_version", "mode", "run_id", "started_utc",
             "finished_utc", "success", "exit_code", "last_stage",
             "driver", "baseline", "provider", "created_device",
             "raw_protocol", "cleanup", "primary_error", "cleanup_errors"}) {
        REQUIRE(json.find(std::string("\"") + key + "\"") !=
                std::string::npos);
    }
    REQUIRE(json.find("report_path") == std::string::npos);
}
TEST_CASE(m1_report_serializes_all_provider_counters_and_diagnostics) {
    myrd::M1Report report;
    report.provider.info_count = 1;
    report.provider.read_count = 2;
    report.provider.write_count = 3;
    report.provider.unmap_count = 4;
    report.provider.zero_count = 5;
    report.provider.close_count = 6;
    report.provider.info_error_count = 7;
    report.provider.read_error_count = 8;
    report.provider.write_error_count = 9;
    report.provider.unmap_error_count = 10;
    report.provider.zero_error_count = 11;
    report.provider.fatal_protocol_error_count = 12;
    report.provider.cancelled = 13;
    report.provider.timed_out = 14;
    report.provider.diagnostics.push_back({
        15, 3, 4096, 512, "write", {"storage", 0x1006, 23,
        "write", "failed"}});

    const auto json = myrd::serialize_m1_report(report);
    for (const auto* key : {
             "info_count", "read_count", "write_count", "unmap_count",
             "zero_count", "close_count", "info_error_count",
             "read_error_count", "write_error_count", "unmap_error_count",
             "zero_error_count", "fatal_protocol_error_count", "cancelled",
             "timed_out", "diagnostics", "native_code"}) {
        REQUIRE(json.find(std::string("\"") + key + "\"") !=
                std::string::npos);
    }
    REQUIRE(json.find("block data") == std::string::npos);
}

TEST_CASE(m1_report_preserves_internal_native_primary_and_cleanup_errors) {
    myrd::M1Report report;
    report.primary_error = myrd::M1ErrorEvidence{
        "protocol", 0x1002, 87, "query", "bad request"};
    report.cleanup_errors.push_back(myrd::M1ErrorEvidence{
        "cleanup", 0x100C, 1167, "remove", "not found"});
    const auto json = myrd::serialize_m1_report(report);
    REQUIRE(json.find("4098") != std::string::npos);
    REQUIRE(json.find("87") != std::string::npos);
    REQUIRE(json.find("1167") != std::string::npos);
    REQUIRE(json.find("bad request") != std::string::npos);
    REQUIRE(json.find("not found") != std::string::npos);
}

TEST_CASE(m1_report_emits_null_optional_sections_without_fake_evidence) {
    myrd::M1Report report;
    const auto json = myrd::serialize_m1_report(report);
    REQUIRE(json.find("\"created_device\": null") != std::string::npos);
    REQUIRE(json.find("\"raw_protocol\": null") != std::string::npos);
    REQUIRE(json.find("\"primary_error\": null") != std::string::npos);
}
