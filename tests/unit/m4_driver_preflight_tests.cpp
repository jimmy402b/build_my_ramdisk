#include "test_framework.h"
#include "src/run/driver_preflight.h"
#include "src/run/driver_preflight.cpp"

#include <string>
#include <vector>

using namespace myrd;

namespace {

SC_HANDLE manager_handle() { return reinterpret_cast<SC_HANDLE>(0x10); }
SC_HANDLE service_handle() { return reinterpret_cast<SC_HANDLE>(0x20); }

// Fully in-memory service control manager.  No test asserts against the real
// phdskmnt service, so the matrix is independent of this machine's driver
// installation, privileges and start type.
struct FakeScm {
    bool manager_ok = true;
    bool service_ok = true;
    bool query_ok = true;
    DWORD start_type = SERVICE_DEMAND_START;
    DWORD error = ERROR_SUCCESS;
    std::vector<SC_HANDLE> closed;
    int manager_calls = 0;
    int service_calls = 0;
    int query_calls = 0;

    ScmServiceProbe probe() {
        ScmServiceProbe seam{};
        seam.open_manager = [this] {
            ++manager_calls;
            return manager_ok ? manager_handle() : nullptr;
        };
        seam.open_service = [this](SC_HANDLE) {
            ++service_calls;
            return service_ok ? service_handle() : nullptr;
        };
        seam.query_start_type = [this](SC_HANDLE, DWORD& value) {
            ++query_calls;
            if (!query_ok) return false;
            value = start_type;
            return true;
        };
        seam.last_error = [this] { return error; };
        seam.close_handle = [this](SC_HANDLE handle) { closed.push_back(handle); };
        return seam;
    }
};

// print_run_error renders exactly "<operation>: <message>" after the bundle
// path, so this is the text a user would see on stderr.
std::wstring rendered(const Error& error) {
    return error.operation + L": " + error.message;
}

bool mentions_driver_unavailable(const Error& error) {
    return rendered(error).find(L"driver_unavailable") != std::wstring::npos;
}

} // namespace

TEST_CASE(m4_driver_preflight_absent_service_is_unavailable) {
    REQUIRE(classify_phdskmnt_service(false, SERVICE_DEMAND_START) ==
            driver_preflight_result::unavailable);
    REQUIRE(classify_phdskmnt_service(false, SERVICE_AUTO_START) ==
            driver_preflight_result::unavailable);
    REQUIRE(classify_phdskmnt_service(false, SERVICE_DISABLED) ==
            driver_preflight_result::unavailable);
}

TEST_CASE(m4_driver_preflight_disabled_service_is_unavailable) {
    REQUIRE(classify_phdskmnt_service(true, SERVICE_DISABLED) ==
            driver_preflight_result::unavailable);
}

TEST_CASE(m4_driver_preflight_every_enabled_start_type_is_available) {
    REQUIRE(classify_phdskmnt_service(true, SERVICE_BOOT_START) ==
            driver_preflight_result::available);
    REQUIRE(classify_phdskmnt_service(true, SERVICE_SYSTEM_START) ==
            driver_preflight_result::available);
    REQUIRE(classify_phdskmnt_service(true, SERVICE_AUTO_START) ==
            driver_preflight_result::available);
    REQUIRE(classify_phdskmnt_service(true, SERVICE_DEMAND_START) ==
            driver_preflight_result::available);
}

TEST_CASE(m4_driver_preflight_seam_absent_service_uses_the_frozen_protocol) {
    FakeScm scm{};
    scm.service_ok = false;
    scm.error = ERROR_SERVICE_DOES_NOT_EXIST;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE(!result.ok());
    REQUIRE(result.error().domain == ErrorDomain::usage);
    REQUIRE_EQ(result.error().code, std::uint32_t{1});
    REQUIRE_EQ(result.error().operation, std::wstring(L"driver_unavailable"));
    REQUIRE_EQ(result.error().message,
               std::wstring(L"phdskmnt is disabled or absent"));
}

TEST_CASE(m4_driver_preflight_seam_disabled_service_uses_the_frozen_protocol) {
    FakeScm scm{};
    scm.start_type = SERVICE_DISABLED;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE(!result.ok());
    REQUIRE(result.error().domain == ErrorDomain::usage);
    REQUIRE_EQ(result.error().code, std::uint32_t{1});
    REQUIRE_EQ(result.error().operation, std::wstring(L"driver_unavailable"));
    REQUIRE_EQ(result.error().message,
               std::wstring(L"phdskmnt is disabled or absent"));
}

TEST_CASE(m4_driver_preflight_seam_enabled_service_passes) {
    for (const DWORD start_type : {SERVICE_BOOT_START, SERVICE_SYSTEM_START,
                                   SERVICE_AUTO_START, SERVICE_DEMAND_START}) {
        FakeScm scm{};
        scm.start_type = start_type;
        REQUIRE(require_phdskmnt_service_available(scm.probe()).ok());
    }
}

