#include "m0_provider.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd::poc {

M0Provider::M0Provider() {
    auto id = proxy::generate_session_id();
    if (!id) return;
    auto names = proxy::ObjectNames::from_session(id.value());
    if (!names) return;
    names_ = names.value();
}

Result<void> M0Provider::start() {
    if (names_.mapping.empty())
        return Result<void>::failure({ErrorDomain::internal, 0,
            L"M0Provider::start", L"no valid session"});

    auto ch = proxy::SharedMemoryChannel::create(names_);
    if (!ch) return Result<void>::failure(ch.error());

    // Move channel into optional for ownership
    channel_.emplace(std::move(ch.value()));

    return Result<void>::success();
}

Result<void> M0Provider::stop_and_release() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    channel_.reset();
    return Result<void>::success();
}

std::wstring M0Provider::native_object_path() const {
    return names_.driver_path;
}

proxy::RequestCounters M0Provider::counters() const {
    return processor_.counters();
}

Result<bool> M0Provider::named_objects_released() const {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, names_.mapping.c_str());
    if (h) { CloseHandle(h); return Result<bool>::success(false); }
    if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return Result<bool>::success(false);
    return Result<bool>::success(true);
}

} // namespace myrd::poc
