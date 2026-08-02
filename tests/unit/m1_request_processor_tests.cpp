#include "test_framework.h"

#include "src/proxy/request_processor.h"
#include "tests/support/fake_block_device.h"

#include <array>
#include <chrono>
#include <cstring>
#include <vector>

using namespace myrd;
using namespace myrd::proxy;

constexpr std::uint64_t k64MiB = 64ull * 1024ull * 1024ull;

static std::vector<std::byte> make_mapping() {
    return std::vector<std::byte>(kMappingSize, std::byte{0xA5});
}

// ── Task 6: Fake device sanity ──────────────────────────────────

TEST_CASE(m1_processor_fake_initial_counters_are_zero) {
    test::FakeBlockDevice fake;
    REQUIRE_EQ(fake.read_calls, 0u);
    REQUIRE_EQ(fake.write_calls, 0u);
    REQUIRE_EQ(fake.unmap_calls, 0u);
    REQUIRE_EQ(fake.zero_calls, 0u);
    REQUIRE_EQ(fake.total_calls(), 0u);
}

// ── Task 7: INFO tests ──────────────────────────────────────────

TEST_CASE(m1_processor_info_reports_features) {
    test::FakeBlockDevice fake;
    fake.size = k64MiB;
    fake.sector = 512;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    write_wire<std::uint64_t>(m, 0, kReqInfo);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(!o.error.has_value());
    auto resp = read_wire<InfoResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().file_size, k64MiB);
    REQUIRE_EQ(resp.value().req_alignment, 512u);
    REQUIRE_EQ(resp.value().flags, kFlagSupportsUnmap | kFlagSupportsZero);
    REQUIRE_EQ(proc.counters().info, 1u);
}

// ── Task 7: READ tests ──────────────────────────────────────────

TEST_CASE(m1_processor_read_success) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    auto resp = read_wire<ReadResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(resp.value().length, 512u);
    REQUIRE_EQ(fake.read_calls, 1u);
}

TEST_CASE(m1_processor_read_data_before_response) {
    test::FakeBlockDevice fake;
    std::memset(fake.bytes.data(), 0xCD, 512);
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 512};
    write_wire(m, 0, req);
    proc.process(OperationContext{}, m);
    REQUIRE(m[kHeaderSize] == std::byte{0xCD});
}

TEST_CASE(m1_processor_read_rejects_zero_length) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 0};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    auto resp = read_wire<ReadResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE(resp.value().errorno != 0u);
    REQUIRE_EQ(resp.value().length, 0u);
    REQUIRE_EQ(fake.total_calls(), 0u);
}

TEST_CASE(m1_processor_read_rejects_overflow) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, k64MiB - 256, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
}

TEST_CASE(m1_processor_read_backend_error) {
    test::FakeBlockDevice fake;
    fake.next_result = Result<void>::failure({ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::backing_read_failed),
        L"fake", L"injected"});
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(proc.counters().read_errors, 1u);
}

TEST_CASE(m1_processor_read_cancelled_does_not_call_backend) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 512};
    write_wire(m, 0, req);
    std::stop_source src;
    src.request_stop();
    OperationContext ctx{src.get_token()};
    auto o = proc.process(ctx, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
    REQUIRE_EQ(proc.counters().cancelled, 1u);
}

TEST_CASE(m1_processor_read_expired_deadline_does_not_call_backend) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ReadRequest req{kReqRead, 0, 512};
    write_wire(m, 0, req);
    OperationContext ctx{{}, std::chrono::steady_clock::now() - std::chrono::seconds(1)};
    auto o = proc.process(ctx, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
    REQUIRE_EQ(proc.counters().timed_out, 1u);
}

// ── Task 8: WRITE tests ─────────────────────────────────────────

TEST_CASE(m1_processor_write_success) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    std::memset(m.data() + kHeaderSize, 0xBB, 512);
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    auto resp = read_wire<WriteResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(resp.value().length, 512u);
    REQUIRE_EQ(fake.write_calls, 1u);
    REQUIRE(fake.bytes[0] == std::byte{0xBB});
}

