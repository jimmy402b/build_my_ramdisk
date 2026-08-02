#include "test_framework.h"

#include "src/proxy/imdproxy_protocol.h"
#include "src/proxy/object_names.h"
#include "src/proxy/request_processor.h"
#include "src/proxy/shared_memory_channel.h"
#include "src/proxy/shm_proxy_session.h"
#include "src/storage/memory_block_device.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using myrd::Error;
using myrd::ErrorDomain;
using myrd::Result;
using myrd::proxy::ChannelCreateStage;
using myrd::proxy::ObjectNames;
using myrd::proxy::SessionStartStage;
using myrd::proxy::SessionState;

constexpr std::uint64_t kDeviceSize = 16ull * 1024ull * 1024ull;

Error injected_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 0xE001u, operation, L"injected failure"};
}
ObjectNames make_names() {
    myrd::proxy::LocalNamespacePolicy policy;
    auto session = myrd::proxy::generate_session_id();
    REQUIRE(session.ok());
    auto names = policy.build_names(session.value());
    REQUIRE(names.ok());
    return names.value();
}

bool wait_for_state(myrd::proxy::ShmProxySession& session,
                    SessionState expected,
                    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (session.snapshot().state == expected) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return session.snapshot().state == expected;
}

bool named_objects_are_absent(const ObjectNames& names) {
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str());
    if (handle != nullptr) {
        CloseHandle(handle);
        return false;
    }
    handle = OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str());
    if (handle != nullptr) {
        CloseHandle(handle);
        return false;
    }
    for (const auto* event_name : {
             names.request.c_str(), names.response.c_str(), names.cancel.c_str()}) {
        handle = OpenEventW(SYNCHRONIZE, FALSE, event_name);
        if (handle != nullptr) {
            CloseHandle(handle);
            return false;
        }
    }
    return true;
}

template <class Request>
void send_request(std::span<std::byte> mapping,
                  HANDLE request_event,
                  HANDLE response_event,
                  const Request& request) {
    auto write = myrd::proxy::write_wire(
        mapping.first(myrd::proxy::kHeaderSize), 0, request);
    REQUIRE(write.ok());
    REQUIRE(SetEvent(request_event) != FALSE);
    REQUIRE(WaitForSingleObject(response_event, 2000) == WAIT_OBJECT_0);
}

void write_range(std::span<std::byte> mapping,
                 std::uint64_t offset,
                 std::uint64_t length) {
    const myrd::proxy::DataSetRangeWire range{offset, length};
    auto result = myrd::proxy::write_wire(
        mapping.subspan(myrd::proxy::kHeaderSize), 0, range);
    REQUIRE(result.ok());
}

} // namespace

TEST_CASE(m1_channel_mapping_is_zeroed_and_handles_are_not_inheritable) {
    auto names = make_names();
    auto channel = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(channel.ok());

    for (const auto value : channel.value().mapping()) {
        REQUIRE(value == std::byte{0});
    }

    for (HANDLE handle : {channel.value().request_event(),
                          channel.value().response_event(),
                          channel.value().cancel_event()}) {
        DWORD flags = HANDLE_FLAG_INHERIT;
        REQUIRE(GetHandleInformation(handle, &flags) != FALSE);
        REQUIRE((flags & HANDLE_FLAG_INHERIT) == 0);
    }
}

TEST_CASE(m1_channel_rolls_back_every_injected_creation_failure) {
    const std::array stages{
        ChannelCreateStage::owner_mutex,
        ChannelCreateStage::mapping,
        ChannelCreateStage::map_view,
        ChannelCreateStage::request_event,
        ChannelCreateStage::response_event,
        ChannelCreateStage::cancel_event,
    };

    for (const auto failing_stage : stages) {
        auto names = make_names();
        auto channel = myrd::proxy::SharedMemoryChannel::create(
            names, [failing_stage](ChannelCreateStage stage) {
                if (stage == failing_stage) {
                    return Result<void>::failure(injected_error(L"channel_create"));
                }
                return Result<void>::success();
            });
        REQUIRE(channel.has_error());
        REQUIRE(named_objects_are_absent(names));
    }
}

