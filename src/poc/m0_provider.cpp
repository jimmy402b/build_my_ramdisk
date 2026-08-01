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

    server_.emplace(std::move(ch.value()), processor_);
    auto started = server_->start();
    if (!started) {
        server_.reset();
        return started;
    }

    return Result<void>::success();
}

Result<void> M0Provider::stop_and_release() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    return Result<void>::success();
}

std::wstring M0Provider::native_object_path() const {
    return names_.driver_path;
}

proxy::RequestCounters M0Provider::counters() const {
    return processor_.counters();
}

Result<bool> M0Provider::named_objects_released() const {
    const auto probe = [](HANDLE handle,
                          const wchar_t* operation) -> Result<bool> {
        const DWORD error = handle == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (handle != nullptr) {
            CloseHandle(handle);
            return Result<bool>::success(false);
        }
        if (error == ERROR_FILE_NOT_FOUND) {
            return Result<bool>::success(true);
        }
        return Result<bool>::failure({
            ErrorDomain::win32,
            static_cast<std::uint32_t>(error),
            operation,
            L"named-object absence probe failed"});
    };

    auto mapping = probe(OpenFileMappingW(
        FILE_MAP_READ, FALSE, names_.mapping.c_str()), L"OpenFileMappingW");
    if (!mapping || !mapping.value()) return mapping;

    auto request = probe(OpenEventW(
        SYNCHRONIZE, FALSE, names_.request.c_str()), L"OpenEventW(Request)");
    if (!request || !request.value()) return request;

    auto response = probe(OpenEventW(
        SYNCHRONIZE, FALSE, names_.response.c_str()), L"OpenEventW(Response)");
    if (!response || !response.value()) return response;

    auto server = probe(OpenMutexW(
        SYNCHRONIZE, FALSE, names_.server.c_str()), L"OpenMutexW(Server)");
    if (!server || !server.value()) return server;

    return Result<bool>::success(true);
}

} // namespace myrd::poc