TEST_CASE(m1_processor_write_private_copy) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    std::memset(m.data() + kHeaderSize, 0xCC, 512);
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    proc.process(OperationContext{}, m);
    // SHM modified after write — backend should have original
    std::memset(m.data() + kHeaderSize, 0xDD, 512);
    REQUIRE(fake.bytes[0] == std::byte{0xCC});
}

TEST_CASE(m1_processor_write_rejects_zero_length) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    WriteRequest req{kReqWrite, 0, 0};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
}

TEST_CASE(m1_processor_write_rejects_data_area_overflow) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    WriteRequest req{kReqWrite, 0, 9ull * 1024ull * 1024ull};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
}

TEST_CASE(m1_processor_write_rejects_read_only) {
    test::FakeBlockDevice fake;
    // Inject a read-only error through next_result
    fake.next_result = Result<void>::failure({ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::read_only),
        L"fake", L"read-only"});
    RequestProcessor proc(fake);
    auto m = make_mapping();
    std::memset(m.data() + kHeaderSize, 0xBB, 512);
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.write_calls, 1u);
    REQUIRE_EQ(proc.counters().write_errors, 1u);
}

TEST_CASE(m1_processor_write_backend_error) {
    test::FakeBlockDevice fake;
    fake.next_result = Result<void>::failure({ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::memory_hard_limit),
        L"fake", L"injected"});
    RequestProcessor proc(fake);
    auto m = make_mapping();
    std::memset(m.data() + kHeaderSize, 0xBB, 512);
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(proc.counters().write_errors, 1u);
}

TEST_CASE(m1_processor_write_cancelled) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    std::stop_source src;
    src.request_stop();
    OperationContext ctx{src.get_token()};
    auto o = proc.process(ctx, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.total_calls(), 0u);
}

TEST_CASE(m1_processor_write_counter_increments_once) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    WriteRequest req{kReqWrite, 0, 512};
    write_wire(m, 0, req);
    proc.process(OperationContext{}, m);
    REQUIRE_EQ(proc.counters().write, 1u);
    // Error path doesn't double count
    fake.next_result = Result<void>::failure({ErrorDomain::storage, 0, L"", L""});
    write_wire(m, 0, req);
    proc.process(OperationContext{}, m);
    REQUIRE_EQ(proc.counters().write, 1u);
    REQUIRE_EQ(proc.counters().write_errors, 1u);
}

// ── Task 9: UNMAP tests ─────────────────────────────────────────

TEST_CASE(m1_processor_unmap_success) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    UnmapRequest req{kReqUnmap, static_cast<std::uint64_t>(sizeof(DataSetRangeWire))};
    write_wire(m, 0, req);
    DataSetRangeWire range{0, 512};
    std::memcpy(m.data() + kHeaderSize, &range, sizeof(range));
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    auto resp = read_wire<UnmapResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(fake.unmap_calls, 1u);
    REQUIRE_EQ(fake.last_ranges.size(), 1u);
}

TEST_CASE(m1_processor_unmap_empty_range_is_success) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    UnmapRequest req{kReqUnmap, 0};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE_EQ(fake.unmap_calls, 1u);
    REQUIRE(fake.last_ranges.empty());
}

TEST_CASE(m1_processor_unmap_normalizes_and_forwards) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    DataSetRangeWire wire[] = {{4096, 4096}, {0, 4096}};
    UnmapRequest req{kReqUnmap, static_cast<std::uint64_t>(sizeof(wire))};
    write_wire(m, 0, req);
    std::memcpy(m.data() + kHeaderSize, wire, sizeof(wire));
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    REQUIRE_EQ(fake.unmap_calls, 1u);
    REQUIRE_EQ(fake.last_ranges.size(), 1u);
    REQUIRE_EQ(fake.last_ranges[0].offset, 0u);
    REQUIRE_EQ(fake.last_ranges[0].length, 8192u);
}

TEST_CASE(m1_processor_unmap_rejects_malformed_length) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    UnmapRequest req{kReqUnmap, 100}; // not multiple of 16
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(fake.unmap_calls, 0u); // Backend not called due to validation failure
    REQUIRE_EQ(proc.counters().unmap_errors, 1u);
}

