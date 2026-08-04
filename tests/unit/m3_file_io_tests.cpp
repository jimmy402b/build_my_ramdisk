#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/common/scope_exit.h"
#include "src/common/unique_handle.h"
#include "src/persistence/file_io.h"
#include "fault_file.h"
#include "test_framework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <latch>
#include <limits>
#include <mutex>
#include <new>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace task11_allocation_fault {
std::atomic<std::int64_t> fail_after{-1};
}

void* operator new(const std::size_t size) {
    const auto remaining = task11_allocation_fault::fail_after.load(
        std::memory_order_relaxed);
    if (remaining >= 0) {
        const auto prior = task11_allocation_fault::fail_after.fetch_sub(
            1, std::memory_order_relaxed);
        if (prior == 0) {
            task11_allocation_fault::fail_after.store(
                -1, std::memory_order_relaxed);
            throw std::bad_alloc();
        }
    }
    if (auto* const allocation = std::malloc(size == 0 ? 1 : size)) {
        return allocation;
    }
    throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* const allocation) noexcept {
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept {
    ::operator delete(allocation);
}

void operator delete(
    void* const allocation,
    const std::size_t) noexcept {
    ::operator delete(allocation);
}

void operator delete[](
    void* const allocation,
    const std::size_t) noexcept {
    ::operator delete[](allocation);
}

namespace {

using myrd::OperationContext;
using myrd::OperationErrorCode;
using myrd::Result;
using myrd::UniqueHandle;
using myrd::persistence::FileOpenMode;
using myrd::persistence::IFileIoBackend;
using myrd::persistence::IPersistenceFile;
using myrd::persistence::make_exact_persistence_file;
using myrd::persistence::open_windows_persistence_file;
using myrd::persistence::test::FaultFileAction;
using myrd::persistence::test::FaultFilePhase;
using myrd::persistence::test::FaultFileRule;
using myrd::persistence::test::make_fault_file;

class TemporaryPath final {
public:
    TemporaryPath() {
        static std::atomic<std::uint64_t> sequence{};
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
            (L"myrd-task11-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(id) + L".bin");
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ~TemporaryPath() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryPath(const TemporaryPath&) = delete;
    TemporaryPath& operator=(const TemporaryPath&) = delete;

    [[nodiscard]] const std::filesystem::path& get() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

template <class T>
void require_native_error(
    const Result<T>& result,
    const std::uint32_t code,
    const wchar_t* operation) {
    REQUIRE(result.has_error());
    REQUIRE_EQ(result.error().domain, myrd::ErrorDomain::win32);
    REQUIRE_EQ(result.error().code, code);
    REQUIRE_EQ(result.error().native_code, code);
    REQUIRE_EQ(result.error().operation, std::wstring(operation));
}

std::array<std::byte, 8> sample_bytes() {
    return {
        std::byte{0x10}, std::byte{0x21}, std::byte{0x32}, std::byte{0x43},
        std::byte{0x54}, std::byte{0x65}, std::byte{0x76}, std::byte{0x87},
    };
}

enum class BackendEntry : std::uint8_t {
    Flush,
    Truncate,
    Close,
};

class BlockingTransferBackend final : public IFileIoBackend {
public:
    Result<std::size_t> read_some(
        const OperationContext&,
        const std::uint64_t,
        const std::span<std::byte> destination) override {
        std::unique_lock lock(mutex_);
        transfer_entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return transfer_released_; });
        std::fill(destination.begin(), destination.end(), std::byte{0});
        return Result<std::size_t>::success(destination.size());
    }

    Result<std::size_t> write_some(
        const OperationContext&,
        const std::uint64_t,
        const std::span<const std::byte> source) override {
        std::unique_lock lock(mutex_);
        transfer_entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return transfer_released_; });
        return Result<std::size_t>::success(source.size());
    }

    Result<void> flush(const OperationContext&) override {
        mark_entry(BackendEntry::Flush);
        return Result<void>::success();
    }

    Result<std::uint64_t> length(const OperationContext&) const override {
        return Result<std::uint64_t>::success(4096);
    }

    Result<void> truncate(
        const OperationContext&,
        const std::uint64_t) override {
        mark_entry(BackendEntry::Truncate);
        return Result<void>::success();
    }

    Result<void> close() override {
        mark_entry(BackendEntry::Close);
        return Result<void>::success();
    }

    [[nodiscard]] bool wait_for_transfer(
        const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [this] { return transfer_entered_; });
    }

    [[nodiscard]] bool wait_for_entry(
        const BackendEntry entry,
        const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, entry] {
            return entry == BackendEntry::Flush
                ? flush_entered_
                : entry == BackendEntry::Truncate
                    ? truncate_entered_
                    : close_entered_;
        });
    }

    void release_transfer() {
        std::lock_guard lock(mutex_);
        transfer_released_ = true;
        condition_.notify_all();
    }

