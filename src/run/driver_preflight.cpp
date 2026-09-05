#include "src/run/driver_preflight.h"

#include <vector>

namespace myrd {
namespace {
constexpr wchar_t kServiceName[] = L"phdskmnt";
Result<void> unavailable() {
    return Result<void>::failure({ErrorDomain::usage, 1, L"driver_unavailable",
                                  L"phdskmnt is disabled or absent"});
}
bool query_config(SC_HANDLE service, DWORD& type) {
    DWORD needed{};
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> bytes(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(bytes.data());
    DWORD written{};
    if (!QueryServiceConfigW(service, config, needed, &written)) return false;
    type = config->dwStartType;
    return true;
}
}
driver_preflight_result classify_phdskmnt_service(bool found, DWORD type) noexcept {
    return !found || type == SERVICE_DISABLED ? driver_preflight_result::unavailable
                                               : driver_preflight_result::available;
}
Result<void> require_phdskmnt_service_available(const ScmServiceProbe& probe) {
    const auto manager = probe.open_manager();
    if (!manager) return Result<void>::failure({ErrorDomain::win32, probe.last_error(), L"OpenSCManagerW", L"service control manager could not be opened"});
    const auto service = probe.open_service(manager);
    if (!service) {
        const auto error = probe.last_error(); probe.close_handle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? unavailable() : Result<void>::failure({ErrorDomain::win32, error, L"OpenServiceW", L"phdskmnt service could not be opened"});
    }
    DWORD type{}; const bool ok = probe.query_start_type(service, type); const auto error = ok ? ERROR_SUCCESS : probe.last_error();
    probe.close_handle(service); probe.close_handle(manager);
    if (!ok) return Result<void>::failure({ErrorDomain::win32, error, L"QueryServiceConfigW", L"phdskmnt service configuration could not be read"});
    return classify_phdskmnt_service(true, type) == driver_preflight_result::available ? Result<void>::success() : unavailable();
}
Result<void> require_phdskmnt_service_available() {
    ScmServiceProbe probe{
        [] { return OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); },
        [](SC_HANDLE manager) { return OpenServiceW(manager, kServiceName, SERVICE_QUERY_CONFIG); },
        query_config,
        [] { return GetLastError(); },
        [](SC_HANDLE handle) { CloseServiceHandle(handle); }};
    return require_phdskmnt_service_available(probe);
}
}
