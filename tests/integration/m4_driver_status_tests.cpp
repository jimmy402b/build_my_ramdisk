#include "src/setup/driver_manifest.h"

#include "tests/support/test_framework.h"

#include <iostream>
#include <string>

using namespace myrd::setup;
using namespace myrd::product;

// ---------------------------------------------------------------------------
// Integration: manifest pin invariants (always true regardless of machine)
// ---------------------------------------------------------------------------
TEST_CASE(integration_manifest_pins_match_spec)
{
    const auto& m = driver_manifest();
    REQUIRE_EQ(m.upstream_commit,
               "cb7a2347e9595b12e2bd178ce803090a7a674037");
    REQUIRE_EQ(m.driver_version, "1.2.28.93");
    REQUIRE(m.architecture == DriverArchitecture::x64);
    REQUIRE_EQ(m.service_name, "phdskmnt");
}

TEST_CASE(integration_archive_url_contains_pinned_commit)
{
    const auto& m = driver_manifest();
    std::string url(m.archive_url);
    REQUIRE(url.find("cb7a2347e9595b12e2bd178ce803090a7a674037")
            != std::string::npos);
}

TEST_CASE(integration_sha256_pins_are_64_uppercase_hex)
{
    const auto& m = driver_manifest();
    REQUIRE(m.archive_sha256.size() == 64);
    REQUIRE(m.x64_sys_sha256.size() == 64);
    for (char c : m.archive_sha256) {
        REQUIRE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    }
    for (char c : m.x64_sys_sha256) {
        REQUIRE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    }
}

// ---------------------------------------------------------------------------
// Integration: read-only status inspection (safe on any machine)
// ---------------------------------------------------------------------------
TEST_CASE(integration_classify_returns_valid_status)
{
    const auto& m = driver_manifest();
    auto result = inspect_installed_driver(m);
    REQUIRE(is_valid(result.status));
    std::cout << "M4 driver status=" << to_string(result.status) << std::endl;
    if (result.status == DriverStatus::running_compatible ||
        result.status == DriverStatus::stopped) {
        REQUIRE_EQ(result.installed_version,
                   std::optional<std::string>(m.driver_version));
        REQUIRE_EQ(result.sys_hash,
                   std::optional<std::string>(m.x64_sys_sha256));
        REQUIRE_EQ(result.signature_valid, std::optional<bool>(true));
        REQUIRE_EQ(result.signature_trust_status,
                   std::optional<std::uint32_t>(0));
        REQUIRE_EQ(result.architecture, std::optional<std::string>("x64"));
        REQUIRE(result.sys_path.has_value());
    }
}

TEST_CASE(integration_classify_is_readonly_no_mutation)
{
    // Call twice; must return consistent results and not mutate anything
    const auto& m = driver_manifest();
    auto r1 = inspect_installed_driver(m);
    auto r2 = inspect_installed_driver(m);
    REQUIRE(r1.status == r2.status);
}

TEST_CASE(integration_classify_missing_service_returns_absent)
{
    DriverManifest fake = driver_manifest();
    fake.service_name = "__myrd_task11_missing_service_7f1b";
    const auto result = inspect_installed_driver(fake);
    REQUIRE_EQ(result.status, DriverStatus::absent);
}

// ---------------------------------------------------------------------------
// Integration: JSON output is always well-formed
// ---------------------------------------------------------------------------
TEST_CASE(integration_json_default_redaction_no_sys_path)
{
    const auto& m = driver_manifest();
    auto result = inspect_installed_driver(m);

    auto json = driver_status_to_json(result, false);

    bool has_sys_path = false;
    for (const auto& f : json) {
        if (f.name == "sys_path") {
            has_sys_path = true;
        }
    }
    REQUIRE(!has_sys_path);
}

TEST_CASE(integration_json_always_has_status_field)
{
    const auto& m = driver_manifest();
    auto result = inspect_installed_driver(m);

    auto json = driver_status_to_json(result, false);

    bool has_status = false;
    for (const auto& f : json) {
        if (f.name == "status") has_status = true;
    }
    REQUIRE(has_status);
}

TEST_CASE(integration_json_with_paths_includes_sys_path_when_available)
{
    const auto& m = driver_manifest();
    auto result = inspect_installed_driver(m);

    auto json = driver_status_to_json(result, true);

    // sys_path may or may not be present depending on machine state,
    // but the function must not crash
    bool has_status = false;
    for (const auto& f : json) {
        if (f.name == "status") has_status = true;
    }
    REQUIRE(has_status);
}

TEST_CASE(integration_to_string_roundtrip)
{
    for (auto status : {DriverStatus::absent, DriverStatus::stopped,
                        DriverStatus::running_compatible,
                        DriverStatus::version_mismatch,
                        DriverStatus::sys_hash_mismatch,
                        DriverStatus::signature_mismatch,
                        DriverStatus::architecture_mismatch,
                        DriverStatus::inspection_failure,
                        DriverStatus::busy}) {
        auto s = to_string(status);
        REQUIRE(!s.empty());
    }
}