private:
    void mark_entry(const BackendEntry entry) {
        std::lock_guard lock(mutex_);
        if (entry == BackendEntry::Flush) {
            flush_entered_ = true;
        } else if (entry == BackendEntry::Truncate) {
            truncate_entered_ = true;
        } else {
            close_entered_ = true;
        }
        condition_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool transfer_entered_{};
    bool transfer_released_{};
    bool flush_entered_{};
    bool truncate_entered_{};
    bool close_entered_{};
};

class OverReportingBackend final : public IFileIoBackend {
public:
    explicit OverReportingBackend(const bool over_report_write)
        : over_report_write_(over_report_write) {}

    Result<std::size_t> read_some(
        const OperationContext&,
        const std::uint64_t,
        const std::span<std::byte> destination) override {
        return Result<std::size_t>::success(
            destination.size() + (over_report_write_ ? 0U : 1U));
    }

    Result<std::size_t> write_some(
        const OperationContext&,
        const std::uint64_t,
        const std::span<const std::byte> source) override {
        return Result<std::size_t>::success(
            source.size() + (over_report_write_ ? 1U : 0U));
    }

    Result<void> flush(const OperationContext&) override {
        return Result<void>::success();
    }

    Result<std::uint64_t> length(const OperationContext&) const override {
        return Result<std::uint64_t>::success(0);
    }

    Result<void> truncate(
        const OperationContext&,
        const std::uint64_t) override {
        return Result<void>::success();
    }

    Result<void> close() override {
        return Result<void>::success();
    }

private:
    bool over_report_write_{};
};

class OverlappedNamedPipe final {
public:
    OverlappedNamedPipe() {
        static std::atomic<std::uint64_t> sequence{};
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        path_ = L"\\\\.\\pipe\\myrd-task11-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(id);

        server_.reset(CreateNamedPipeW(
            path_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));
        if (!server_) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "CreateNamedPipeW");
        }

        UniqueHandle connect_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connect_event) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "CreateEventW(connect)");
        }
        OVERLAPPED connect{};
        connect.hEvent = connect_event.get();
        bool connect_pending = false;
        myrd::ScopeExit drain_connect([&]() noexcept {
            if (connect_pending) {
                static_cast<void>(CancelIoEx(server_.get(), &connect));
                DWORD ignored = 0;
                static_cast<void>(GetOverlappedResult(
                    server_.get(), &connect, &ignored, TRUE));
            }
        });
        const auto connected = ConnectNamedPipe(server_.get(), &connect);
        const auto connect_error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connect_error != ERROR_IO_PENDING) {
            throw std::system_error(
                static_cast<int>(connect_error),
                std::system_category(),
                "ConnectNamedPipe");
        }
        connect_pending = !connected;

        auto opened = open_windows_persistence_file(
            std::filesystem::path(path_),
            FileOpenMode::ReadWriteExisting);
        if (!opened.ok()) {
            throw std::system_error(
                static_cast<int>(opened.error().native_code),
                std::system_category(),
                "open_windows_persistence_file(named pipe)");
        }
        file_ = opened.move_value();

        if (!connected) {
            const auto wait = WaitForSingleObject(connect_event.get(), 2000);
            if (wait != WAIT_OBJECT_0) {
                throw std::runtime_error("named pipe connect timed out");
            }
            DWORD transferred = 0;
            if (!GetOverlappedResult(
                    server_.get(), &connect, &transferred, FALSE)) {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "GetOverlappedResult(connect)");
            }
            connect_pending = false;
        }
        drain_connect.release();
    }

    ~OverlappedNamedPipe() noexcept {
        file_.reset();
        if (server_) {
            static_cast<void>(DisconnectNamedPipe(server_.get()));
        }
    }

    OverlappedNamedPipe(const OverlappedNamedPipe&) = delete;
    OverlappedNamedPipe& operator=(const OverlappedNamedPipe&) = delete;

    [[nodiscard]] IPersistenceFile& file() noexcept { return *file_; }

    void write(const std::span<const std::byte> source) {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "CreateEventW(write)");
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        const auto issued = WriteFile(
            server_.get(),
            source.data(),
            static_cast<DWORD>(source.size()),
            nullptr,
            &overlapped);
        const auto issue_error = issued ? ERROR_SUCCESS : GetLastError();
        if (!issued && issue_error != ERROR_IO_PENDING) {
            throw std::system_error(
                static_cast<int>(issue_error),
                std::system_category(),
                "WriteFile(named pipe)");
        }
        const auto wait = WaitForSingleObject(event.get(), 2000);
        if (wait != WAIT_OBJECT_0) {
            static_cast<void>(CancelIoEx(server_.get(), &overlapped));
            DWORD ignored = 0;
            static_cast<void>(GetOverlappedResult(
                server_.get(), &overlapped, &ignored, TRUE));
            throw std::runtime_error("named pipe write timed out");
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(
                server_.get(), &overlapped, &transferred, FALSE)) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "GetOverlappedResult(write)");
        }
        if (transferred != source.size()) {
            throw std::runtime_error("named pipe write was partial");
        }
    }

