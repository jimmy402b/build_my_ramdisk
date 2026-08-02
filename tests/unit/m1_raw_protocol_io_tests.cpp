#include "test_framework.h"

#include "src/m1/m1_raw_protocol_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

using namespace myrd;

namespace {

struct FakeRawDiskApi final : IM1RawDiskApi {
    std::vector<std::byte> storage =
        std::vector<std::byte>(64ull * 1024ull * 1024ull);
    bool open_called = false;
    bool flush_fails = false;
    bool trim_fails = false;
    bool short_read = false;
    bool short_write = false;
    int corrupt_read_call = 0;
    int flush_failure_call = 0;
    int read_calls = 0;
    int write_calls = 0;
    int flush_calls = 0;
    int trim_calls = 0;
    std::wstring opened_path;
    std::vector<std::string> operations;

    Result<void> open(const aim::LocatedDisk& disk) override {
        open_called = true;
        opened_path = disk.path;
        operations.push_back("open");
        return Result<void>::success();
    }
    Result<void> read(std::uint64_t offset,
                      std::span<std::byte> buffer) override {
        read_calls++;
        operations.push_back("read");
        if (short_read) {
            return Result<void>::failure({ErrorDomain::win32,
                ERROR_READ_FAULT, L"read", L"short read"});
        }
        if (offset + buffer.size() > storage.size()) {
            return Result<void>::failure({ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"read", L"out of bounds"});
        }
        std::memcpy(buffer.data(), storage.data() + offset, buffer.size());
        if (corrupt_read_call == read_calls && !buffer.empty()) {
            buffer.front() ^= std::byte{0xFF};
        }
        return Result<void>::success();
    }
    Result<void> write(std::uint64_t offset,
                       std::span<const std::byte> buffer) override {
        write_calls++;
        operations.push_back("write");
        if (short_write) {
            return Result<void>::failure({ErrorDomain::win32,
                ERROR_WRITE_FAULT, L"write", L"short write"});
        }
        if (offset + buffer.size() > storage.size()) {
            return Result<void>::failure({ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"write", L"out of bounds"});
        }
        std::memcpy(storage.data() + offset, buffer.data(), buffer.size());
        return Result<void>::success();
    }
    Result<void> flush() override {
        flush_calls++;
        operations.push_back("flush");
        if (flush_fails || flush_failure_call == flush_calls) {
            return Result<void>::failure({ErrorDomain::win32,
                ERROR_IO_DEVICE, L"flush", L"flush failed"});
        }
        return Result<void>::success();
    }
    Result<void> trim_range(
        std::uint64_t offset, std::uint64_t length) override {
        trim_calls++;
        operations.push_back("trim");
        if (trim_fails) {
            return Result<void>::failure({ErrorDomain::win32,
                ERROR_IO_DEVICE, L"trim", L"trim failed"});
        }
        std::memset(storage.data() + offset, 0,
                    static_cast<std::size_t>(length));
        return Result<void>::success();
    }
};

proxy::RequestCounters make_counters(int zero, int unmap) {
    proxy::RequestCounters c{};
    c.zero = static_cast<std::uint64_t>(zero);
    c.unmap = static_cast<std::uint64_t>(unmap);
    return c;
}

constexpr std::uint64_t kDiskSize = 64ull * 1024ull * 1024ull;

aim::LocatedDisk make_disk(std::uint64_t size = kDiskSize) {
    return {37, L"\\\\.\\PhysicalDrive37", size, 1, 2, 3};
}

} // anonymous namespace

