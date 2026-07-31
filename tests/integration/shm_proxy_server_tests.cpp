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
