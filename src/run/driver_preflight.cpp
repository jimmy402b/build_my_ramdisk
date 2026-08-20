#include "driver_preflight.h"

#include <vector>

namespace myrd {
namespace {

constexpr wchar_t kServiceName[] = L"phdskmnt";

Error driver_unavailable_error() {
    return Error{ErrorDomain::usage, 1, L"driver_unavailable",
                 L"phdskmnt is disabled or absent"};
}

Result<void> classified(bool service_found, DWORD start_type) {
    return classify_phdskmnt_service(service_found, start_type) ==
                   driver_preflight_result::available
               ? Result<void>::success()
               : Result<void>::failure(driver_unavailable_error());
}

bool read_start_type(SC_HANDLE service, DWORD& start_type) {
    DWORD needed = 0;
    if (QueryServiceConfigW(service, nullptr, 0, &needed)) {
        // A zero-byte buffer cannot succeed; treat it as a protocol failure
        // rather than reporting a stale GetLastError to the caller.
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    if (needed < sizeof(QUERY_SERVICE_CONFIGW)) needed = sizeof(QUERY_SERVICE_CONFIGW);
    std::vector<unsigned char> buffer(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
    DWORD written = 0;
    if (!QueryServiceConfigW(service, config, needed, &written)) return false;
    start_type = config->dwStartType;
    return true;
}

} // namespace

driver_preflight_result classify_phdskmnt_service(bool service_found,
                                                  DWORD start_type) noexcept {
    // Exactly two known results are unavailable: the service is absent, or it
    // is configured DISABLED.  Every other start type — including a value
    // Windows does not currently define — stays available so that the single
    // adapter discovery authority remains the one that rejects it.  Widening
    // this set would let `driver_unavailable` stand for a third, unknown
    // condition.
    if (!service_found) return driver_preflight_result::unavailable;
    if (start_type == SERVICE_DISABLED) return driver_preflight_result::unavailable;
    return driver_preflight_result::available;
}

Result<void> require_phdskmnt_service_available(const ScmServiceProbe& probe) {
    const SC_HANDLE manager = probe.open_manager();
    if (manager == nullptr) {
        return Result<void>::failure({ErrorDomain::win32, probe.last_error(),
            L"OpenSCManagerW", L"service control manager could not be opened"});
    }
    const SC_HANDLE service = probe.open_service(manager);
    if (service == nullptr) {
        // Read the error before closing: CloseServiceHandle overwrites it.
        const DWORD error = probe.last_error();
        probe.close_handle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) return classified(false, 0);
        return Result<void>::failure({ErrorDomain::win32, error, L"OpenServiceW",
            L"phdskmnt service could not be opened"});
    }
    DWORD start_type = 0;
    const bool queried = probe.query_start_type(service, start_type);
    const DWORD query_error = queried ? ERROR_SUCCESS : probe.last_error();
    probe.close_handle(service);
    probe.close_handle(manager);
    if (!queried) {
        return Result<void>::failure({ErrorDomain::win32, query_error,
            L"QueryServiceConfigW",
            L"phdskmnt service configuration could not be read"});
    }
    return classified(true, start_type);
}

Result<void> require_phdskmnt_service_available() {
    ScmServiceProbe probe{};
    probe.open_manager = [] {
        return OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    };
    probe.open_service = [](SC_HANDLE manager) {
        return OpenServiceW(manager, kServiceName, SERVICE_QUERY_CONFIG);
    };
    probe.query_start_type = read_start_type;
    probe.last_error = [] { return GetLastError(); };
    probe.close_handle = [](SC_HANDLE handle) { CloseServiceHandle(handle); };
    return require_phdskmnt_service_available(probe);
}

} // namespace myrd
