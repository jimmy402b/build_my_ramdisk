#include "test_framework.h"

#include "src/m1/m1_raw_protocol_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
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
    int read_calls = 0;
    int write_calls = 0;
    int trim_calls = 0;

    Result<void> open(const std::wstring&) override {
        open_called = true;
        return Result<void>::success();
    }
    Result<void> read(std::uint64_t offset,
                      std::span<std::byte> buffer) override {
        read_calls++;
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
        return Result<void>::success();
    }
    Result<void> write(std::uint64_t offset,
                       std::span<const std::byte> buffer) override {
        write_calls++;
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
        if (flush_fails) {
            return Result<void>::failure({ErrorDomain::win32,
                ERROR_IO_DEVICE, L"flush", L"flush failed"});
        }
        return Result<void>::success();
    }
    Result<void> trim(std::uint64_t offset, std::uint64_t length) override {
        trim_calls++;
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
        fake, 64ull * 1024ull * 1024ull, 512, before, counters_fn);
    REQUIRE(result.ok());
    REQUIRE(result.value().zero_write_verified);
    REQUIRE(result.value().unmap_verified);
    REQUIRE(fake.write_calls >= 3);
    REQUIRE(fake.trim_calls >= 1);
}

TEST_CASE(m1_raw_protocol_io_short_read_fails) {
    FakeRawDiskApi fake;
    fake.short_read = true;
    auto before = make_counters(0, 0);
    auto result = verify_m1_raw_protocol(
        fake, 64ull * 1024ull * 1024ull, 512, before,
        []() { return make_counters(0, 0); });
    REQUIRE(result.has_error());
}

TEST_CASE(m1_raw_protocol_io_trim_failure) {
    FakeRawDiskApi fake;
    fake.trim_fails = true;
    auto before = make_counters(0, 0);
    auto result = verify_m1_raw_protocol(
        fake, 64ull * 1024ull * 1024ull, 512, before,
        []() { return make_counters(0, 0); });
    REQUIRE(result.has_error());
}

TEST_CASE(m1_raw_protocol_io_counter_not_increasing) {
    FakeRawDiskApi fake;
    auto before = make_counters(0, 0);
    // Counter never increases
    auto result = verify_m1_raw_protocol(
        fake, 64ull * 1024ull * 1024ull, 512, before,
        []() { return make_counters(0, 0); });
    // Should still succeed (counter verification is best-effort in unit)
    // but zero_write_verified/unmap_verified should be false
    REQUIRE(result.ok());
    REQUIRE(!result.value().zero_write_verified);
    REQUIRE(!result.value().unmap_verified);
}
