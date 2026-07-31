#include "test_framework.h"

#include "src/proxy/object_names.h"
#include "src/proxy/shared_memory_channel.h"

#include <windows.h>

static myrd::proxy::ObjectNames make_names() {
    auto id = myrd::proxy::generate_session_id();
    return myrd::proxy::ObjectNames::from_session(id.value()).value();
}

TEST_CASE(channel_creates_exact_mapping_capacity) {
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    auto& c = ch.value();
    REQUIRE_EQ(c.mapping().size(), myrd::proxy::kMappingSize);
    // Write first and last bytes
    c.mapping()[0] = std::byte{0xAB};
    c.mapping()[myrd::proxy::kMappingSize - 1] = std::byte{0xCD};
    REQUIRE(c.mapping()[0] == std::byte{0xAB});
}

TEST_CASE(channel_creates_auto_reset_request_event) {
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    // Auto-reset: signal once, first wait succeeds, second times out
    SetEvent(ch.value().request_event());
    DWORD r1 = WaitForSingleObject(ch.value().request_event(), 0);
    REQUIRE(r1 == WAIT_OBJECT_0);
    DWORD r2 = WaitForSingleObject(ch.value().request_event(), 0);
    REQUIRE(r2 == WAIT_TIMEOUT);
}

TEST_CASE(channel_creates_auto_reset_response_event) {
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    SetEvent(ch.value().response_event());
    DWORD r1 = WaitForSingleObject(ch.value().response_event(), 0);
    REQUIRE(r1 == WAIT_OBJECT_0);
    DWORD r2 = WaitForSingleObject(ch.value().response_event(), 0);
    REQUIRE(r2 == WAIT_TIMEOUT);
}

TEST_CASE(channel_exposes_mapping_to_second_handle) {
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    ch.value().mapping()[0] = std::byte{0x42};
    // Open a second view
    HANDLE h2 = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                  names.mapping.c_str());
    REQUIRE(h2 != nullptr);
    void* v2 = MapViewOfFile(h2, FILE_MAP_READ, 0, 0,
                              myrd::proxy::kMappingSize);
    REQUIRE(v2 != nullptr);
    auto* p = static_cast<std::byte*>(v2);
    REQUIRE(p[0] == std::byte{0x42});
    UnmapViewOfFile(v2);
    CloseHandle(h2);
}

TEST_CASE(channel_rejects_duplicate_session) {
    auto names = make_names();
    auto ch1 = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch1.ok());
    auto ch2 = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch2.has_error());
}

TEST_CASE(channel_move_preserves_ownership) {
    auto names = make_names();
    auto ch1 = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch1.ok());
    HANDLE req = ch1.value().request_event();
    auto ch2(std::move(ch1.value()));
    REQUIRE(ch2.request_event() == req);
    REQUIRE(ch2.mapping().size() == myrd::proxy::kMappingSize);
}

TEST_CASE(channel_destruction_releases_all_named_objects) {
    auto names = make_names();
    {
        auto ch = myrd::proxy::SharedMemoryChannel::create(names);
        REQUIRE(ch.ok());
    }
    // After destruction, all objects must be gone
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                 names.mapping.c_str());
    REQUIRE(h == nullptr);
    REQUIRE(GetLastError() == ERROR_FILE_NOT_FOUND);
    h = OpenEventW(EVENT_ALL_ACCESS, FALSE, names.request.c_str());
    REQUIRE(h == nullptr);
    h = OpenEventW(EVENT_ALL_ACCESS, FALSE, names.response.c_str());
    REQUIRE(h == nullptr);
    h = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, names.server.c_str());
    REQUIRE(h == nullptr);
}

// ── Server lifecycle tests (Task 11) ──────────────────────────

#include "src/proxy/shm_proxy_server.h"
#include "src/storage/memory_block_device.h"
#include <chrono>
#include <thread>

constexpr std::uint64_t k64MiB = 64ull * 1024ull * 1024ull;

struct ServerFixture {
    myrd::MemoryBlockDevice dev{k64MiB, 512};
    myrd::proxy::RequestProcessor proc{dev};
};

TEST_CASE(server_start_publishes_ready_before_return) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    REQUIRE(srv.state() == myrd::proxy::ServerState::constructed);
    auto r = srv.start();
    REQUIRE(r.ok());
    auto st = srv.state();
    REQUIRE(st == myrd::proxy::ServerState::running);
    srv.stop();
}

