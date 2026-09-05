#include "src/setup/driver_manifest.h"

#include "tests/support/test_framework.h"

#include <string>

using namespace myrd::setup;
using namespace myrd::product;

// ---------------------------------------------------------------------------
// 11.1 — exact frozen pins
// ---------------------------------------------------------------------------
TEST_CASE(manifest_pins_exact_commit)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.upstream_commit,
               "cb7a2347e9595b12e2bd178ce803090a7a674037");
}

TEST_CASE(manifest_pins_exact_archive_url)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.archive_url,
               "https://raw.githubusercontent.com/ArsenalRecon/"
               "Arsenal-Image-Mounter/"
               "cb7a2347e9595b12e2bd178ce803090a7a674037/"
               "DriverSetup/DriverFiles.zip");
}

TEST_CASE(manifest_pins_exact_archive_sha256)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.archive_sha256,
               "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
}

TEST_CASE(manifest_pins_exact_x64_sys_sha256)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.x64_sys_sha256,
               "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9");
}

TEST_CASE(manifest_pins_exact_driver_version)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.driver_version, "1.2.28.93");
}

TEST_CASE(manifest_pins_architecture_x64)
{
    const auto& m = driver_manifest();
    REQUIRE(m.architecture == DriverArchitecture::x64);
}

TEST_CASE(manifest_pins_service_name)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.service_name, "phdskmnt");
}

TEST_CASE(manifest_is_immutable_singleton)
{
    const auto& a = driver_manifest();
    const auto& b = driver_manifest();
    REQUIRE(&a == &b);
}

TEST_CASE(manifest_no_alternate_url_or_fallback_member)
{
    // The manifest struct has no field for an alternate URL or latest-version
    // mechanism.  This is a compile-time property; the test confirms the
    // singleton is immutable and exposes only the frozen pin.
    const auto& m = driver_manifest();
    REQUIRE(!m.archive_url.empty());
    REQUIRE(!m.archive_sha256.empty());
    REQUIRE(!m.x64_sys_sha256.empty());
    REQUIRE(!m.driver_version.empty());
    REQUIRE(!m.upstream_commit.empty());
}

