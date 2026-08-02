#include "shared_memory_channel.h"

#include "src/common/unique_local.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <cstring>

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
    const ObjectNames& names, ChannelCreateHook hook) {
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

    // Stage 0: owner mutex
    if (hook) {
        auto hr = hook(ChannelCreateStage::owner_mutex);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
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

    // Stage 1: file mapping
    if (hook) {
        auto hr = hook(ChannelCreateStage::mapping);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
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

    // Stage 2: map view
    if (hook) {
        auto hr = hook(ChannelCreateStage::map_view);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
    void* view = MapViewOfFile(ch.mapping_handle_.get(),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingSize);
    if (!view) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"MapViewOfFile", L"failed to map view"});
    }
    ch.mapping_view_ = UniqueView(view);
    std::memset(view, 0, kMappingSize);

    // Stage 3: request event (auto-reset)
    if (hook) {
        auto hr = hook(ChannelCreateStage::request_event);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
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

    // Stage 4: response event (auto-reset)
    if (hook) {
        auto hr = hook(ChannelCreateStage::response_event);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
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

    // Stage 5: cancel event (manual-reset)
    if (hook) {
        auto hr = hook(ChannelCreateStage::cancel_event);
        if (!hr) return Result<SharedMemoryChannel>::failure(hr.error());
    }
    ch.cancel_event_ = UniqueHandle(
        CreateEventW(&sa, TRUE, FALSE, names.cancel.c_str()));
    if (!ch.cancel_event_) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateEventW(cancel)", L"failed"});
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return Result<SharedMemoryChannel>::failure(
            {ErrorDomain::protocol, ERROR_ALREADY_EXISTS,
             L"CreateEventW(cancel)", L"event already exists"});
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

HANDLE SharedMemoryChannel::cancel_event() const noexcept {
    return cancel_event_.get();
}

const ObjectNames& SharedMemoryChannel::names() const noexcept {
    return names_;
}

} // namespace myrd::proxy