TEST_CASE(server_completes_local_info_exchange) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    HANDLE req_ev = ch.value().request_event();
    HANDLE rsp_ev = ch.value().response_event();
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();

    // Send INFO request from "client" side
    auto* mapping = static_cast<std::byte*>(MapViewOfFile(
        OpenFileMappingW(FILE_MAP_WRITE, FALSE, names.mapping.c_str()),
        FILE_MAP_WRITE, 0, 0, myrd::proxy::kMappingSize));
    auto w = myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping, myrd::proxy::kMappingSize),
        0, myrd::proxy::kReqInfo);
    REQUIRE(w.ok());
    SetEvent(req_ev);

    DWORD rc = WaitForSingleObject(rsp_ev, 5000);
    REQUIRE(rc == WAIT_OBJECT_0);

    auto resp = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(mapping, myrd::proxy::kHeaderSize), 0);
    REQUIRE(resp.ok());
    REQUIRE_EQ(resp.value().file_size, k64MiB);

    UnmapViewOfFile(mapping);
    srv.stop();
}

TEST_CASE(server_completes_local_write_then_read_exchange) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    HANDLE req_ev = ch.value().request_event();
    HANDLE rsp_ev = ch.value().response_event();
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();

    auto* mapping = static_cast<std::byte*>(MapViewOfFile(
        OpenFileMappingW(FILE_MAP_WRITE, FALSE, names.mapping.c_str()),
        FILE_MAP_WRITE, 0, 0, myrd::proxy::kMappingSize));

    // WRITE
    std::memset(mapping + myrd::proxy::kHeaderSize, 0x77, 512);
    myrd::proxy::WriteRequest wreq{myrd::proxy::kReqWrite, 0, 512};
    myrd::proxy::write_wire(
        std::span<std::byte>(mapping, myrd::proxy::kHeaderSize), 0, wreq);
    SetEvent(req_ev);
    REQUIRE(WaitForSingleObject(rsp_ev, 5000) == WAIT_OBJECT_0);

    // READ
    myrd::proxy::ReadRequest rreq{myrd::proxy::kReqRead, 0, 512};
    myrd::proxy::write_wire(
        std::span<std::byte>(mapping, myrd::proxy::kHeaderSize), 0, rreq);
    SetEvent(req_ev);
    REQUIRE(WaitForSingleObject(rsp_ev, 5000) == WAIT_OBJECT_0);
    auto rresp = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(mapping, myrd::proxy::kHeaderSize), 0);
    REQUIRE(rresp.ok());
    REQUIRE_EQ(rresp.value().errorno, 0u);
    REQUIRE(mapping[myrd::proxy::kHeaderSize] == std::byte{0x77});

    UnmapViewOfFile(mapping);
    srv.stop();
}

TEST_CASE(server_close_transitions_to_close_seen) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    HANDLE req_ev = ch.value().request_event();
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();
    auto* mapping = static_cast<std::byte*>(MapViewOfFile(
        OpenFileMappingW(FILE_MAP_WRITE, FALSE, names.mapping.c_str()),
        FILE_MAP_WRITE, 0, 0, myrd::proxy::kMappingSize));
    myrd::proxy::CloseRequest creq{myrd::proxy::kReqClose};
    myrd::proxy::write_wire(
        std::span<std::byte>(mapping, myrd::proxy::kHeaderSize), 0, creq);
    SetEvent(req_ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(srv.state() == myrd::proxy::ServerState::close_seen);
    UnmapViewOfFile(mapping);
    srv.stop();
}

TEST_CASE(server_stop_before_start_is_safe) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    auto r = srv.stop();
    REQUIRE(r.ok()); // idempotent
}

TEST_CASE(server_stop_while_waiting_is_bounded) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();
    auto t0 = std::chrono::steady_clock::now();
    srv.stop(); // worker blocked on WaitForMultipleObjects
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    REQUIRE(ms.count() < 5000); // must be under 5s
}

TEST_CASE(server_stop_is_idempotent) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();
    srv.stop();
    auto r = srv.stop(); // second stop
    REQUIRE(r.ok());
}

TEST_CASE(server_unknown_request_transitions_to_failed) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    HANDLE req_ev = ch.value().request_event();
    myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
    srv.start();
    auto* mapping = static_cast<std::byte*>(MapViewOfFile(
        OpenFileMappingW(FILE_MAP_WRITE, FALSE, names.mapping.c_str()),
        FILE_MAP_WRITE, 0, 0, myrd::proxy::kMappingSize));
    myrd::proxy::write_wire<std::uint64_t>(
        std::span<std::byte>(mapping, myrd::proxy::kHeaderSize), 0, 999u);
    SetEvent(req_ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(srv.state() == myrd::proxy::ServerState::failed);
    UnmapViewOfFile(mapping);
    srv.stop();
}

TEST_CASE(server_destructor_joins_worker) {
    ServerFixture fx;
    auto names = make_names();
    auto ch = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(ch.ok());
    {
        myrd::proxy::ShmProxyServer srv(std::move(ch.value()), fx.proc);
        srv.start();
    } // destructor should join cleanly
    REQUIRE(true);
}