// ---------------------------------------------------------------------------
// 11.1 — strict SHA-256 hex parser
// ---------------------------------------------------------------------------
TEST_CASE(parse_sha256_exact_64_hex_uppercase_output)
{
    auto r = parse_sha256_hex(
        "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(),
               "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
}

TEST_CASE(parse_sha256_lowercase_input_canonical_uppercase_output)
{
    auto r = parse_sha256_hex(
        "b889a4baae15ce4f6ddd94327bcc56c9b1d0fc442ddb3e0a1fd233f8d12bd324");
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(),
               "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
}

TEST_CASE(parse_sha256_mixed_case_canonical_uppercase_output)
{
    auto r = parse_sha256_hex(
        "B889a4bAAE15CE4F6Ddd94327bCC56C9B1d0FC442DDB3E0a1FD233F8d12BD324");
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(),
               "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
}

TEST_CASE(parse_sha256_empty_string_rejected)
{
    auto r = parse_sha256_hex("");
    REQUIRE(!r.ok());
}

TEST_CASE(parse_sha256_63_chars_rejected)
{
    auto r = parse_sha256_hex(
        "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD32");
    REQUIRE(!r.ok());
}

TEST_CASE(parse_sha256_65_chars_rejected)
{
    auto r = parse_sha256_hex(
        "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD3240");
    REQUIRE(!r.ok());
}

TEST_CASE(parse_sha256_non_hex_char_rejected)
{
    auto r = parse_sha256_hex(
        "G889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324");
    REQUIRE(!r.ok());
}

TEST_CASE(parse_sha256_with_dashes_rejected)
{
    auto r = parse_sha256_hex(
        "B889A4BA-AE15-4F6D-DD94-327BCC56C9B1-D0FC-442D-DB3E-0A1FD233F8D1");
    REQUIRE(!r.ok());
}

TEST_CASE(parse_sha256_with_spaces_rejected)
{
    auto r = parse_sha256_hex(
        "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324 ");
    REQUIRE(!r.ok());
}

// ---------------------------------------------------------------------------
// 11.3 — status state coverage and precedence
// ---------------------------------------------------------------------------
DriverObservation matching_observation(DriverServiceState state) {
    DriverObservation observation{};
    observation.service_state = state;
    observation.installed_version = "1.2.28.93";
    observation.sys_path = "C:\\Windows\\System32\\drivers\\phdskmnt.sys";
    observation.sys_hash =
        "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9";
    observation.signature_valid = true;
    observation.signature_trust_status = 0;
    observation.architecture = DriverArchitecture::x64;
    return observation;
}

TEST_CASE(status_enum_has_all_required_values)
{
    REQUIRE(is_valid(DriverStatus::absent));
    REQUIRE(is_valid(DriverStatus::stopped));
    REQUIRE(is_valid(DriverStatus::running_compatible));
    REQUIRE(is_valid(DriverStatus::version_mismatch));
    REQUIRE(is_valid(DriverStatus::sys_hash_mismatch));
    REQUIRE(is_valid(DriverStatus::signature_mismatch));
    REQUIRE(is_valid(DriverStatus::architecture_mismatch));
    REQUIRE(is_valid(DriverStatus::inspection_failure));
    REQUIRE(is_valid(DriverStatus::busy));
}

TEST_CASE(status_to_string_is_deterministic)
{
    REQUIRE_EQ(to_string(DriverStatus::absent), "absent");
    REQUIRE_EQ(to_string(DriverStatus::stopped), "stopped");
    REQUIRE_EQ(to_string(DriverStatus::running_compatible), "running_compatible");
    REQUIRE_EQ(to_string(DriverStatus::version_mismatch), "version_mismatch");
    REQUIRE_EQ(to_string(DriverStatus::sys_hash_mismatch), "sys_hash_mismatch");
    REQUIRE_EQ(to_string(DriverStatus::signature_mismatch), "signature_mismatch");
    REQUIRE_EQ(to_string(DriverStatus::architecture_mismatch), "architecture_mismatch");
    REQUIRE_EQ(to_string(DriverStatus::inspection_failure), "inspection_failure");
    REQUIRE_EQ(to_string(DriverStatus::busy), "busy");
}

TEST_CASE(status_classifier_covers_absent_query_failure_running_and_stopped) {
    DriverObservation observation{};
    observation.service_state = DriverServiceState::absent;
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::absent);

    observation.service_state = DriverServiceState::query_failed;
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);

    REQUIRE_EQ(classify_driver_status(
                   driver_manifest(),
                   matching_observation(DriverServiceState::running)).status,
               DriverStatus::running_compatible);
    REQUIRE_EQ(classify_driver_status(
                   driver_manifest(),
                   matching_observation(DriverServiceState::stopped)).status,
               DriverStatus::stopped);
}

TEST_CASE(status_classifier_has_explicit_mismatch_precedence) {
    auto observation = matching_observation(DriverServiceState::transitional);
    observation.architecture = DriverArchitecture::x86;
    observation.signature_valid = false;
    observation.sys_hash = std::string(64, '0');
    observation.installed_version = "9.9.9.9";
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::architecture_mismatch);

    observation.architecture = DriverArchitecture::x64;
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::signature_mismatch);

    observation.signature_valid = true;
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::sys_hash_mismatch);

    observation.sys_hash = std::string(driver_manifest().x64_sys_sha256);
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::version_mismatch);

    observation.installed_version = std::string(driver_manifest().driver_version);
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::busy);
}

TEST_CASE(status_classifier_fails_closed_when_installed_evidence_is_missing) {
    auto observation = matching_observation(DriverServiceState::running);
    observation.sys_hash.reset();
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);

    observation = matching_observation(DriverServiceState::running);
    observation.signature_valid.reset();
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);

    observation = matching_observation(DriverServiceState::running);
    observation.installed_version.reset();
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);

    observation = matching_observation(DriverServiceState::running);
    observation.architecture.reset();
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);

    observation = matching_observation(DriverServiceState::running);
    observation.signature_trust_status.reset();
    REQUIRE_EQ(classify_driver_status(driver_manifest(), observation).status,
               DriverStatus::inspection_failure);
}

