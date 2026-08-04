#include "file_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <utility>

namespace myrd::persistence {
namespace {

constexpr auto kMaximumFileOffset =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[nodiscard]] Error win32_error(
    const std::uint32_t code,
    std::wstring operation,
    std::wstring message) {
    return {
        ErrorDomain::win32,
        code,
        std::move(operation),
        std::move(message),
        code,
    };
}

template <class T>
[[nodiscard]] Result<T> win32_failure(
    const std::uint32_t code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Result<T>::failure(win32_error(code, operation, message));
}

[[nodiscard]] Result<void> range_error(const wchar_t* operation) {
    return Result<void>::failure(win32_error(
        ERROR_ARITHMETIC_OVERFLOW,
        operation,
        L"file offset or end is outside the supported signed 64-bit range"));
}

[[nodiscard]] bool valid_range(
    const std::uint64_t offset,
    const std::size_t length) noexcept {
    if (offset > kMaximumFileOffset) {
        return false;
    }
    return static_cast<std::uint64_t>(length) <=
        kMaximumFileOffset - offset;
}

[[nodiscard]] OperationContext unlimited_context() noexcept {
    return {};
}

class ExactPersistenceFile final : public IPersistenceFile {
public:
    explicit ExactPersistenceFile(std::unique_ptr<IFileIoBackend> backend)
        : backend_(std::move(backend)) {}

    ~ExactPersistenceFile() noexcept override {
        if (backend_ != nullptr && !closed_) {
            static_cast<void>(backend_->close());
        }
    }

    Result<void> read_exact(
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        return read_exact(unlimited_context(), offset, destination);
    }

    Result<void> write_exact(
        const std::uint64_t offset,
        const std::span<const std::byte> source) override {
        return write_exact(unlimited_context(), offset, source);
    }

    Result<void> flush() override {
        return flush(unlimited_context());
    }

    Result<std::uint64_t> length() const override {
        return length(unlimited_context());
    }

    Result<void> truncate(const std::uint64_t new_length) override {
        return truncate(unlimited_context(), new_length);
    }

    Result<void> read_exact(
        const OperationContext& context,
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        if (!valid_range(offset, destination.size())) {
            return range_error(L"file_io.read");
        }
        auto ready = ready_for_io(context, L"file_io.read");
        if (!ready.ok()) {
            return ready;
        }

        std::size_t completed = 0;
        std::uint64_t current_offset = offset;
        while (completed < destination.size()) {
            auto context_result = check_operation_context(context);
            if (!context_result.ok()) {
                return context_result;
            }
            const auto remaining = destination.size() - completed;
            const auto chunk = (std::min)(
                remaining,
                static_cast<std::size_t>(
                    (std::numeric_limits<DWORD>::max)()));
            auto transfer = backend_->read_some(
                context,
                current_offset,
                destination.subspan(completed, chunk));
            if (!transfer.ok()) {
                return Result<void>::failure(std::move(transfer.error()));
            }
            if (transfer.value() > chunk) {
                return win32_failure<void>(
                    ERROR_INVALID_DATA,
                    L"file_io.read",
                    L"backend reported more bytes than requested");
            }
            if (transfer.value() == 0) {
                return win32_failure<void>(
                    ERROR_HANDLE_EOF,
                    L"file_io.read",
                    L"exact read reached EOF before completion");
            }
            completed += transfer.value();
            current_offset += transfer.value();
        }
        return Result<void>::success();
    }