TEST_CASE(m1_channel_rejects_duplicate_mapping_and_events_without_leaks) {
    enum class ExistingObject { mapping, request, response, cancel };
    for (const auto existing : {ExistingObject::mapping, ExistingObject::request,
                                ExistingObject::response, ExistingObject::cancel}) {
        auto names = make_names();
        HANDLE blocker = nullptr;
        if (existing == ExistingObject::mapping) {
            blocker = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE, 0, 4096, names.mapping.c_str());
        } else {
            const wchar_t* name = existing == ExistingObject::request
                ? names.request.c_str()
                : existing == ExistingObject::response
                    ? names.response.c_str()
                    : names.cancel.c_str();
            blocker = CreateEventW(nullptr, FALSE, FALSE, name);
        }
        REQUIRE(blocker != nullptr);

        auto channel = myrd::proxy::SharedMemoryChannel::create(names);
        REQUIRE(channel.has_error());
        CloseHandle(blocker);
        REQUIRE(named_objects_are_absent(names));
    }
}

TEST_CASE(m1_session_start_returns_ready_and_full_protocol_round_trip_works) {
    myrd::MemoryBlockDevice device(kDeviceSize, 512);
    myrd::proxy::RequestProcessor processor(device);
    auto names = make_names();
    auto channel = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(channel.ok());
    auto mapping = channel.value().mapping();
    const HANDLE request_event = channel.value().request_event();
    const HANDLE response_event = channel.value().response_event();

    myrd::proxy::ShmProxySession session(
        std::move(channel.value()), processor, {2s, 8});
    auto started = std::async(std::launch::async, [&session] {
        return session.start();
    });
    REQUIRE(started.wait_for(2s) == std::future_status::ready);
    REQUIRE(started.get().ok());
    REQUIRE(session.snapshot().state == SessionState::serving);

    const std::uint64_t info = myrd::proxy::kReqInfo;
    send_request(mapping, request_event, response_event, info);
    auto info_response = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(mapping.data(), myrd::proxy::kHeaderSize), 0);
    REQUIRE(info_response.ok());
    REQUIRE_EQ(info_response.value().file_size, kDeviceSize);
    REQUIRE_EQ(info_response.value().req_alignment, 512u);

    std::memset(mapping.data() + myrd::proxy::kHeaderSize, 0x5A, 1024);
    send_request(mapping, request_event, response_event,
                 myrd::proxy::WriteRequest{myrd::proxy::kReqWrite, 0, 1024});
    auto write_response = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(mapping.data(), myrd::proxy::kHeaderSize), 0);
    REQUIRE(write_response.ok());
    REQUIRE_EQ(write_response.value().errorno, 0u);

    std::memset(mapping.data() + myrd::proxy::kHeaderSize, 0, 1024);
    send_request(mapping, request_event, response_event,
                 myrd::proxy::ReadRequest{myrd::proxy::kReqRead, 0, 1024});
    REQUIRE(mapping[myrd::proxy::kHeaderSize] == std::byte{0x5A});

    write_range(mapping, 0, 512);
    send_request(mapping, request_event, response_event,
                 myrd::proxy::ZeroRequest{myrd::proxy::kReqZero,
                                          sizeof(myrd::proxy::DataSetRangeWire)});
    send_request(mapping, request_event, response_event,
                 myrd::proxy::ReadRequest{myrd::proxy::kReqRead, 0, 512});
    for (std::size_t index = 0; index < 512; ++index) {
        REQUIRE(mapping[myrd::proxy::kHeaderSize + index] == std::byte{0});
    }

    write_range(mapping, 512, 512);
    send_request(mapping, request_event, response_event,
                 myrd::proxy::UnmapRequest{myrd::proxy::kReqUnmap,
                                           sizeof(myrd::proxy::DataSetRangeWire)});
    send_request(mapping, request_event, response_event,
                 myrd::proxy::ReadRequest{myrd::proxy::kReqRead, 512, 512});
    for (std::size_t index = 0; index < 512; ++index) {
        REQUIRE(mapping[myrd::proxy::kHeaderSize + index] == std::byte{0});
    }

    const auto counters = session.snapshot().requests;
    REQUIRE_EQ(counters.info, 1u);
    REQUIRE_EQ(counters.write, 1u);
    REQUIRE_EQ(counters.read, 3u);
    REQUIRE_EQ(counters.zero, 1u);
    REQUIRE_EQ(counters.unmap, 1u);

    auto close_write = myrd::proxy::write_wire(
        mapping.first(myrd::proxy::kHeaderSize), 0,
        myrd::proxy::CloseRequest{myrd::proxy::kReqClose});
    REQUIRE(close_write.ok());
    REQUIRE(SetEvent(request_event) != FALSE);
    REQUIRE(wait_for_state(session, SessionState::closing));
    REQUIRE(session.stop().ok());
    REQUIRE(session.snapshot().state == SessionState::closed);
}

