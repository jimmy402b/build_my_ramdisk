#include "test_framework.h"

#include "src/proxy/request_processor.h"
#include "src/storage/memory_block_device.h"

#include <cstring>
#include <vector>

constexpr std::uint64_t k64MiB = 64ull * 1024ull * 1024ull;

static std::vector<std::byte> make_mapping() {
    return std::vector<std::byte>(myrd::proxy::kMappingSize, std::byte{0xA5});
}

// ── INFO tests (Task 7) ──────────────────────────────────────

TEST_CASE(processor_rejects_mapping_smaller_than_protocol_header) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    std::vector<std::byte> small_map(100, std::byte{0});
    auto o = proc.process(std::span<std::byte>(small_map));
    REQUIRE(o.action == myrd::proxy::ProcessorAction::fatal);
}

TEST_CASE(processor_info_reports_64_mib) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    auto r = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(m.data(), myrd::proxy::kHeaderSize), 0);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().file_size, k64MiB);
}

TEST_CASE(processor_info_reports_512_alignment) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    auto r = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(m.data(), myrd::proxy::kHeaderSize), 0);
    REQUIRE_EQ(r.value().req_alignment, 512u);
}

TEST_CASE(processor_info_advertises_unmap_and_zero_flags) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    auto r = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(m.data(), myrd::proxy::kHeaderSize), 0);
    REQUIRE_EQ(r.value().flags,
        myrd::proxy::kFlagSupportsUnmap | myrd::proxy::kFlagSupportsZero);
}

TEST_CASE(processor_info_increments_only_info_counter) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    auto c = proc.counters();
    REQUIRE_EQ(c.info, 1u);
    REQUIRE_EQ(c.read, 0u);
    REQUIRE_EQ(c.write, 0u);
    REQUIRE_EQ(c.close, 0u);
}

TEST_CASE(processor_info_does_not_touch_data_area) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    std::vector<std::byte> orig(m.begin() + 4096, m.end());
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    for (std::size_t i = 4096; i < m.size(); ++i)
        REQUIRE(m[i] == orig[i - 4096]);
}

// ── READ tests (Task 8) ──────────────────────────────────────

TEST_CASE(processor_read_copies_backend_bytes_to_data_area) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    // Pre-fill backend
    std::byte wbuf[512]; std::memset(wbuf, 0xCD, 512);
    dev.write(myrd::OperationContext{}, 0, std::span<const std::byte>(wbuf, 512));
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::ReadRequest req{myrd::proxy::kReqRead, 0, 512};
    myrd::proxy::write_wire(m, 0, req);
    auto o = proc.process(m);
    REQUIRE(o.action == myrd::proxy::ProcessorAction::respond);
    // Data area should contain backend bytes
    REQUIRE(m[4096] == std::byte{0xCD});
    REQUIRE_EQ(proc.counters().read, 1u);
}

TEST_CASE(processor_read_writes_success_response_after_data) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::ReadRequest req{myrd::proxy::kReqRead, 0, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(resp.value().length, 512u);
}

TEST_CASE(processor_read_rejects_length_larger_than_data_area) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::ReadRequest req{myrd::proxy::kReqRead, 0,
        9ull * 1024ull * 1024ull}; // > 8 MiB
    myrd::proxy::write_wire(m, 0, req);
    auto o = proc.process(m);
    REQUIRE(o.action == myrd::proxy::ProcessorAction::respond);
    auto resp = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
    REQUIRE_EQ(resp.value().length, 0u);
}

TEST_CASE(processor_read_returns_error_response_for_unaligned_range) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::ReadRequest req{myrd::proxy::kReqRead, 1, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
}

TEST_CASE(processor_read_returns_error_response_for_backend_range_failure) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::ReadRequest req{myrd::proxy::kReqRead, k64MiB, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
}

// ── WRITE tests ──────────────────────────────────────────────

TEST_CASE(processor_write_copies_data_area_to_backend) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    // Put data in data area
    std::memset(m.data() + 4096, 0xBB, 512);
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, 0, 512};
    myrd::proxy::write_wire(m, 0, req);
    auto o = proc.process(m);
    REQUIRE(o.action == myrd::proxy::ProcessorAction::respond);
    REQUIRE_EQ(proc.counters().write, 1u);
    // Verify backend received data
    std::byte rbuf[512]{};
    dev.read(myrd::OperationContext{}, 0, rbuf);
    REQUIRE(rbuf[0] == std::byte{0xBB});
}

TEST_CASE(processor_write_uses_private_copy_before_backend_call) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    std::memset(m.data() + 4096, 0xCC, 512);
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, 0, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    // Modify shared memory after write — backend should still have original
    std::memset(m.data() + 4096, 0xDD, 512);
    std::byte rbuf[512]{};
    dev.read(myrd::OperationContext{}, 0, rbuf);
    REQUIRE(rbuf[0] == std::byte{0xCC}); // Original preserved
}

TEST_CASE(processor_write_writes_success_response) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, 0, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE_EQ(resp.value().errorno, 0u);
    REQUIRE_EQ(resp.value().length, 512u);
}

TEST_CASE(processor_write_rejects_length_larger_than_data_area) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, 0,
        9ull * 1024ull * 1024ull};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
}

TEST_CASE(processor_write_returns_error_response_for_unaligned_range) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, 1, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
}

TEST_CASE(processor_write_returns_error_response_for_backend_failure) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::WriteRequest req{myrd::proxy::kReqWrite, k64MiB, 512};
    myrd::proxy::write_wire(m, 0, req);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    REQUIRE(resp.value().errorno != 0u);
}

// ── CLOSE and fatal tests ────────────────────────────────────

TEST_CASE(processor_close_returns_close_and_increments_counter) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::CloseRequest req{myrd::proxy::kReqClose};
    myrd::proxy::write_wire(m, 0, req);
    auto o = proc.process(m);
    REQUIRE(o.action == myrd::proxy::ProcessorAction::close);
    REQUIRE_EQ(proc.counters().close, 1u);
}

TEST_CASE(processor_unknown_code_returns_fatal) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, 999u);
    auto o = proc.process(m);
    REQUIRE(o.action == myrd::proxy::ProcessorAction::fatal);
}

TEST_CASE(processor_unknown_code_increments_fatal_counter) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, 999u);
    proc.process(m);
    REQUIRE_EQ(proc.counters().fatal_protocol_errors, 1u);
}

TEST_CASE(processor_never_advertises_unimplemented_capabilities) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    myrd::proxy::RequestProcessor proc(dev);
    auto m = make_mapping();
    myrd::proxy::write_wire<std::uint64_t>(m, 0, myrd::proxy::kReqInfo);
    proc.process(m);
    auto resp = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(m.data(), 4096), 0);
    // M1: UNMAP and ZERO are now implemented capabilities
    REQUIRE_EQ(resp.value().flags,
        myrd::proxy::kFlagSupportsUnmap | myrd::proxy::kFlagSupportsZero);
}
