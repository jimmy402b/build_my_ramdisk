#include "test_framework.h"

#include "src/poc/json_writer.h"
#include "src/poc/poc_report.h"

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
