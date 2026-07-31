#include "shared_memory_channel.h"

#include "src/common/unique_local.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

namespace myrd::proxy {

namespace {

UniqueLocal make_security_attributes(SECURITY_ATTRIBUTES& sa) {
    UniqueLocal sd;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
        return {};
    }
    sd = UniqueLocal(sa.lpSecurityDescriptor);
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return sd;
}

} // anonymous namespace

Result<SharedMemoryChannel> SharedMemoryChannel::create(
    const ObjectNames& names) {
    SharedMemoryChannel ch;
    ch.names_ = names;

    SECURITY_ATTRIBUTES sa{};
    auto sd_owner = make_security_attributes(sa);
    if (!sd_owner) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"ConvertStringSecurityDescriptorToSecurityDescriptorW",
             L"failed to create security descriptor"});
    }

    // 1. Create server mutex (reject duplicate)
    ch.server_mutex_ = UniqueHandle(
        CreateMutexW(&sa, FALSE, names.server.c_str()));
    if (!ch.server_mutex_) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateMutexW", L"failed to create server mutex"});
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::protocol, ERROR_ALREADY_EXISTS,
             L"CreateMutexW", L"server mutex already exists"});
    }

    // 2. Create file mapping (reject duplicate)
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(kMappingSize);
    ch.mapping_handle_ = UniqueHandle(
        CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
            PAGE_READWRITE, size.HighPart, size.LowPart,
            names.mapping.c_str()));
    if (!ch.mapping_handle_) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateFileMappingW", L"failed to create file mapping"});
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::protocol, ERROR_ALREADY_EXISTS,
             L"CreateFileMappingW", L"mapping already exists"});
    }

    // 3. Map view
    void* view = MapViewOfFile(ch.mapping_handle_.get(),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingSize);
    if (!view) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"MapViewOfFile", L"failed to map view"});
    }
    ch.mapping_view_ = UniqueView(view);

    // 4. Create Request event (auto-reset, reject duplicate)
    ch.request_event_ = UniqueHandle(
        CreateEventW(&sa, FALSE, FALSE, names.request.c_str()));
    if (!ch.request_event_) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateEventW(request)", L"failed"});
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::protocol, ERROR_ALREADY_EXISTS,
             L"CreateEventW(request)", L"event already exists"});
    }

    // 5. Create Response event (auto-reset, reject duplicate)
    ch.response_event_ = UniqueHandle(
        CreateEventW(&sa, FALSE, FALSE, names.response.c_str()));
    if (!ch.response_event_) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateEventW(response)", L"failed"});
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::protocol, ERROR_ALREADY_EXISTS,
             L"CreateEventW(response)", L"event already exists"});
    }

    return Result<SharedMemoryChannel>::success(std::move(ch));
}

SharedMemoryChannel::SharedMemoryChannel(SharedMemoryChannel&&) noexcept
    = default;
SharedMemoryChannel& SharedMemoryChannel::operator=(
    SharedMemoryChannel&&) noexcept = default;
SharedMemoryChannel::~SharedMemoryChannel() noexcept = default;

std::span<std::byte> SharedMemoryChannel::mapping() noexcept {
    return std::span<std::byte>(
        static_cast<std::byte*>(mapping_view_.get()), kMappingSize);
}

HANDLE SharedMemoryChannel::request_event() const noexcept {
    return request_event_.get();
}

HANDLE SharedMemoryChannel::response_event() const noexcept {
    return response_event_.get();
}

const ObjectNames& SharedMemoryChannel::names() const noexcept {
    return names_;
}

} // namespace myrd::proxy
