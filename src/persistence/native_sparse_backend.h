#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <span>

namespace myrd::persistence::detail {

class INativeSparseBackend {
public:
    virtual ~INativeSparseBackend() = default;

    // Returns true for synchronous success. False uses native_error to
    // distinguish ERROR_IO_PENDING from an immediate failure.
    [[nodiscard]] virtual bool begin(
        HANDLE file,
        FILE_SET_SPARSE_BUFFER* input,
        OVERLAPPED* overlapped,
        DWORD& native_error) noexcept = 0;
    [[nodiscard]] virtual DWORD wait(
        std::span<const HANDLE> handles,
        DWORD timeout_ms) noexcept = 0;
    virtual void cancel(HANDLE file, OVERLAPPED* overlapped) noexcept = 0;
    [[nodiscard]] virtual bool complete(
        HANDLE file,
        OVERLAPPED* overlapped,
        DWORD& native_error) noexcept = 0;
};

}  // namespace myrd::persistence::detail