    Result<void> write_exact(
        const OperationContext& context,
        const std::uint64_t offset,
        const std::span<const std::byte> source) override {
        if (!valid_range(offset, source.size())) {
            return range_error(L"file_io.write");
        }
        auto ready = ready_for_io(context, L"file_io.write");
        if (!ready.ok()) {
            return ready;
        }

        std::size_t completed = 0;
        std::uint64_t current_offset = offset;
        while (completed < source.size()) {
            auto context_result = check_operation_context(context);
            if (!context_result.ok()) {
                return context_result;
            }
            const auto remaining = source.size() - completed;
            const auto chunk = (std::min)(
                remaining,
                static_cast<std::size_t>(
                    (std::numeric_limits<DWORD>::max)()));
            auto transfer = backend_->write_some(
                context,
                current_offset,
                source.subspan(completed, chunk));
            if (!transfer.ok()) {
                return Result<void>::failure(std::move(transfer.error()));
            }
            if (transfer.value() > chunk) {
                return win32_failure<void>(
                    ERROR_INVALID_DATA,
                    L"file_io.write",
                    L"backend reported more bytes than requested");
            }
            if (transfer.value() == 0) {
                return win32_failure<void>(
                    ERROR_WRITE_FAULT,
                    L"file_io.write",
                    L"exact write made zero progress");
            }
            completed += transfer.value();
            current_offset += transfer.value();
        }
        return Result<void>::success();
    }

    Result<void> flush(const OperationContext& context) override {
        auto ready = ready_for_io(context, L"file_io.flush");
        if (!ready.ok()) {
            return ready;
        }
        return backend_->flush(context);
    }

    Result<std::uint64_t> length(
        const OperationContext& context) const override {
        auto ready = ready_for_io(context, L"file_io.length");
        if (!ready.ok()) {
            return Result<std::uint64_t>::failure(std::move(ready.error()));
        }
        auto result = backend_->length(context);
        if (!result.ok()) {
            return result;
        }
        if (result.value() > kMaximumFileOffset) {
            return win32_failure<std::uint64_t>(
                ERROR_ARITHMETIC_OVERFLOW,
                L"file_io.length",
                L"backend reported an unsupported file length");
        }
        return result;
    }

    Result<void> truncate(
        const OperationContext& context,
        const std::uint64_t new_length) override {
        if (new_length > kMaximumFileOffset) {
            return range_error(L"file_io.truncate");
        }
        auto ready = ready_for_io(context, L"file_io.truncate");
        if (!ready.ok()) {
            return ready;
        }
        return backend_->truncate(context, new_length);
    }

    Result<void> close() override {
        if (closed_) {
            return Result<void>::success();
        }
        if (backend_ == nullptr) {
            return invalid_handle(L"file_io.close");
        }
        auto result = backend_->close();
        if (result.ok()) {
            closed_ = true;
        }
        return result;
    }

private:
    std::unique_ptr<IFileIoBackend> backend_;
    bool closed_{};

    [[nodiscard]] Result<void> ready_for_io(
        const OperationContext& context,
        const wchar_t* operation) const {
        if (closed_ || backend_ == nullptr) {
            return invalid_handle(operation);
        }
        return check_operation_context(context);
    }

    [[nodiscard]] static Result<void> invalid_handle(
        const wchar_t* operation) {
        return win32_failure<void>(
            ERROR_INVALID_HANDLE,
            operation,
            L"persistence file is closed");
    }
};

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] DWORD deadline_timeout_ms(
    const OperationContext& context) noexcept {
    if (context.deadline ==
        std::chrono::steady_clock::time_point::max()) {
        return INFINITE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= context.deadline) {
        return 0;
    }
    const auto remaining = context.deadline - now;
    const auto whole_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(remaining);
    auto timeout = whole_ms.count();
    if (whole_ms < remaining) {
        ++timeout;
    }
    return static_cast<DWORD>((std::min)(
        timeout,
        static_cast<decltype(timeout)>((std::numeric_limits<DWORD>::max)() - 1)));
}

void cancel_and_drain(HANDLE handle, OVERLAPPED& overlapped) noexcept {
    // Even if cancellation loses a completion race or reports an unexpected
    // native error, the OVERLAPPED storage must remain alive until Windows no
    // longer owns it. GetOverlappedResult(TRUE) supplies that lifetime fence.
    static_cast<void>(CancelIoEx(handle, &overlapped));
    DWORD ignored = 0;
    static_cast<void>(GetOverlappedResult(
        handle, &overlapped, &ignored, TRUE));
}