TEST_CASE(m1_processor_unmap_backend_error) {
    test::FakeBlockDevice fake;
    fake.next_result = Result<void>::failure({ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::decommit_failed),
        L"fake", L"injected"});
    RequestProcessor proc(fake);
    auto m = make_mapping();
    UnmapRequest req{kReqUnmap, static_cast<std::uint64_t>(sizeof(DataSetRangeWire))};
    write_wire(m, 0, req);
    DataSetRangeWire range{0, 512};
    std::memcpy(m.data() + kHeaderSize, &range, sizeof(range));
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(proc.counters().unmap_errors, 1u);
}

// ── Task 9: ZERO tests ──────────────────────────────────────────

TEST_CASE(m1_processor_zero_success) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ZeroRequest req{kReqZero, static_cast<std::uint64_t>(sizeof(DataSetRangeWire))};
    write_wire(m, 0, req);
    DataSetRangeWire range{0, 512};
    std::memcpy(m.data() + kHeaderSize, &range, sizeof(range));
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::respond);
    auto resp = read_wire<ZeroResponse>(
        std::span<const std::byte>(m.data(), kHeaderSize), 0);
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(fake.zero_calls, 1u);
}

TEST_CASE(m1_processor_zero_backend_error) {
    test::FakeBlockDevice fake;
    fake.next_result = Result<void>::failure({ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::decommit_failed),
        L"fake", L"injected"});
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ZeroRequest req{kReqZero, static_cast<std::uint64_t>(sizeof(DataSetRangeWire))};
    write_wire(m, 0, req);
    DataSetRangeWire range{0, 512};
    std::memcpy(m.data() + kHeaderSize, &range, sizeof(range));
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(proc.counters().zero_errors, 1u);
}

TEST_CASE(m1_processor_zero_independent_counter) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    ZeroRequest req{kReqZero, static_cast<std::uint64_t>(sizeof(DataSetRangeWire))};
    write_wire(m, 0, req);
    DataSetRangeWire range{0, 512};
    std::memcpy(m.data() + kHeaderSize, &range, sizeof(range));
    proc.process(OperationContext{}, m);
    REQUIRE_EQ(proc.counters().zero, 1u);
    REQUIRE_EQ(proc.counters().unmap, 0u);
}

// ── Task 9: CLOSE tests ─────────────────────────────────────────

TEST_CASE(m1_processor_close_no_response) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    CloseRequest req{kReqClose};
    write_wire(m, 0, req);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::close);
    REQUIRE(!o.error.has_value());
    REQUIRE_EQ(proc.counters().close, 1u);
}

// ── Task 9: Fatal tests ─────────────────────────────────────────

TEST_CASE(m1_processor_truncated_header_is_fatal) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    std::vector<std::byte> small(100, std::byte{0});
    auto o = proc.process(OperationContext{}, small);
    REQUIRE(o.action == ProcessorAction::fatal);
    REQUIRE_EQ(proc.counters().fatal_protocol_errors, 1u);
}

TEST_CASE(m1_processor_unknown_code_is_fatal) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    write_wire<std::uint64_t>(m, 0, 999u);
    auto o = proc.process(OperationContext{}, m);
    REQUIRE(o.action == ProcessorAction::fatal);
    REQUIRE(o.error.has_value());
    REQUIRE_EQ(proc.counters().fatal_protocol_errors, 1u);
}

TEST_CASE(m1_processor_fatal_decode_does_not_call_backend) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    write_wire<std::uint64_t>(m, 0, 999u);
    proc.process(OperationContext{}, m);
    REQUIRE_EQ(fake.total_calls(), 0u);
}

// ── M0 compatibility shim ───────────────────────────────────────

TEST_CASE(m1_processor_context_free_shim_works) {
    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    auto m = make_mapping();
    write_wire<std::uint64_t>(m, 0, kReqInfo);
    auto o = proc.process(m); // context-free overload
    REQUIRE(o.action == ProcessorAction::respond);
}
