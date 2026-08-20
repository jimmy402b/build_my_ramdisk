#pragma once

#include "src/common/result.h"

#include <windows.h>

#include <functional>

namespace myrd {

// Result of the cheap `phdskmnt` service configuration check that runs before
// any bundle, AIM or target-drive work.  It answers exactly one question: is
// the service known to be absent or configured DISABLED?  It never stands in
// for adapter discovery, which remains the sole AIM authority.
enum class driver_preflight_result { available, unavailable };

// Replaceable SCM I/O seam.  Each member maps one-to-one onto a single Win32
// service-control call so the caller can report the exact failing operation
// instead of collapsing every SCM failure into one error.
struct ScmServiceProbe {
    // OpenSCManagerW; returns nullptr on failure.
    std::function<SC_HANDLE()> open_manager;
    // OpenServiceW for phdskmnt; returns nullptr on failure.
    std::function<SC_HANDLE(SC_HANDLE)> open_service;
    // QueryServiceConfigW; writes the start type and returns false on failure.
    std::function<bool(SC_HANDLE, DWORD&)> query_start_type;
    // GetLastError for the most recent seam call.
    std::function<DWORD()> last_error;
    // CloseServiceHandle.
    std::function<void(SC_HANDLE)> close_handle;
};

// Pure classification.  Only "service absent" and SERVICE_DISABLED are
// unavailable; SERVICE_BOOT_START, SERVICE_SYSTEM_START, SERVICE_AUTO_START
// and SERVICE_DEMAND_START are available.
driver_preflight_result classify_phdskmnt_service(bool service_found,
                                                  DWORD start_type) noexcept;

// Runs the classification over the supplied seam.  Only
// ERROR_SERVICE_DOES_NOT_EXIST counts as "service absent"; every other SCM
// failure becomes an ErrorDomain::win32 error carrying the raw Win32 code and
// the exact operation name, never the `driver_unavailable` protocol.
Result<void> require_phdskmnt_service_available(const ScmServiceProbe& probe);

// Same check over the real service control manager.
Result<void> require_phdskmnt_service_available();

} // namespace myrd