[[nodiscard]] Result<std::size_t> wait_for_transfer(
    HANDLE handle,
    OVERLAPPED& overlapped,
    const OperationContext& context,
    const wchar_t* operation) {
    ScopedHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (stop_event.get() == nullptr) {
        const auto event_error = GetLastError();
        cancel_and_drain(handle, overlapped);
        return win32_failure<std::size_t>(
            event_error, operation, L"failed to create cancellation event");
    }

    std::stop_callback callback(
        context.stop_token,
        [event = stop_event.get()]() noexcept {
            static_cast<void>(SetEvent(event));
        });

    const HANDLE events[] = {overlapped.hEvent, stop_event.get()};
    const auto wait_result = WaitForMultipleObjects(
        2, events, FALSE, deadline_timeout_ms(context));
    if (wait_result == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (!GetOverlappedResult(
                handle, &overlapped, &transferred, FALSE)) {
            return win32_failure<std::size_t>(
                GetLastError(), operation, L"overlapped file I/O failed");
        }
        return Result<std::size_t>::success(transferred);
    }
    if (wait_result == WAIT_OBJECT_0 + 1) {
        cancel_and_drain(handle, overlapped);
        return Result<std::size_t>::failure(
            std::move(check_operation_context(context).error()));
    }
    if (wait_result == WAIT_TIMEOUT) {
        cancel_and_drain(handle, overlapped);
        return Result<std::size_t>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::timed_out),
            L"check_operation_context",
            L"operation deadline expired",
        });
    }
    const auto error = GetLastError();
    cancel_and_drain(handle, overlapped);
    return win32_failure<std::size_t>(
        error, operation, L"waiting for overlapped file I/O failed");
}

class WindowsFileIoBackend final : public IFileIoBackend {
public:
    explicit WindowsFileIoBackend(HANDLE handle) noexcept : handle_(handle) {}

    ~WindowsFileIoBackend() noexcept override {
        std::unique_lock lock(handle_mutex_);
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    Result<std::size_t> read_some(
        const OperationContext& context,
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        return transfer(
            context, offset, destination, {}, false, L"file_io.read");
    }

    Result<std::size_t> write_some(
        const OperationContext& context,
        const std::uint64_t offset,
        const std::span<const std::byte> source) override {
        return transfer(
            context, offset, {}, source, true, L"file_io.write");
    }

    Result<void> flush(const OperationContext& context) override {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return context_result;
        }
        std::shared_lock lock(handle_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return win32_failure<void>(
                ERROR_INVALID_HANDLE, L"file_io.flush", L"file is closed");
        }
        if (!FlushFileBuffers(handle_)) {
            return win32_failure<void>(
                GetLastError(), L"file_io.flush", L"FlushFileBuffers failed");
        }
        return Result<void>::success();
    }

    Result<std::uint64_t> length(
        const OperationContext& context) const override {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<std::uint64_t>::failure(
                std::move(context_result.error()));
        }
        std::shared_lock lock(handle_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return win32_failure<std::uint64_t>(
                ERROR_INVALID_HANDLE, L"file_io.length", L"file is closed");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size)) {
            return win32_failure<std::uint64_t>(
                GetLastError(), L"file_io.length", L"GetFileSizeEx failed");
        }
        if (size.QuadPart < 0) {
            return win32_failure<std::uint64_t>(
                ERROR_INVALID_DATA,
                L"file_io.length",
                L"file reported a negative length");
        }
        return Result<std::uint64_t>::success(
            static_cast<std::uint64_t>(size.QuadPart));
    }

    Result<void> truncate(
        const OperationContext& context,
        const std::uint64_t new_length) override {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return context_result;
        }
        if (new_length > kMaximumFileOffset) {
            return range_error(L"file_io.truncate");
        }
        std::shared_lock lock(handle_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return win32_failure<void>(
                ERROR_INVALID_HANDLE, L"file_io.truncate", L"file is closed");
        }
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = static_cast<LONGLONG>(new_length);
        if (!SetFileInformationByHandle(
                handle_, FileEndOfFileInfo, &info, sizeof(info))) {
            return win32_failure<void>(
                GetLastError(),
                L"file_io.truncate",
                L"SetFileInformationByHandle failed");
        }
        return Result<void>::success();
    }

    Result<void> close() override {
        std::unique_lock lock(handle_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return Result<void>::success();
        }
        if (!CloseHandle(handle_)) {
            return win32_failure<void>(
                GetLastError(), L"file_io.close", L"CloseHandle failed");
        }
        handle_ = INVALID_HANDLE_VALUE;
        return Result<void>::success();
    }

