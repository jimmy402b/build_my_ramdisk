#include "src/control/public_pipe_server.h"

#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <limits>
#include <optional>
#include <stop_token>
#include <utility>

namespace myrd::ipc {

namespace {

constexpr DWORD kPipeBufferBytes = 64U * 1024U;
constexpr auto kServerOperationBound = std::chrono::seconds(30);
static_assert(kPublicPipeModeFlags
              == (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                  | PIPE_REJECT_REMOTE_CLIENTS));

class PipeHandleOwner final {
public:
    explicit PipeHandleOwner(HANDLE handle) noexcept : handle_(handle) {}
    ~PipeHandleOwner() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
        }
    }
    PipeHandleOwner(const PipeHandleOwner&) = delete;
    PipeHandleOwner& operator=(const PipeHandleOwner&) = delete;

    HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HANDLE handle_{};
};

template <class T>
Result<T> server_error(std::uint32_t code,
                       const wchar_t* operation,
                       const wchar_t* message,
                       std::uint32_t native_code = 0) {
    return Result<T>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        code, operation, message, native_code});
}

Result<void*> create_public_instance(bool must_be_first_instance) {
    auto create_with = [must_be_first_instance](
                           PublicPipeSecurityDescriptor& security) {
        return CreateNamedPipeW(
            kPublicControlPipeName.data(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
                | (must_be_first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            static_cast<DWORD>(kPublicPipeModeFlags),
            static_cast<DWORD>(kMaximumPublicPipeInstances),
            kPipeBufferBytes,
            kPipeBufferBytes,
            0,
            static_cast<SECURITY_ATTRIBUTES*>(
                security.native_security_attributes()));
    };

    auto security = create_public_pipe_security_descriptor(
        PublicPipeMandatoryLabel::include);
    if (!security.ok()) {
        return Result<void*>::failure(security.error());
    }
    HANDLE handle = create_with(security.value());
    if (handle == INVALID_HANDLE_VALUE
        && GetLastError() == ERROR_PRIVILEGE_NOT_HELD) {
        auto fallback = create_public_pipe_security_descriptor(
            PublicPipeMandatoryLabel::omit_for_privilege_fallback);
        if (!fallback.ok()) {
            return Result<void*>::failure(fallback.error());
        }
        handle = create_with(fallback.value());
    }
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return server_error<void*>(
            error, L"public_pipe_server.create",
            L"Failed to create local public pipe instance", error);
    }
    return Result<void*>::success(handle);
}

void cancel_and_drain(HANDLE handle, OVERLAPPED& overlapped) noexcept {
    static_cast<void>(CancelIoEx(handle, &overlapped));
    DWORD ignored = 0;
    static_cast<void>(GetOverlappedResult(handle, &overlapped, &ignored, TRUE));
}

Result<void> connect_instance(HANDLE handle,
                              const OperationContext& context) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        const auto error = GetLastError();
        return server_error<void>(
            error, L"public_pipe_server.connect",
            L"Failed to create connect event", error);
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    if (ConnectNamedPipe(handle, &overlapped)) {
        static_cast<void>(CloseHandle(event));
        return Result<void>::success();
    }
    const auto connect_error = GetLastError();
    if (connect_error == ERROR_PIPE_CONNECTED) {
        static_cast<void>(CloseHandle(event));
        return Result<void>::success();
    }
    if (connect_error != ERROR_IO_PENDING) {
        static_cast<void>(CloseHandle(event));
        return server_error<void>(
            connect_error, L"public_pipe_server.connect",
            L"Failed to connect public pipe instance", connect_error);
    }

    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        const auto error = GetLastError();
        cancel_and_drain(handle, overlapped);
        static_cast<void>(CloseHandle(event));
        return server_error<void>(
            error, L"public_pipe_server.connect",
            L"Failed to create stop event", error);
    }
    const auto remaining = context.deadline - std::chrono::steady_clock::now();
    const auto whole_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    auto raw_timeout = remaining <= std::chrono::steady_clock::duration::zero()
        ? 0LL : whole_timeout.count();
    if (remaining > std::chrono::steady_clock::duration::zero()
        && whole_timeout < remaining) {
        ++raw_timeout;
    }
    const auto timeout = static_cast<DWORD>((std::min)(
        raw_timeout,
        static_cast<long long>((std::numeric_limits<DWORD>::max)() - 1U)));
    DWORD wait = WAIT_FAILED;
    DWORD wait_error = ERROR_SUCCESS;
    {
        std::stop_callback callback(
            context.stop_token,
            [stop_event]() noexcept {
                static_cast<void>(SetEvent(stop_event));
            });
        const HANDLE events[]{event, stop_event};
        wait = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (wait == WAIT_FAILED) wait_error = GetLastError();
    }
    static_cast<void>(CloseHandle(stop_event));
    if (wait == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        if (!GetOverlappedResult(handle, &overlapped, &ignored, FALSE)) {
            const auto error = GetLastError();
            static_cast<void>(CloseHandle(event));
            return server_error<void>(
                error, L"public_pipe_server.connect",
                L"Completing pipe connection failed", error);
        }
        static_cast<void>(CloseHandle(event));
        return Result<void>::success();
    }
    cancel_and_drain(handle, overlapped);
    static_cast<void>(CloseHandle(event));
    if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
        auto checked = check_operation_context(context);
        if (!checked.ok()) return checked;
        return server_error<void>(
            static_cast<std::uint32_t>(OperationErrorCode::timed_out),
            L"public_pipe_server.connect", L"Pipe accept wait expired");
    }
    return server_error<void>(
        wait_error, L"public_pipe_server.connect",
        L"Waiting for pipe connection failed", wait_error);
}

} // namespace