private:
    std::wstring path_;
    UniqueHandle server_;
    std::unique_ptr<IPersistenceFile> file_;
};

template <class Operation>
void require_operation_waits_for_transfer(
    const BackendEntry expected_entry,
    Operation operation) {
    auto backend_owner = std::make_unique<BlockingTransferBackend>();
    auto* const backend = backend_owner.get();
    auto file = make_exact_persistence_file(std::move(backend_owner));
    std::array<std::byte, 1> byte{};

    auto transfer = std::async(std::launch::async, [&] {
        return file->read_exact(0, byte);
    });
    REQUIRE(backend->wait_for_transfer(std::chrono::seconds(2)));

    std::latch operation_started{1};
    auto competing = std::async(std::launch::async, [&] {
        operation_started.count_down();
        return operation(*file);
    });
    operation_started.wait();
    const auto entered_while_transfer_pending = backend->wait_for_entry(
        expected_entry, std::chrono::milliseconds(200));

    backend->release_transfer();
    const auto transfer_result = transfer.get();
    const auto competing_result = competing.get();
    REQUIRE(transfer_result.ok());
    REQUIRE(competing_result.ok());
    REQUIRE(!entered_while_transfer_pending);
}

} // namespace

TEST_CASE(m3_file_io_native_exact_random_access_flush_length_and_truncate) {
    TemporaryPath path;
    auto opened = open_windows_persistence_file(
        path.get(), FileOpenMode::CreateNewReadWrite);
    REQUIRE(opened.ok());
    auto file = opened.move_value();

    const auto data = sample_bytes();
    REQUIRE(file->write_exact(3, data).ok());
    REQUIRE(file->flush().ok());
    auto length = file->length();
    REQUIRE(length.ok());
    REQUIRE_EQ(length.value(), 11U);

    std::array<std::byte, 11> readback{};
    readback.fill(std::byte{0xCC});
    REQUIRE(file->read_exact(0, readback).ok());
    for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE_EQ(readback[index], std::byte{0});
    }
    for (std::size_t index = 0; index < data.size(); ++index) {
        REQUIRE_EQ(readback[index + 3], data[index]);
    }

    REQUIRE(file->truncate(6).ok());
    REQUIRE_EQ(file->length().value(), 6U);
    REQUIRE(file->close().ok());
    REQUIRE(file->close().ok());
}