TEST_CASE(status_classifier_compares_hash_case_insensitively_and_preserves_path) {
    auto observation = matching_observation(DriverServiceState::running);
    for (auto& character : *observation.sys_hash) {
        if (character >= 'A' && character <= 'F') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    const auto result = classify_driver_status(driver_manifest(), observation);
    REQUIRE_EQ(result.status, DriverStatus::running_compatible);
    REQUIRE_EQ(result.sys_path, observation.sys_path);
    REQUIRE_EQ(result.signature_trust_status,
               observation.signature_trust_status);
}

// ---------------------------------------------------------------------------
// 11.5 — JSON output with default path redaction
// ---------------------------------------------------------------------------
TEST_CASE(json_redacted_by_default_no_sys_path)
{
    DriverStatusResult r;
    r.status = DriverStatus::running_compatible;
    r.installed_version = "1.2.28.93";
    r.sys_path = "C:\\Windows\\System32\\drivers\\phdskmnt.sys";
    r.sys_hash = "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9";
    r.signature_valid = true;
    r.signature_trust_status = 0;
    r.architecture = "x64";

    auto json = driver_status_to_json(r, false);

    bool has_path = false;
    for (const auto& f : json) {
        if (f.name == "sys_path") has_path = true;
    }
    REQUIRE(!has_path);
    for (const auto& field : json) {
        if (const auto* text = std::get_if<std::string>(&field.value)) {
            REQUIRE(text->find("C:\\Windows\\System32") == std::string::npos);
        }
    }
}

TEST_CASE(json_includes_paths_when_requested)
{
    DriverStatusResult r;
    r.status = DriverStatus::running_compatible;
    r.installed_version = "1.2.28.93";
    r.sys_path = "C:\\Windows\\System32\\drivers\\phdskmnt.sys";
    r.sys_hash = "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9";
    r.signature_valid = true;
    r.signature_trust_status = 0;
    r.architecture = "x64";

    auto json = driver_status_to_json(r, true);

    bool has_path = false;
    for (const auto& f : json) {
        if (f.name == "sys_path" && std::holds_alternative<std::string>(f.value)) {
            has_path = true;
        }
    }
    REQUIRE(has_path);
}

TEST_CASE(json_deterministic_field_order)
{
    DriverStatusResult r;
    r.status = DriverStatus::running_compatible;
    r.installed_version = "1.2.28.93";
    r.sys_path = "C:\\Windows\\System32\\drivers\\phdskmnt.sys";
    r.sys_hash = "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9";
    r.signature_valid = true;
    r.architecture = "x64";

    auto json1 = driver_status_to_json(r, true);
    auto json2 = driver_status_to_json(r, true);

    REQUIRE(json1.size() == json2.size());
    for (size_t i = 0; i < json1.size(); ++i) {
        REQUIRE_EQ(json1[i].name, json2[i].name);
    }
}

TEST_CASE(json_absent_status_minimal_fields)
{
    DriverStatusResult r;
    r.status = DriverStatus::absent;

    auto json = driver_status_to_json(r, false);

    bool has_status = false;
    for (const auto& f : json) {
        if (f.name == "status") has_status = true;
    }
    REQUIRE(has_status);
}

TEST_CASE(json_null_optionals_omitted)
{
    DriverStatusResult r;
    r.status = DriverStatus::absent;

    auto json = driver_status_to_json(r, false);

    for (const auto& f : json) {
        REQUIRE(f.name != "installed_version");
        REQUIRE(f.name != "sys_hash");
        REQUIRE(f.name != "signature_valid");
        REQUIRE(f.name != "signature_trust_status");
        REQUIRE(f.name != "architecture");
    }
}

TEST_CASE(json_exposes_numeric_signature_trust_metadata_without_path) {
    auto result = classify_driver_status(
        driver_manifest(), matching_observation(DriverServiceState::running));
    auto json = driver_status_to_json(result, false);
    bool found_trust = false;
    for (const auto& field : json) {
        if (field.name == "signature_trust_status") {
            const auto* value = std::get_if<std::uint64_t>(&field.value);
            REQUIRE(value != nullptr);
            REQUIRE_EQ(*value, std::uint64_t{0});
            found_trust = true;
        }
        REQUIRE(field.name != "sys_path");
    }
    REQUIRE(found_trust);
}
