#include "src/control/pipe_transport.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace myrd::ipc {

namespace {

constexpr auto kMaximumTransportWait = std::chrono::seconds(30);

template <class T>
Result<T> transport_error(std::uint32_t code,
                          const wchar_t* operation,
                          const wchar_t* message,
                          std::uint32_t native_code = 0) {
    return Result<T>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        code, operation, message, native_code});
}

OperationContext bounded_context(const OperationContext& context) {
    const auto maximum_deadline =
        std::chrono::steady_clock::now() + kMaximumTransportWait;
    return OperationContext{
        context.stop_token,
        (std::min)(context.deadline, maximum_deadline)};
}

DWORD deadline_timeout_ms(const OperationContext& context) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= context.deadline) return 0;
    const auto remaining = context.deadline - now;
    const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining);
    auto milliseconds = whole.count();
    if (whole < remaining) ++milliseconds;
    return static_cast<DWORD>((std::min)(
        milliseconds,
        static_cast<decltype(milliseconds)>(
            (std::numeric_limits<DWORD>::max)() - 1U)));
}

void cancel_and_drain(HANDLE handle, OVERLAPPED& overlapped) noexcept {
    static_cast<void>(CancelIoEx(handle, &overlapped));
    DWORD ignored = 0;
    static_cast<void>(GetOverlappedResult(
        handle, &overlapped, &ignored, TRUE));
}

Result<DWORD> wait_for_io(HANDLE handle,
                          OVERLAPPED& overlapped,
                          const OperationContext& context,
                          const wchar_t* operation) {
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        const auto error = GetLastError();
        cancel_and_drain(handle, overlapped);
        return transport_error<DWORD>(
            error, operation, L"Failed to create cancellation event", error);
    }
    DWORD wait = WAIT_FAILED;
    DWORD wait_error = ERROR_SUCCESS;
    {
        std::stop_callback callback(
            context.stop_token,
            [stop_event]() noexcept {
                static_cast<void>(SetEvent(stop_event));
            });
        const HANDLE events[]{overlapped.hEvent, stop_event};
        wait = WaitForMultipleObjects(
            2, events, FALSE, deadline_timeout_ms(context));
        if (wait == WAIT_FAILED) wait_error = GetLastError();
    }
    static_cast<void>(CloseHandle(stop_event));
    if (wait == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
            const auto error = GetLastError();
            return transport_error<DWORD>(
                error, operation, L"Overlapped pipe I/O failed", error);
        }
        return Result<DWORD>::success(transferred);
    }
    if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
        cancel_and_drain(handle, overlapped);
        const auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<DWORD>::failure(context_result.error());
        }
        return transport_error<DWORD>(
            static_cast<std::uint32_t>(OperationErrorCode::timed_out),
            operation, L"Pipe operation exceeded its bounded wait");
    }
    cancel_and_drain(handle, overlapped);
    return transport_error<DWORD>(
        wait_error, operation, L"Waiting for pipe I/O failed", wait_error);
}

template <class Span>
Result<void> transfer_exact(HANDLE handle,
                            const OperationContext& context,
                            Span bytes) {
    constexpr bool write =
        std::is_const_v<typename Span::element_type>;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) return context_result;

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            const auto error = GetLastError();
            return transport_error<void>(
                error, write ? L"pipe.write" : L"pipe.read",
                L"Failed to create overlapped event", error);
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        const auto remaining = bytes.size() - offset;
        const auto chunk = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD transferred = 0;
        BOOL completed = FALSE;
        if constexpr (write) {
            completed = WriteFile(
                handle, bytes.data() + offset, chunk, &transferred, &overlapped);
        } else {
            completed = ReadFile(
                handle, bytes.data() + offset, chunk, &transferred, &overlapped);
        }
        if (!completed) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                static_cast<void>(CloseHandle(event));
                return transport_error<void>(
                    error, write ? L"pipe.write" : L"pipe.read",
                    error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED
                        ? L"Pipe peer disconnected" : L"Pipe I/O failed",
                    error);
            }
            auto waited = wait_for_io(
                handle, overlapped, context,
                write ? L"pipe.write" : L"pipe.read");
            static_cast<void>(CloseHandle(event));
            if (!waited.ok()) return Result<void>::failure(waited.error());
            transferred = waited.value();
        } else {
            static_cast<void>(CloseHandle(event));
        }
        if (transferred == 0) {
            return transport_error<void>(
                static_cast<std::uint32_t>(OperationErrorCode::session_closed),
                write ? L"pipe.write" : L"pipe.read",
                L"Pipe peer closed the session");
        }
        offset += transferred;
    }
    return Result<void>::success();
}

std::uint32_t read_payload_length(
    const std::array<std::byte, kFrameHeaderBytes>& header) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<unsigned>(header[14])
        | (static_cast<unsigned>(header[15]) << 8U)
        | (static_cast<unsigned>(header[16]) << 16U)
        | (static_cast<unsigned>(header[17]) << 24U));
}

} // namespace