TEST_CASE(m3_file_io_native_partial_read_reports_exact_eof) {
    TemporaryPath path;
    auto opened = open_windows_persistence_file(
        path.get(), FileOpenMode::CreateNewReadWrite);
    REQUIRE(opened.ok());
    auto file = opened.move_value();

    const std::array data{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    REQUIRE(file->write_exact(0, data).ok());
    std::array<std::byte, 4> output{};
    auto result = file->read_exact(0, output);
    require_native_error(result, ERROR_HANDLE_EOF, L"file_io.read");
    REQUIRE_EQ(output[0], data[0]);
    REQUIRE_EQ(output[1], data[1]);
    REQUIRE_EQ(output[2], data[2]);
}

TEST_CASE(m3_file_io_rejects_backend_transfer_over_report) {
    std::array<std::byte, 1> byte{};

    auto read_file = make_exact_persistence_file(
        std::make_unique<OverReportingBackend>(false));
    auto read_result = read_file->read_exact(0, byte);
    require_native_error(read_result, ERROR_INVALID_DATA, L"file_io.read");

    auto write_file = make_exact_persistence_file(
        std::make_unique<OverReportingBackend>(true));
    auto write_result = write_file->write_exact(0, byte);
    require_native_error(write_result, ERROR_INVALID_DATA, L"file_io.write");
}

TEST_CASE(m3_file_io_open_allocation_failure_is_mapped_without_handle_leak) {
    const std::filesystem::path null_device(L"NUL");
    for (const std::int64_t fail_after : {0, 1}) {
        DWORD before = 0;
        REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE);

        bool threw_bad_alloc = false;
        bool mapped_out_of_memory = false;
        task11_allocation_fault::fail_after.store(
            fail_after, std::memory_order_relaxed);
        try {
            auto opened = open_windows_persistence_file(
                null_device,
                FileOpenMode::ReadWriteExisting);
            mapped_out_of_memory = opened.has_error() &&
                opened.error().domain == myrd::ErrorDomain::win32 &&
                opened.error().code == ERROR_NOT_ENOUGH_MEMORY &&
                opened.error().native_code == ERROR_NOT_ENOUGH_MEMORY &&
                opened.error().operation == L"file_io.open";
        } catch (const std::bad_alloc&) {
            threw_bad_alloc = true;
        }

        DWORD after = 0;
        REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE);
        REQUIRE(!threw_bad_alloc);
        REQUIRE(mapped_out_of_memory);
        REQUIRE(after <= before);
    }
}

TEST_CASE(m3_file_io_native_pending_pipe_read_stop_cancels_and_drains) {
    OverlappedNamedPipe pipe;
    std::array<std::byte, 1> output{};
    std::stop_source stop;
    OperationContext context{
        stop.get_token(),
        std::chrono::steady_clock::now() + std::chrono::seconds(3),
    };

    auto pending = std::async(std::launch::async, [&] {
        return pipe.file().read_exact(context, 0, output);
    });
    REQUIRE_EQ(
        pending.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);
    stop.request_stop();
    REQUIRE_EQ(
        pending.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    const auto cancelled = pending.get();
    REQUIRE(cancelled.has_error());
    REQUIRE_EQ(
        cancelled.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));

    const std::array next{std::byte{0x5A}};
    pipe.write(next);
    OperationContext reuse_context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
    REQUIRE(pipe.file().read_exact(reuse_context, 0, output).ok());
    REQUIRE_EQ(output, next);
}

TEST_CASE(m3_file_io_native_pending_pipe_read_deadline_cancels_and_drains) {
    OverlappedNamedPipe pipe;
    std::array<std::byte, 1> output{};
    OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)};

    auto result = pipe.file().read_exact(context, 0, output);
    REQUIRE(result.has_error());
    REQUIRE_EQ(
        result.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::timed_out));

    const std::array next{std::byte{0x6B}};
    pipe.write(next);
    OperationContext reuse_context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
    REQUIRE(pipe.file().read_exact(reuse_context, 0, output).ok());
    REQUIRE_EQ(output, next);
}

