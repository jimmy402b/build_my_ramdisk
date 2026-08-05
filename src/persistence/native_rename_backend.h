#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <span>

namespace myrd::persistence::detail {

inline constexpr LONG kNativeStatusPending = 0x00000103L;

// Layout-compatible with IO_STATUS_BLOCK. The backend owns only the native
// calls; the rename operation owns this storage until completion is drained.
struct NativeRenameIoStatus final {
    union {
        LONG status;
        void* pointer;
    };
    ULONG_PTR information;
};

class INativeRenameBackend {
public:
    virtual ~INativeRenameBackend() = default;

    [[nodiscard]] virtual LONG begin(
        HANDLE file,
        NativeRenameIoStatus* io_status,
        void* information,
        ULONG information_size) = 0;
    [[nodiscard]] virtual DWORD wait(
        std::span<const HANDLE> handles,
        DWORD timeout_ms) = 0;
    virtual void cancel(HANDLE file) noexcept = 0;
    [[nodiscard]] virtual ULONG status_to_dos(LONG status) noexcept = 0;
    [[nodiscard]] virtual bool flush(
        HANDLE file,
        DWORD& native_error) noexcept = 0;
};

} // namespace myrd::persistence::detail