PublicPipeServer::PublicPipeServer(PublicPipeHandler handler,
                                   void* first_instance)
    : handler_(std::move(handler)) {
    void* unowned_first_instance = first_instance;
    try {
        workers_.reserve(kMaximumPublicPipeInstances);
        for (std::size_t index = 0;
             index < kMaximumPublicPipeInstances;
             ++index) {
            workers_.emplace_back(
                [this, instance = index == 0 ? first_instance : nullptr]
                (std::stop_token token) {
                    run(token, instance);
                });
            if (index == 0) unowned_first_instance = nullptr;
        }
    } catch (...) {
        if (unowned_first_instance != nullptr
            && unowned_first_instance != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(
                static_cast<HANDLE>(unowned_first_instance)));
        }
        for (auto& worker : workers_) worker.request_stop();
        throw;
    }
}

PublicPipeServer::~PublicPipeServer() noexcept {
    for (auto& worker : workers_) worker.request_stop();
}

Result<std::unique_ptr<PublicPipeServer>> PublicPipeServer::start(
    PublicPipeHandler handler) {
    if (!handler) {
        return server_error<std::unique_ptr<PublicPipeServer>>(
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"public_pipe_server.start", L"Pipe request handler is missing");
    }
    auto instance = create_public_instance(true);
    if (!instance.ok()) {
        return Result<std::unique_ptr<PublicPipeServer>>::failure(
            instance.error());
    }
    PipeHandleOwner instance_owner(static_cast<HANDLE>(instance.value()));
    return Result<std::unique_ptr<PublicPipeServer>>::success(
        std::unique_ptr<PublicPipeServer>(
            new PublicPipeServer(
                std::move(handler), instance_owner.release())));
}

Result<void> PublicPipeServer::stop(const OperationContext& context) {
    for (auto& worker : workers_) worker.request_stop();
    std::unique_lock lock(state_mutex_);
    while (workers_done_ != workers_.size()) {
        if (context.stop_token.stop_requested()) {
            return Result<void>::failure(check_operation_context(context).error());
        }
        if (state_cv_.wait_until(lock, context.deadline)
            == std::cv_status::timeout
            && workers_done_ != workers_.size()) {
            return Result<void>::failure(Error{
                ErrorDomain::protocol,
                static_cast<std::uint32_t>(OperationErrorCode::timed_out),
                L"public_pipe_server.stop", L"Server stop deadline expired"});
        }
    }
    lock.unlock();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    return Result<void>::success();
}

void PublicPipeServer::run(std::stop_token stop_token,
                           void* first_instance) noexcept {
    void* instance = first_instance;
    try {
        while (!stop_token.stop_requested()) {
            if (instance == nullptr || instance == INVALID_HANDLE_VALUE) {
                auto created = create_public_instance(false);
                if (!created.ok()) break;
                instance = created.value();
            }
            const OperationContext accept_context{
                stop_token,
                std::chrono::steady_clock::now() + kServerOperationBound};
            auto connected = connect_instance(
                static_cast<HANDLE>(instance), accept_context);
            if (!connected.ok()) {
                static_cast<void>(CloseHandle(static_cast<HANDLE>(instance)));
                instance = nullptr;
                continue;
            }
            auto transport = PipeTransport::adopt_connected_handle(instance);
            instance = nullptr;
            if (!transport.ok()) break;

            while (!stop_token.stop_requested()) {
                const OperationContext request_context{
                    stop_token,
                    std::chrono::steady_clock::now() + kServerOperationBound};
                auto request = transport.value()->read_frame(request_context);
                if (!request.ok()) break;
                if (request.value().message_type != MessageType::request) break;
                auto caller = transport.value()->capture_caller_identity();
                if (!caller.ok()) break;
                std::optional<Result<Frame>> response;
                try {
                    response.emplace(handler_(
                        request_context, caller.value(), request.value()));
                } catch (...) {
                    break;
                }
                if (!response->ok()
                    || response->value().message_type != MessageType::response
                    || response->value().request_id
                        != request.value().request_id) {
                    break;
                }
                auto written = transport.value()->write_frame(
                    request_context, response->value());
                if (!written.ok()) break;
            }
        }
    } catch (...) {
        // A request-local allocation failure must retire only this worker.
    }
    if (instance != nullptr && instance != INVALID_HANDLE_VALUE) {
        static_cast<void>(CancelIoEx(static_cast<HANDLE>(instance), nullptr));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(instance)));
    }
    {
        std::lock_guard lock(state_mutex_);
        ++workers_done_;
    }
    state_cv_.notify_all();
}

} // namespace myrd::ipc