PipeTransport::PipeTransport(void* native_handle) noexcept
    : handle_(native_handle) {}

PipeTransport::~PipeTransport() noexcept {
    close();
}

Result<std::unique_ptr<PipeTransport>> PipeTransport::connect(
    const OperationContext& context,
    std::wstring_view pipe_name) {
    if (pipe_name.empty() || pipe_name.size() > 256) {
        return transport_error<std::unique_ptr<PipeTransport>>(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"pipe.connect", L"Pipe name is invalid");
    }
    const auto bounded = bounded_context(context);
    const std::wstring name(pipe_name);
    while (true) {
        auto context_result = check_operation_context(bounded);
        if (!context_result.ok()) {
            return Result<std::unique_ptr<PipeTransport>>::failure(
                context_result.error());
        }
        HANDLE handle = CreateFileW(
            name.c_str(), static_cast<DWORD>(kPublicPipeClientAccessMask),
            0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                const auto error = GetLastError();
                static_cast<void>(CloseHandle(handle));
                return transport_error<std::unique_ptr<PipeTransport>>(
                    error, L"pipe.connect", L"Failed to select pipe byte mode",
                    error);
            }
            return Result<std::unique_ptr<PipeTransport>>::success(
                std::unique_ptr<PipeTransport>(new PipeTransport(handle)));
        }
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            return transport_error<std::unique_ptr<PipeTransport>>(
                error, L"pipe.connect", L"Failed to open local control pipe",
                error);
        }
        const auto wait_slice = (std::min)(
            deadline_timeout_ms(bounded), static_cast<DWORD>(50));
        if (!WaitNamedPipeW(name.c_str(), wait_slice)) {
            const auto wait_error = GetLastError();
            if (wait_error != ERROR_SEM_TIMEOUT
                && wait_error != ERROR_FILE_NOT_FOUND
                && wait_error != ERROR_PIPE_BUSY) {
                return transport_error<std::unique_ptr<PipeTransport>>(
                    wait_error, L"pipe.connect",
                    L"Waiting for local control pipe failed", wait_error);
            }
        }
    }
}

Result<std::unique_ptr<PipeTransport>> PipeTransport::adopt_connected_handle(
    void* native_handle) {
    if (native_handle == nullptr
        || native_handle == INVALID_HANDLE_VALUE) {
        return transport_error<std::unique_ptr<PipeTransport>>(
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"pipe.adopt", L"Connected pipe handle is invalid");
    }
    return Result<std::unique_ptr<PipeTransport>>::success(
        std::unique_ptr<PipeTransport>(new PipeTransport(native_handle)));
}

Result<void> PipeTransport::write_frame(const OperationContext& context,
                                        const Frame& frame) {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return transport_error<void>(
            static_cast<std::uint32_t>(OperationErrorCode::session_closed),
            L"pipe.write", L"Pipe session is closed");
    }
    auto encoded = encode_frame(frame);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    const auto bounded = bounded_context(context);
    return transfer_exact(
        static_cast<HANDLE>(handle_), bounded,
        std::span<const std::byte>(encoded.value()));
}

Result<Frame> PipeTransport::read_frame(const OperationContext& context) {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return transport_error<Frame>(
            static_cast<std::uint32_t>(OperationErrorCode::session_closed),
            L"pipe.read", L"Pipe session is closed");
    }
    const auto bounded = bounded_context(context);
    std::array<std::byte, kFrameHeaderBytes> header{};
    auto header_result = transfer_exact(
        static_cast<HANDLE>(handle_), bounded, std::span(header));
    if (!header_result.ok()) return Result<Frame>::failure(header_result.error());
    const auto payload_length = read_payload_length(header);
    if (payload_length > kMaxPayloadBytes) {
        return transport_error<Frame>(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"pipe.read", L"Pipe frame exceeds the protocol limit");
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kFrameHeaderBytes + payload_length);
    encoded.insert(encoded.end(), header.begin(), header.end());
    if (payload_length != 0) {
        const auto payload_offset = encoded.size();
        encoded.resize(payload_offset + payload_length);
        auto payload_result = transfer_exact(
            static_cast<HANDLE>(handle_), bounded,
            std::span(encoded).subspan(payload_offset));
        if (!payload_result.ok()) {
            return Result<Frame>::failure(payload_result.error());
        }
    }
    return decode_frame(encoded);
}

Result<CallerIdentity> PipeTransport::capture_caller_identity() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::session_closed),
            L"pipe.capture_caller_identity",
            L"Pipe session is closed"});
    }
    return capture_named_pipe_caller_identity(handle_);
}

void PipeTransport::close() noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CancelIoEx(static_cast<HANDLE>(handle_), nullptr));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
        handle_ = nullptr;
    }
}

} // namespace myrd::ipc
