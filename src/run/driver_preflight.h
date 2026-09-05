#pragma once

#include "src/common/result.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <functional>

namespace myrd {
enum class driver_preflight_result { available, unavailable };
struct ScmServiceProbe final {
    std::function<SC_HANDLE()> open_manager;
    std::function<SC_HANDLE(SC_HANDLE)> open_service;
    std::function<bool(SC_HANDLE, DWORD&)> query_start_type;
    std::function<DWORD()> last_error;
    std::function<void(SC_HANDLE)> close_handle;
};
driver_preflight_result classify_phdskmnt_service(bool found, DWORD start_type) noexcept;
Result<void> require_phdskmnt_service_available(const ScmServiceProbe&);
Result<void> require_phdskmnt_service_available();
}