TEST_CASE(m3_file_io_native_pending_pipe_write_stop_cancels_and_drains) {
    OverlappedNamedPipe pipe;
    std::vector<std::byte> input(1024U * 1024U, std::byte{0x4D});
    std::stop_source stop;
    OperationContext context{
        stop.get_token(),
        std::chrono::steady_clock::now() + std::chrono::seconds(3),
    };

    auto pending = std::async(std::launch::async, [&] {
        return pipe.file().write_exact(context, 0, input);
    });
    REQUIRE_EQ(
        pending.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);
    stop.request_stop();
    REQUIRE_EQ(
        pending.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    const auto cancelled = pending.get();
    REQUIRE(cancelled.has_error());
    REQUIRE_EQ(
        cancelled.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
}

TEST_CASE(m3_file_io_native_pending_pipe_completion_wins_late_stop_race) {
    OverlappedNamedPipe pipe;
    std::array<std::byte, 1> output{};
    std::stop_source stop;
    OperationContext context{
        stop.get_token(),
        std::chrono::steady_clock::now() + std::chrono::seconds(3),
    };

    auto pending = std::async(std::launch::async, [&] {
        return pipe.file().read_exact(context, 0, output);
    });
    REQUIRE_EQ(
        pending.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);
    const std::array expected{std::byte{0x7C}};
    pipe.write(expected);
    stop.request_stop();
    REQUIRE_EQ(
        pending.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    REQUIRE(pending.get().ok());
    REQUIRE_EQ(output, expected);
}

TEST_CASE(m3_file_io_native_close_waits_for_pending_pipe_read) {
    OverlappedNamedPipe pipe;
    std::array<std::byte, 1> output{};
    OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(3)};

    auto pending = std::async(std::launch::async, [&] {
        return pipe.file().read_exact(context, 0, output);
    });
    REQUIRE_EQ(
        pending.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);
    auto closing = std::async(std::launch::async, [&] {
        return pipe.file().close();
    });
    REQUIRE_EQ(
        closing.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);

    const std::array expected{std::byte{0x8D}};
    pipe.write(expected);
    REQUIRE_EQ(
        pending.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    REQUIRE_EQ(
        closing.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    REQUIRE(pending.get().ok());
    REQUIRE(closing.get().ok());
    REQUIRE_EQ(output, expected);
}

TEST_CASE(m3_file_io_partial_read_and_write_are_retried_at_every_byte) {
    const auto data = sample_bytes();
    for (std::size_t split = 1; split < data.size(); ++split) {
        auto writer = make_fault_file(0, {{
            FaultFilePhase::Write,
            1,
            static_cast<std::uint64_t>(split),
            FaultFileAction::ShortWrite,
            ERROR_SUCCESS,
        }});
        REQUIRE(writer.file->write_exact(0, data).ok());
        REQUIRE_EQ(writer.state->calls(FaultFilePhase::Write), 2U);
        REQUIRE_EQ(writer.state->logical_length(), data.size());

        auto reader = make_fault_file(data.size(), {{
            FaultFilePhase::Read,
            1,
            static_cast<std::uint64_t>(split),
            FaultFileAction::ShortRead,
            ERROR_SUCCESS,
        }});
        REQUIRE(reader.file->write_exact(0, data).ok());
        std::array<std::byte, data.size()> output{};
        REQUIRE(reader.file->read_exact(0, output).ok());
        REQUIRE_EQ(reader.state->calls(FaultFilePhase::Read), 2U);
        REQUIRE_EQ(output, data);
    }
}

TEST_CASE(m3_file_io_eof_and_zero_progress_fail_closed) {
    auto eof = make_fault_file(3);
    std::array<std::byte, 4> output{};
    auto eof_result = eof.file->read_exact(0, output);
    require_native_error(eof_result, ERROR_HANDLE_EOF, L"file_io.read");

    auto zero_read = make_fault_file(8, {{
        FaultFilePhase::Read,
        1,
        std::nullopt,
        FaultFileAction::ZeroProgress,
        ERROR_SUCCESS,
    }});
    auto zero_read_result = zero_read.file->read_exact(0, output);
    require_native_error(
        zero_read_result, ERROR_HANDLE_EOF, L"file_io.read");

    auto zero_write = make_fault_file(0, {{
        FaultFilePhase::Write,
        1,
        std::nullopt,
        FaultFileAction::ZeroProgress,
        ERROR_SUCCESS,
    }});
    auto zero_write_result = zero_write.file->write_exact(0, output);
    require_native_error(
        zero_write_result, ERROR_WRITE_FAULT, L"file_io.write");
    REQUIRE_EQ(zero_write.state->logical_length(), 0U);
}

TEST_CASE(m3_file_io_checks_offset_and_end_before_backend_calls) {
    auto fake = make_fault_file();
    std::array<std::byte, 3> bytes{};
    const auto largest =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

    auto bad_offset = fake.file->read_exact(
        (std::numeric_limits<std::uint64_t>::max)(), bytes);
    require_native_error(
        bad_offset, ERROR_ARITHMETIC_OVERFLOW, L"file_io.read");

    auto bad_end = fake.file->write_exact(largest - 1, bytes);
    require_native_error(
        bad_end, ERROR_ARITHMETIC_OVERFLOW, L"file_io.write");
    REQUIRE_EQ(fake.state->calls(FaultFilePhase::Read), 0U);
    REQUIRE_EQ(fake.state->calls(FaultFilePhase::Write), 0U);

    auto impossible_length = make_fault_file(
        (std::numeric_limits<std::uint64_t>::max)());
    auto impossible_length_result = impossible_length.file->length();
    require_native_error(
        impossible_length_result,
        ERROR_ARITHMETIC_OVERFLOW,
        L"file_io.length");

    std::span<std::byte> empty_read;
    std::span<const std::byte> empty_write;
    REQUIRE(fake.file->read_exact(largest, empty_read).ok());
    REQUIRE(fake.file->write_exact(largest, empty_write).ok());
    REQUIRE_EQ(fake.state->calls(FaultFilePhase::Read), 0U);
    REQUIRE_EQ(fake.state->calls(FaultFilePhase::Write), 0U);
}

TEST_CASE(m3_file_io_cancel_and_deadline_are_checked_before_and_between_chunks) {
    std::stop_source cancelled;
    cancelled.request_stop();
    OperationContext cancelled_context{
        cancelled.get_token(),
        std::chrono::steady_clock::time_point::max(),
    };
    auto pre_cancel = make_fault_file(8);
    std::array<std::byte, 8> bytes{};
    auto cancelled_result = pre_cancel.file->read_exact(
        cancelled_context, 0, bytes);
    REQUIRE(cancelled_result.has_error());
    REQUIRE_EQ(cancelled_result.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
    REQUIRE_EQ(pre_cancel.state->calls(FaultFilePhase::Read), 0U);
    auto cancelled_length = pre_cancel.file->length(cancelled_context);
    REQUIRE(cancelled_length.has_error());
    REQUIRE_EQ(cancelled_length.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
    REQUIRE_EQ(pre_cancel.state->calls(FaultFilePhase::Length), 0U);

    OperationContext expired_context{
        {}, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)};
    auto pre_deadline = make_fault_file();
    auto deadline_result = pre_deadline.file->write_exact(
        expired_context, 0, bytes);
    REQUIRE(deadline_result.has_error());
    REQUIRE_EQ(deadline_result.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::timed_out));
    REQUIRE_EQ(pre_deadline.state->calls(FaultFilePhase::Write), 0U);
    auto deadline_flush = pre_deadline.file->flush(expired_context);
    REQUIRE(deadline_flush.has_error());
    REQUIRE_EQ(deadline_flush.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::timed_out));
    auto deadline_truncate = pre_deadline.file->truncate(expired_context, 4);
    REQUIRE(deadline_truncate.has_error());
    REQUIRE_EQ(deadline_truncate.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::timed_out));
    REQUIRE_EQ(pre_deadline.state->calls(FaultFilePhase::Flush), 0U);
    REQUIRE_EQ(pre_deadline.state->calls(FaultFilePhase::Truncate), 0U);

    std::stop_source during;
    auto between = make_fault_file(
        8,
        {{
            FaultFilePhase::Read,
            1,
            4,
            FaultFileAction::ShortRead,
            ERROR_SUCCESS,
        }},
        [&during](const FaultFilePhase phase, const std::uint64_t call) {
            if (phase == FaultFilePhase::Read && call == 1) {
                during.request_stop();
            }
        });
    OperationContext during_context{
        during.get_token(),
        std::chrono::steady_clock::time_point::max(),
    };
    auto during_result = between.file->read_exact(
        during_context, 0, bytes);
    REQUIRE(during_result.has_error());
    REQUIRE_EQ(during_result.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
    REQUIRE_EQ(between.state->calls(FaultFilePhase::Read), 1U);
}

TEST_CASE(m3_file_io_flush_and_truncate_are_exclusive_with_transfers) {
    require_operation_waits_for_transfer(
        BackendEntry::Flush,
        [](IPersistenceFile& file) { return file.flush(); });
    require_operation_waits_for_transfer(
        BackendEntry::Truncate,
        [](IPersistenceFile& file) { return file.truncate(0); });
}

TEST_CASE(m3_file_io_close_waits_for_pending_transfer_and_closes_once) {
    require_operation_waits_for_transfer(
        BackendEntry::Close,
        [](IPersistenceFile& file) { return file.close(); });
}

TEST_CASE(m3_file_io_faults_cover_disk_torn_flush_corruption_and_close) {
    const auto data = sample_bytes();

    auto disk_full = make_fault_file(0, {{
        FaultFilePhase::Write, 1, std::nullopt,
        FaultFileAction::DiskFull, ERROR_DISK_FULL,
    }});
    auto disk_result = disk_full.file->write_exact(0, data);
    require_native_error(disk_result, ERROR_DISK_FULL, L"file_io.write");
    REQUIRE_EQ(disk_full.state->physical_bytes(), 0U);

    auto torn = make_fault_file(0, {{
        FaultFilePhase::Write, 1, 4,
        FaultFileAction::TornWrite, ERROR_WRITE_FAULT,
    }});
    auto torn_result = torn.file->write_exact(0, data);
    require_native_error(torn_result, ERROR_WRITE_FAULT, L"file_io.write");
    REQUIRE_EQ(torn.state->logical_length(), 4U);
    std::array<std::byte, 4> torn_prefix{};
    REQUIRE(torn.file->read_exact(0, torn_prefix).ok());
    for (std::size_t index = 0; index < torn_prefix.size(); ++index) {
        REQUIRE_EQ(torn_prefix[index], data[index]);
    }

    auto flush = make_fault_file(0, {{
        FaultFilePhase::Flush, 1, std::nullopt,
        FaultFileAction::FlushError, ERROR_IO_DEVICE,
    }});
    auto flush_result = flush.file->flush();
    require_native_error(flush_result, ERROR_IO_DEVICE, L"file_io.flush");

    auto nth_flush = make_fault_file(0, {{
        FaultFilePhase::Flush, 2, std::nullopt,
        FaultFileAction::FlushError, ERROR_CRC,
    }});
    REQUIRE(nth_flush.file->flush().ok());
    auto nth_flush_result = nth_flush.file->flush();
    require_native_error(nth_flush_result, ERROR_CRC, L"file_io.flush");
    REQUIRE(nth_flush.file->flush().ok());
    REQUIRE_EQ(nth_flush.state->calls(FaultFilePhase::Flush), 3U);

    auto corruption = make_fault_file(data.size(), {{
        FaultFilePhase::Read, 1, 3,
        FaultFileAction::ReadbackCorruption, ERROR_SUCCESS,
    }});
    REQUIRE(corruption.file->write_exact(0, data).ok());
    std::array<std::byte, data.size()> corrupted{};
    REQUIRE(corruption.file->read_exact(0, corrupted).ok());
    REQUIRE(corrupted[3] != data[3]);
    for (std::size_t index = 0; index < corrupted.size(); ++index) {
        if (index != 3) {
            REQUIRE_EQ(corrupted[index], data[index]);
        }
    }

    auto close = make_fault_file(0, {{
        FaultFilePhase::Close, 1, std::nullopt,
        FaultFileAction::CloseError, ERROR_WRITE_FAULT,
    }});
    auto close_result = close.file->close();
    require_native_error(close_result, ERROR_WRITE_FAULT, L"file_io.close");
    REQUIRE(!close.state->closed());
    REQUIRE(close.file->close().ok());
    REQUIRE(close.state->closed());
}

TEST_CASE(m3_file_io_fault_byte_ranges_are_half_open_across_requests) {
    const auto data = sample_bytes();

    for (const auto action : {
             FaultFileAction::DiskFull,
             FaultFileAction::TornWrite,
         }) {
        auto endpoint = make_fault_file(0, {{
            FaultFilePhase::Write,
            1,
            static_cast<std::uint64_t>(data.size()),
            action,
            ERROR_WRITE_FAULT,
        }});
        REQUIRE(endpoint.file->write_exact(0, data).ok());
        REQUIRE_EQ(endpoint.state->logical_length(), data.size());
    }

    auto second_request = make_fault_file(0, {{
        FaultFilePhase::Write,
        1,
        static_cast<std::uint64_t>(data.size()),
        FaultFileAction::DiskFull,
        ERROR_DISK_FULL,
    }});
    REQUIRE(second_request.file->write_exact(0, data).ok());
    std::array<std::byte, 4> suffix{};
    auto second_result = second_request.file->write_exact(
        data.size(), suffix);
    require_native_error(
        second_result, ERROR_DISK_FULL, L"file_io.write");
    REQUIRE_EQ(second_request.state->logical_length(), data.size());
    REQUIRE_EQ(second_request.state->calls(FaultFilePhase::Write), 2U);
}

TEST_CASE(m3_file_io_disk_full_and_torn_write_preserve_fault_prefix) {
    const auto data = sample_bytes();

    for (const auto action : {
             FaultFileAction::DiskFull,
             FaultFileAction::TornWrite,
         }) {
        const auto error = static_cast<std::uint32_t>(
            action == FaultFileAction::DiskFull
                ? ERROR_DISK_FULL
                : ERROR_WRITE_FAULT);
        auto fault = make_fault_file(0, {{
            FaultFilePhase::Write,
            1,
            4,
            action,
            error,
        }});
        auto result = fault.file->write_exact(0, data);
        require_native_error(result, error, L"file_io.write");
        REQUIRE_EQ(fault.state->logical_length(), 4U);

        std::array<std::byte, 4> prefix{};
        REQUIRE(fault.file->read_exact(0, prefix).ok());
        for (std::size_t index = 0; index < prefix.size(); ++index) {
            REQUIRE_EQ(prefix[index], data[index]);
        }

        auto immediate = make_fault_file(0, {{
            FaultFilePhase::Write,
            1,
            8,
            action,
            error,
        }});
        auto immediate_result = immediate.file->write_exact(8, data);
        require_native_error(immediate_result, error, L"file_io.write");
        REQUIRE_EQ(immediate.state->logical_length(), 0U);
        REQUIRE_EQ(immediate.state->physical_bytes(), 0U);
    }
}

TEST_CASE(m3_file_io_faults_cover_length_truncate_and_access_mapping) {
    auto length_fault = make_fault_file(8, {{
        FaultFilePhase::Length, 1, std::nullopt,
        FaultFileAction::NativeError, ERROR_ACCESS_DENIED,
    }});
    auto length_result = length_fault.file->length();
    require_native_error(
        length_result, ERROR_ACCESS_DENIED, L"file_io.length");

    auto truncate_fault = make_fault_file(8, {{
        FaultFilePhase::Truncate, 1, std::nullopt,
        FaultFileAction::NativeError, ERROR_DISK_FULL,
    }});
    auto truncate_result = truncate_fault.file->truncate(4);
    require_native_error(
        truncate_result, ERROR_DISK_FULL, L"file_io.truncate");
    REQUIRE_EQ(truncate_fault.state->logical_length(), 8U);

    TemporaryPath missing;
    auto missing_result = open_windows_persistence_file(
        missing.get(), FileOpenMode::ReadOnlyExisting);
    require_native_error(
        missing_result, ERROR_FILE_NOT_FOUND, L"file_io.open");

    auto invalid_mode = open_windows_persistence_file(
        missing.get(), static_cast<FileOpenMode>(0xFF));
    require_native_error(
        invalid_mode, ERROR_INVALID_PARAMETER, L"file_io.open");

    TemporaryPath path;
    auto created = open_windows_persistence_file(
        path.get(), FileOpenMode::CreateNewReadWrite);
    REQUIRE(created.ok());
    REQUIRE(created.value()->close().ok());
    auto read_only = open_windows_persistence_file(
        path.get(), FileOpenMode::ReadOnlyExisting);
    REQUIRE(read_only.ok());
    std::array<std::byte, 1> one{};
    auto denied = read_only.value()->write_exact(0, one);
    require_native_error(denied, ERROR_ACCESS_DENIED, L"file_io.write");
}

TEST_CASE(m3_file_io_multi_terabyte_sparse_fake_is_bounded) {
    constexpr std::uint64_t tebibyte = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t logical_size = 4ULL * tebibyte + 8192ULL;
    auto sparse = make_fault_file();
    REQUIRE(sparse.file->truncate(logical_size).ok());
    REQUIRE_EQ(sparse.state->physical_bytes(), 0U);

    std::array<std::byte, 4096> data{};
    data.fill(std::byte{0xA5});
    REQUIRE(sparse.file->write_exact(logical_size - data.size(), data).ok());
    REQUIRE_EQ(sparse.state->allocated_pages(), 1U);
    REQUIRE_EQ(sparse.state->physical_bytes(), 4096U);

    std::array<std::byte, 8192> range{};
    REQUIRE(sparse.file->read_exact(logical_size - range.size(), range).ok());
    REQUIRE_EQ(sparse.state->calls(FaultFilePhase::Read), 1U);
    for (std::size_t index = 0; index < 4096; ++index) {
        REQUIRE_EQ(range[index], std::byte{0});
    }
    for (std::size_t index = 4096; index < range.size(); ++index) {
        REQUIRE_EQ(range[index], std::byte{0xA5});
    }
}

TEST_CASE(m3_file_io_native_open_close_10000_times_does_not_leak_handles) {
    TemporaryPath path;
    auto created = open_windows_persistence_file(
        path.get(), FileOpenMode::CreateNewReadWrite);
    REQUIRE(created.ok());
    REQUIRE(created.value()->close().ok());

    DWORD before = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE);
    for (std::size_t iteration = 0; iteration < 10000; ++iteration) {
        auto opened = open_windows_persistence_file(
            path.get(), FileOpenMode::ReadOnlyExisting);
        REQUIRE(opened.ok());
        REQUIRE(opened.value()->close().ok());
    }
    DWORD after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE);
    REQUIRE(after <= before + 1);
}