TEST_CASE(m1_raw_protocol_io_expected_sequence) {
    FakeRawDiskApi fake;
    proxy::RequestCounters before = make_counters(0, 0);

    int counter_queries = 0;
    auto counters_fn = [&]() -> proxy::RequestCounters {
        counter_queries++;
        // Simulate zero/unmap being observed after first query
        if (counter_queries >= 1) {
            return make_counters(1, 1);
        }
        return make_counters(0, 0);
    };

    auto result = verify_m1_raw_protocol(
        fake, make_disk(), before, counters_fn);
    REQUIRE(result.ok());
    REQUIRE(fake.open_called);
    REQUIRE(fake.opened_path == make_disk().path);
    REQUIRE(result.value().zero_write_verified);
    REQUIRE(result.value().unmap_verified);
    REQUIRE(result.value().flush_succeeded);
    REQUIRE(result.value().write_sha256 == result.value().read_sha256);
    REQUIRE(result.value().write_sha256.size() == 64);
    REQUIRE(result.value().write_sha256 != "sha256-placeholder");
    REQUIRE_EQ(fake.write_calls, 3);
    REQUIRE_EQ(fake.read_calls, 3);
    REQUIRE_EQ(fake.flush_calls, 3);
    REQUIRE_EQ(fake.trim_calls, 1);
    const std::vector<std::string> expected{
        "open", "write", "flush", "read", "write", "flush", "read",
        "write", "flush", "trim", "read"};
    REQUIRE(fake.operations == expected);
}

TEST_CASE(m1_raw_protocol_io_short_read_fails) {
    FakeRawDiskApi fake;
    fake.short_read = true;
    auto before = make_counters(0, 0);
    auto result = verify_m1_raw_protocol(
        fake, make_disk(), before,
        []() { return make_counters(1, 1); });
    REQUIRE(result.has_error());
    REQUIRE_EQ(fake.read_calls, 1);
}

TEST_CASE(m1_raw_protocol_io_trim_failure) {
    FakeRawDiskApi fake;
    fake.trim_fails = true;
    auto before = make_counters(0, 0);
    auto result = verify_m1_raw_protocol(
        fake, make_disk(), before,
        []() { return make_counters(1, 1); });
    REQUIRE(result.has_error());
    REQUIRE_EQ(fake.trim_calls, 1);
}

TEST_CASE(m1_raw_protocol_io_flush_failure_is_not_ignored) {
    for (int failing_call = 1; failing_call <= 3; ++failing_call) {
        FakeRawDiskApi fake;
        fake.flush_failure_call = failing_call;
        auto result = verify_m1_raw_protocol(
            fake, make_disk(), make_counters(0, 0),
            []() { return make_counters(1, 1); });
        REQUIRE(result.has_error());
        REQUIRE_EQ(fake.flush_calls, failing_call);
    }
}

TEST_CASE(m1_raw_protocol_io_rejects_corrupt_pattern_zero_and_trim_readback) {
    for (int corrupt_call = 1; corrupt_call <= 3; ++corrupt_call) {
        FakeRawDiskApi fake;
        fake.corrupt_read_call = corrupt_call;
        auto result = verify_m1_raw_protocol(
            fake, make_disk(), make_counters(0, 0),
            []() { return make_counters(1, 1); });
        REQUIRE(result.has_error());
        REQUIRE_EQ(fake.read_calls, corrupt_call);
    }
}

TEST_CASE(m1_raw_protocol_io_rejects_device_too_small_or_wrong_path) {
    FakeRawDiskApi fake;
    auto too_small = verify_m1_raw_protocol(
        fake, make_disk(8ull * 1024ull * 1024ull), make_counters(0, 0),
        []() { return make_counters(1, 1); });
    REQUIRE(too_small.has_error());
    REQUIRE(!fake.open_called);

    auto wrong_path_disk = make_disk();
    wrong_path_disk.path = L"\\\\.\\PhysicalDrive38";
    auto wrong_path = verify_m1_raw_protocol(
        fake, wrong_path_disk, make_counters(0, 0),
        []() { return make_counters(1, 1); });
    REQUIRE(wrong_path.has_error());
    REQUIRE(!fake.open_called);
}

TEST_CASE(m1_raw_protocol_io_counter_not_increasing) {
    FakeRawDiskApi fake;
    auto before = make_counters(0, 0);
    // Counter never increases
    auto result = verify_m1_raw_protocol(
        fake, make_disk(), before,
        []() { return make_counters(0, 0); });
    REQUIRE(result.has_error());
}
