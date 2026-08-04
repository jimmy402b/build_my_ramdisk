#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/persistence/file_io.h"
#include "fault_file.h"
#include "test_framework.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace {

using myrd::OperationContext;
using myrd::OperationErrorCode;
using myrd::Result;
using myrd::persistence::FileOpenMode;
using myrd::persistence::IPersistenceFile;
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