TEST_CASE(m4_driver_preflight_open_manager_failure_is_a_win32_error) {
    FakeScm scm{};
    scm.manager_ok = false;
    scm.error = ERROR_ACCESS_DENIED;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE(!result.ok());
    REQUIRE(result.error().domain == ErrorDomain::win32);
    REQUIRE_EQ(result.error().code, std::uint32_t{ERROR_ACCESS_DENIED});
    REQUIRE_EQ(result.error().operation, std::wstring(L"OpenSCManagerW"));
    REQUIRE(!mentions_driver_unavailable(result.error()));
    REQUIRE_EQ(scm.service_calls, 0);
    REQUIRE_EQ(scm.query_calls, 0);
}

TEST_CASE(m4_driver_preflight_open_service_access_denied_is_a_win32_error) {
    FakeScm scm{};
    scm.service_ok = false;
    scm.error = ERROR_ACCESS_DENIED;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE(!result.ok());
    REQUIRE(result.error().domain == ErrorDomain::win32);
    REQUIRE_EQ(result.error().code, std::uint32_t{ERROR_ACCESS_DENIED});
    REQUIRE_EQ(result.error().operation, std::wstring(L"OpenServiceW"));
    REQUIRE(!mentions_driver_unavailable(result.error()));
    REQUIRE_EQ(scm.query_calls, 0);
}

TEST_CASE(m4_driver_preflight_query_config_failure_is_a_win32_error) {
    FakeScm scm{};
    scm.query_ok = false;
    scm.error = ERROR_INVALID_HANDLE;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE(!result.ok());
    REQUIRE(result.error().domain == ErrorDomain::win32);
    REQUIRE_EQ(result.error().code, std::uint32_t{ERROR_INVALID_HANDLE});
    REQUIRE_EQ(result.error().operation, std::wstring(L"QueryServiceConfigW"));
    REQUIRE(!mentions_driver_unavailable(result.error()));
}

// An undefined start type must not be reported as the frozen
// `driver_unavailable` protocol: that protocol stands for exactly two known
// results, and widening it would let it represent a third, unknown condition.
TEST_CASE(m4_driver_preflight_undefined_start_type_is_left_to_adapter_discovery) {
    REQUIRE(classify_phdskmnt_service(true, 0x7fffffff) ==
            driver_preflight_result::available);
    FakeScm scm{};
    scm.start_type = 0x7fffffff;
    REQUIRE(require_phdskmnt_service_available(scm.probe()).ok());
}

TEST_CASE(m4_driver_preflight_closes_every_handle_it_opened) {
    FakeScm available{};
    REQUIRE(require_phdskmnt_service_available(available.probe()).ok());
    REQUIRE_EQ(available.closed,
               std::vector<SC_HANDLE>({service_handle(), manager_handle()}));

    FakeScm disabled{};
    disabled.start_type = SERVICE_DISABLED;
    REQUIRE(!require_phdskmnt_service_available(disabled.probe()).ok());
    REQUIRE_EQ(disabled.closed,
               std::vector<SC_HANDLE>({service_handle(), manager_handle()}));

    FakeScm query_failed{};
    query_failed.query_ok = false;
    query_failed.error = ERROR_INVALID_HANDLE;
    REQUIRE(!require_phdskmnt_service_available(query_failed.probe()).ok());
    REQUIRE_EQ(query_failed.closed,
               std::vector<SC_HANDLE>({service_handle(), manager_handle()}));

    FakeScm absent{};
    absent.service_ok = false;
    absent.error = ERROR_SERVICE_DOES_NOT_EXIST;
    REQUIRE(!require_phdskmnt_service_available(absent.probe()).ok());
    REQUIRE_EQ(absent.closed, std::vector<SC_HANDLE>({manager_handle()}));

    FakeScm no_manager{};
    no_manager.manager_ok = false;
    no_manager.error = ERROR_ACCESS_DENIED;
    REQUIRE(!require_phdskmnt_service_available(no_manager.probe()).ok());
    REQUIRE(no_manager.closed.empty());
}

// The error must be read before the handle is closed, because
// CloseServiceHandle overwrites the thread's last-error value.
TEST_CASE(m4_driver_preflight_reads_the_open_error_before_closing_the_manager) {
    FakeScm scm{};
    scm.service_ok = false;
    scm.error = ERROR_ACCESS_DENIED;
    auto seam = scm.probe();
    seam.close_handle = [&scm](SC_HANDLE handle) {
        scm.closed.push_back(handle);
        scm.error = ERROR_SUCCESS;
    };
    const auto result = require_phdskmnt_service_available(seam);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code, std::uint32_t{ERROR_ACCESS_DENIED});
    REQUIRE_EQ(result.error().operation, std::wstring(L"OpenServiceW"));
}

TEST_CASE(m4_driver_preflight_stops_at_the_first_failing_scm_call) {
    // A manager failure must not be masked by a later service or query result.
    FakeScm scm{};
    scm.manager_ok = false;
    scm.service_ok = false;
    scm.query_ok = false;
    scm.error = ERROR_ACCESS_DENIED;
    const auto result = require_phdskmnt_service_available(scm.probe());
    REQUIRE_EQ(result.error().operation, std::wstring(L"OpenSCManagerW"));
    REQUIRE_EQ(scm.manager_calls, 1);
    REQUIRE_EQ(scm.service_calls, 0);
    REQUIRE_EQ(scm.query_calls, 0);
}