private:
    mutable std::shared_mutex handle_mutex_;
    HANDLE handle_{INVALID_HANDLE_VALUE};

    Result<std::size_t> transfer(
        const OperationContext& context,
        const std::uint64_t offset,
        const std::span<std::byte> destination,
        const std::span<const std::byte> source,
        const bool write,
        const wchar_t* operation) {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<std::size_t>::failure(
                std::move(context_result.error()));
        }
        const auto length = write ? source.size() : destination.size();
        if (!valid_range(offset, length) ||
            length > (std::numeric_limits<DWORD>::max)()) {
            return win32_failure<std::size_t>(
                ERROR_ARITHMETIC_OVERFLOW,
                operation,
                L"transfer range exceeds Win32 limits");
        }

        std::shared_lock lock(handle_mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return win32_failure<std::size_t>(
                ERROR_INVALID_HANDLE, operation, L"file is closed");
        }
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            return win32_failure<std::size_t>(
                GetLastError(), operation, L"failed to create I/O event");
        }

        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        overlapped.hEvent = event.get();

        const auto byte_count = static_cast<DWORD>(length);
        const auto issued = write
            ? WriteFile(
                handle_, source.data(), byte_count, nullptr, &overlapped)
            : ReadFile(
                handle_, destination.data(), byte_count, nullptr, &overlapped);
        if (!issued) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return win32_failure<std::size_t>(
                    error, operation, L"positioned file I/O failed");
            }
        }
        return wait_for_transfer(handle_, overlapped, context, operation);
    }
};

} // namespace

std::unique_ptr<IPersistenceFile> make_exact_persistence_file(
    std::unique_ptr<IFileIoBackend> backend) {
    return std::make_unique<ExactPersistenceFile>(std::move(backend));
}

Result<std::unique_ptr<IPersistenceFile>> open_windows_persistence_file(
    const std::filesystem::path& path,
    const FileOpenMode mode) {
    DWORD access = 0;
    DWORD creation = OPEN_EXISTING;
    DWORD sharing = FILE_SHARE_READ;
    switch (mode) {
    case FileOpenMode::ReadOnlyExisting:
        access = GENERIC_READ;
        break;
    case FileOpenMode::ReadWriteExisting:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    case FileOpenMode::CreateNewReadWrite:
        access = GENERIC_READ | GENERIC_WRITE;
        creation = CREATE_NEW;
        sharing = 0;
        break;
    default:
        return Result<std::unique_ptr<IPersistenceFile>>::failure(
            win32_error(
                ERROR_INVALID_PARAMETER,
                L"file_io.open",
                L"unknown file open mode"));
    }

    const auto handle = CreateFileW(
        path.c_str(),
        access,
        sharing,
        nullptr,
        creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<IPersistenceFile>>::failure(
            win32_error(
                GetLastError(), L"file_io.open", L"CreateFileW failed"));
    }
    return Result<std::unique_ptr<IPersistenceFile>>::success(
        make_exact_persistence_file(
            std::make_unique<WindowsFileIoBackend>(handle)));
}

} // namespace myrd::persistence