TEST_CASE(m1_session_start_stage_failures_are_reported_and_joined) {
    for (const auto failing_stage : {
             SessionStartStage::cancel_monitor,
             SessionStartStage::request_worker,
             SessionStartStage::ready_publish}) {
        myrd::MemoryBlockDevice device(kDeviceSize, 512);
        myrd::proxy::RequestProcessor processor(device);
        auto names = make_names();
        auto channel = myrd::proxy::SharedMemoryChannel::create(names);
        REQUIRE(channel.ok());

        myrd::proxy::ShmProxySessionHooks hooks;
        hooks.before_start_stage = [failing_stage](SessionStartStage stage) {
            if (stage == failing_stage) {
                return Result<void>::failure(injected_error(L"session_start"));
            }
            return Result<void>::success();
        };
        {
            myrd::proxy::ShmProxySession session(
                std::move(channel.value()), processor, {}, std::move(hooks));
            auto started = std::async(std::launch::async, [&session] {
                return session.start();
            });
            REQUIRE(started.wait_for(2s) == std::future_status::ready);
            REQUIRE(started.get().has_error());
            REQUIRE(session.snapshot().state == SessionState::failed);
            REQUIRE(session.stop().ok());
        }
        REQUIRE(named_objects_are_absent(names));
    }
}

TEST_CASE(m1_session_signal_failure_is_fatal_and_diagnostics_are_bounded) {
    myrd::MemoryBlockDevice device(kDeviceSize, 512);
    myrd::proxy::RequestProcessor processor(device);
    auto names = make_names();
    auto channel = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(channel.ok());
    auto mapping = channel.value().mapping();
    const HANDLE request_event = channel.value().request_event();

    myrd::proxy::ShmProxySessionHooks hooks;
    hooks.signal_event = [](HANDLE, std::wstring_view) {
        return Result<void>::failure(injected_error(L"SetEvent"));
    };
    myrd::proxy::ShmProxySession session(
        std::move(channel.value()), processor, {2s, 1}, std::move(hooks));
    REQUIRE(session.start().ok());

    auto write = myrd::proxy::write_wire(
        mapping.first(myrd::proxy::kHeaderSize), 0, myrd::proxy::kReqInfo);
    REQUIRE(write.ok());
    REQUIRE(SetEvent(request_event) != FALSE);
    REQUIRE(wait_for_state(session, SessionState::failed));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.last_error.has_value());
    REQUIRE_EQ(snapshot.requests.info, 1u);
    REQUIRE(session.stop().ok());
    REQUIRE(session.snapshot().state == SessionState::failed);
}

TEST_CASE(m1_session_idle_cancel_and_repeated_stop_are_bounded) {
    myrd::MemoryBlockDevice device(kDeviceSize, 512);
    myrd::proxy::RequestProcessor processor(device);
    auto names = make_names();
    {
        auto channel = myrd::proxy::SharedMemoryChannel::create(names);
        REQUIRE(channel.ok());
        myrd::proxy::ShmProxySession session(std::move(channel.value()), processor);
        REQUIRE(session.start().ok());
        const auto begin = std::chrono::steady_clock::now();
        REQUIRE(session.cancel().ok());
        REQUIRE(session.join().ok());
        REQUIRE(std::chrono::steady_clock::now() - begin < 2s);
        REQUIRE(session.snapshot().state == SessionState::closed);
        REQUIRE(session.stop().ok());
    }
    REQUIRE(named_objects_are_absent(names));
}
