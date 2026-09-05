#include "tests/support/test_framework.h"

#include "src/common/operation_context.h"
#include "src/control/authorization.h"
#include "src/control/ipc_codec.h"
#include "src/control/public_pipe_client.h"
#include "src/control/public_pipe_server.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <stop_token>
#include <string>
#include <optional>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::OperationContext bounded_context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

std::wstring current_process_sid_string() {
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD bytes = 0;
    REQUIRE(!GetTokenInformation(token, TokenUser, nullptr, 0, &bytes));
    REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::byte> buffer(bytes);
    REQUIRE(GetTokenInformation(
        token, TokenUser, buffer.data(), bytes, &bytes));
    REQUIRE(CloseHandle(token));
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR raw = nullptr;
    REQUIRE(ConvertSidToStringSidW(user->User.Sid, &raw));
    std::wstring value(raw);
    LocalFree(raw);
    return value;
}

HANDLE create_non_admin_impersonation_token() {
    HANDLE process_token = nullptr;
    REQUIRE(OpenProcessToken(
        GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE,
        &process_token));
    std::byte admin_storage[SECURITY_MAX_SID_SIZE]{};
    DWORD admin_bytes = sizeof(admin_storage);
    REQUIRE(CreateWellKnownSid(
        WinBuiltinAdministratorsSid, nullptr, admin_storage, &admin_bytes));
    SID_AND_ATTRIBUTES disabled_admin{
        static_cast<PSID>(static_cast<void*>(admin_storage)), 0};
    HANDLE restricted = nullptr;
    REQUIRE(CreateRestrictedToken(
        process_token, DISABLE_MAX_PRIVILEGE, 1, &disabled_admin,
        0, nullptr, 0, nullptr, &restricted));
    REQUIRE(CloseHandle(process_token));

    HANDLE impersonation = nullptr;
    REQUIRE(DuplicateTokenEx(
        restricted, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
        SecurityImpersonation, TokenImpersonation, &impersonation));
    REQUIRE(CloseHandle(restricted));
    return impersonation;
}

bool current_process_is_enabled_admin() {
    std::byte admin_storage[SECURITY_MAX_SID_SIZE]{};
    DWORD admin_bytes = sizeof(admin_storage);
    REQUIRE(CreateWellKnownSid(
        WinBuiltinAdministratorsSid, nullptr, admin_storage, &admin_bytes));
    BOOL member = FALSE;
    REQUIRE(CheckTokenMembership(nullptr, admin_storage, &member));
    return member != FALSE;
}

class RawHandle final {
public:
    explicit RawHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~RawHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    RawHandle(const RawHandle&) = delete;
    RawHandle& operator=(const RawHandle&) = delete;
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

RawHandle open_raw_client() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE handle = CreateFileW(
            myrd::ipc::kPublicControlPipeName.data(),
            static_cast<DWORD>(myrd::ipc::kPublicPipeClientAccessMask),
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) return RawHandle(handle);
        REQUIRE(GetLastError() == ERROR_PIPE_BUSY
                || GetLastError() == ERROR_FILE_NOT_FOUND);
        static_cast<void>(WaitNamedPipeW(
            myrd::ipc::kPublicControlPipeName.data(), 50));
    }
    REQUIRE(false);
    return RawHandle{};
}

void write_chunks(HANDLE handle,
                  std::span<const std::byte> bytes,
                  std::size_t chunk_size) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>((std::min)(
            chunk_size, bytes.size() - offset));
        DWORD written = 0;
        REQUIRE(WriteFile(handle, bytes.data() + offset, count, &written, nullptr));
        REQUIRE(written != 0);
        offset += written;
    }
}

myrd::ipc::PublicPipeHandler echo_handler() {
    return [](const myrd::OperationContext&,
              const myrd::ipc::CallerIdentity&,
              const myrd::ipc::Frame& request)
        -> myrd::Result<myrd::ipc::Frame> {
        return myrd::Result<myrd::ipc::Frame>::success(myrd::ipc::Frame{
            request.protocol_major,
            request.protocol_minor,
            myrd::ipc::MessageType::response,
            request.flags,
            request.request_id,
            request.operation_id,
            request.command_id,
            request.payload});
    };
}

void report_server_start_error(
    const myrd::Result<std::unique_ptr<myrd::ipc::PublicPipeServer>>& result) {
    if (result.ok()) return;
    std::wcerr << L"public pipe start error: operation="
               << result.error().operation
               << L" native=" << result.error().native_code
               << L" code=" << result.error().code
               << L" message=" << result.error().message << L'\n';
}

} // namespace

TEST_CASE(public_pipe_exact_name_and_loopback_preserve_correlation) {
    REQUIRE_EQ(std::wstring(myrd::ipc::kPublicControlPipeName),
               std::wstring(L"\\\\.\\pipe\\MyRamDisk.Control.v1"));

    auto server = myrd::ipc::PublicPipeServer::start(echo_handler());
    report_server_start_error(server);
    REQUIRE(server.ok());

    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());

    const auto request_id = uuid("00112233-4455-6677-8899-aabbccddeeff");
    const auto operation_id = uuid("10213243-5465-7687-98a9-bacbdcedfe0f");
    myrd::ipc::Frame request{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::request,
        0x11223344U,
        request_id,
        operation_id,
        static_cast<std::uint32_t>(myrd::ipc::CommandId::status),
        {std::byte{0xaa}, std::byte{0xbb}}};
    auto response = client.value()->request(bounded_context(), request);
    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().message_type, myrd::ipc::MessageType::response);
    REQUIRE_EQ(response.value().request_id, request_id);
    REQUIRE_EQ(response.value().operation_id, operation_id);
    REQUIRE_EQ(response.value().command_id, request.command_id);
    REQUIRE_EQ(response.value().flags, request.flags);
    REQUIRE_EQ(response.value().payload, request.payload);

    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_start_rejects_a_precreated_control_endpoint) {
    RawHandle squatter(CreateNamedPipeW(
        myrd::ipc::kPublicControlPipeName.data(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        static_cast<DWORD>(myrd::ipc::kPublicPipeModeFlags),
        static_cast<DWORD>(myrd::ipc::kMaximumPublicPipeInstances),
        4096, 4096, 0, nullptr));
    REQUIRE(squatter.get() != INVALID_HANDLE_VALUE);

    auto server = myrd::ipc::PublicPipeServer::start(echo_handler());
    REQUIRE(!server.ok());
}

TEST_CASE(public_pipe_passes_real_identity_not_caller_supplied_owner_sid) {
    std::mutex identity_mutex;
    std::optional<myrd::ipc::CallerIdentity> observed;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity& caller,
            const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            {
                std::lock_guard lock(identity_mutex);
                observed = caller;
            }
            return echo_handler()(context, caller, request);
        });
    report_server_start_error(server);
    REQUIRE(server.ok());
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());

    myrd::control::DiskCreateCommand create{
        L"identity-test", L"D:\\identity-test", 64ULL * 1024ULL * 1024ULL,
        L"R:", std::wstring(L"S-1-5-18"), true,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    auto payload = myrd::ipc::encode_command_payload(
        myrd::control::Command{std::move(create)});
    REQUIRE(payload.ok());
    myrd::ipc::Frame request{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("10112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        static_cast<std::uint32_t>(payload.value().command_id),
        payload.move_value().payload};
    REQUIRE(client.value()->request(bounded_context(), request).ok());

    {
        std::lock_guard lock(identity_mutex);
        REQUIRE(observed.has_value());
        REQUIRE_EQ(observed->primary_sid, current_process_sid_string());
        REQUIRE(observed->primary_sid != L"S-1-5-18");
    }
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_recaptures_identity_after_same_connection_token_change) {
    std::mutex observed_mutex;
    std::vector<myrd::ipc::CallerIdentity> observed;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity& caller,
            const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            {
                std::lock_guard lock(observed_mutex);
                observed.push_back(caller);
            }
            return echo_handler()(context, caller, request);
        });
    REQUIRE(server.ok());
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());

    auto make_request = [](std::string_view request_id) {
        return myrd::ipc::Frame{
            1, 0, myrd::ipc::MessageType::request, 0, uuid(request_id),
            uuid("00000000-0000-0000-0000-000000000000"),
            static_cast<std::uint32_t>(myrd::ipc::CommandId::driver_status), {}};
    };
    auto first = make_request("20112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(client.value()->request(bounded_context(), first).ok());

    HANDLE restricted = create_non_admin_impersonation_token();
    REQUIRE(SetThreadToken(nullptr, restricted));
    auto second = make_request("30112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(client.value()->request(bounded_context(), second).ok());
    REQUIRE(RevertToSelf());
    REQUIRE(CloseHandle(restricted));

    auto third = make_request("40112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(client.value()->request(bounded_context(), third).ok());
    REQUIRE(server.value()->stop(bounded_context()).ok());

    std::lock_guard lock(observed_mutex);
    REQUIRE(observed.size() == 3);
    const bool process_admin = current_process_is_enabled_admin();
    REQUIRE(observed[0].administrator == process_admin);
    REQUIRE(!observed[1].administrator);
    REQUIRE(observed[2].administrator == process_admin);
    REQUIRE_EQ(observed[0].primary_sid, current_process_sid_string());
    REQUIRE_EQ(observed[1].primary_sid, current_process_sid_string());
    REQUIRE_EQ(observed[2].primary_sid, current_process_sid_string());
}

TEST_CASE(public_pipe_allows_local_restricted_user_and_captures_non_admin) {
    std::mutex identity_mutex;
    std::optional<myrd::ipc::CallerIdentity> observed;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity& caller,
            const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            {
                std::lock_guard lock(identity_mutex);
                observed = caller;
            }
            return echo_handler()(context, caller, request);
        });
    REQUIRE(server.ok());

    HANDLE restricted = create_non_admin_impersonation_token();
    REQUIRE(SetThreadToken(nullptr, restricted));
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());
    myrd::ipc::Frame request{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("50112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        static_cast<std::uint32_t>(myrd::ipc::CommandId::driver_status), {}};
    REQUIRE(client.value()->request(bounded_context(), request).ok());
    REQUIRE(RevertToSelf());
    REQUIRE(CloseHandle(restricted));

    {
        std::lock_guard lock(identity_mutex);
        REQUIRE(observed.has_value());
        REQUIRE(observed->authenticated);
        REQUIRE(!observed->administrator);
        REQUIRE(!myrd::ipc::authorize_public_command(
            static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_create),
            observed.value(), std::nullopt).allowed);
        REQUIRE_EQ(observed->primary_sid, current_process_sid_string());
    }
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_accepts_chunked_frame_and_recovers_after_partial_disconnect) {
    auto server = myrd::ipc::PublicPipeServer::start(echo_handler());
    REQUIRE(server.ok());

    {
        auto disconnected = open_raw_client();
        const std::array<std::byte, 7> prefix{{
            std::byte{0x4d}, std::byte{0x59}, std::byte{0x52}, std::byte{0x44},
            std::byte{1}, std::byte{0}, std::byte{0}}};
        write_chunks(disconnected.get(), prefix, 2);
    }

    auto raw = open_raw_client();
    const auto request_id = uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    myrd::ipc::Frame request{
        1, 0, myrd::ipc::MessageType::request, 7, request_id,
        uuid("00000000-0000-0000-0000-000000000000"),
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_list),
        {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}};
    auto encoded = myrd::ipc::encode_frame(request);
    REQUIRE(encoded.ok());
    write_chunks(raw.get(), encoded.value(), 3);

    std::vector<std::byte> response_bytes(myrd::ipc::kFrameHeaderBytes);
    std::size_t received = 0;
    while (received < response_bytes.size()) {
        DWORD count = 0;
        REQUIRE(ReadFile(raw.get(), response_bytes.data() + received,
                         static_cast<DWORD>(response_bytes.size() - received),
                         &count, nullptr));
        REQUIRE(count != 0);
        received += count;
    }
    const auto payload_length = static_cast<std::uint32_t>(
        static_cast<unsigned>(response_bytes[14])
        | (static_cast<unsigned>(response_bytes[15]) << 8U)
        | (static_cast<unsigned>(response_bytes[16]) << 16U)
        | (static_cast<unsigned>(response_bytes[17]) << 24U));
    response_bytes.resize(response_bytes.size() + payload_length);
    while (received < response_bytes.size()) {
        DWORD count = 0;
        REQUIRE(ReadFile(raw.get(), response_bytes.data() + received,
                         static_cast<DWORD>(response_bytes.size() - received),
                         &count, nullptr));
        REQUIRE(count != 0);
        received += count;
    }
    auto decoded = myrd::ipc::decode_frame(response_bytes);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().request_id, request_id);
    REQUIRE_EQ(decoded.value().payload, request.payload);
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_rejects_mismatched_response_correlation) {
    auto server = myrd::ipc::PublicPipeServer::start(
        [](const myrd::OperationContext&,
           const myrd::ipc::CallerIdentity&,
           const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            auto wrong_id = uuid("ffffffff-ffff-4fff-8fff-ffffffffffff");
            return myrd::Result<myrd::ipc::Frame>::success(myrd::ipc::Frame{
                1, 0, myrd::ipc::MessageType::response, 0, wrong_id,
                request.operation_id, request.command_id, {}});
        });
    REQUIRE(server.ok());
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());
    myrd::ipc::Frame request{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"), 1, {}};
    REQUIRE(!client.value()->request(bounded_context(), request).ok());
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_serves_four_clients_concurrently_with_bounded_instances) {
    constexpr std::size_t client_count = 4;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity& caller,
            const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            std::unique_lock lock(mutex);
            ++entered;
            cv.notify_all();
            if (!cv.wait_until(
                    lock,
                    std::chrono::steady_clock::now() + std::chrono::seconds(3),
                    [&] { return entered == client_count; })) {
                return myrd::Result<myrd::ipc::Frame>::failure(myrd::Error{
                    myrd::ErrorDomain::protocol,
                    static_cast<std::uint32_t>(
                        myrd::OperationErrorCode::timed_out),
                    L"concurrency-test", L"Clients were serialized"});
            }
            lock.unlock();
            return echo_handler()(context, caller, request);
        });
    REQUIRE(server.ok());

    std::array<bool, client_count> succeeded{};
    std::vector<std::jthread> clients;
    clients.reserve(client_count);
    for (std::size_t index = 0; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
            if (!client.ok()) return;
            myrd::ipc::Frame request{
                1, 0, myrd::ipc::MessageType::request, 0,
                uuid("00112233-4455-6677-8899-aabbccddeeff"),
                uuid("00000000-0000-0000-0000-000000000000"),
                static_cast<std::uint32_t>(index + 1), {}};
            succeeded[index] =
                client.value()->request(bounded_context(), request).ok();
        });
    }
    for (auto& client : clients) client.join();
    for (const auto result : succeeded) REQUIRE(result);
    REQUIRE_EQ(entered, client_count);
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_connect_observes_cancellation_and_deadline) {
    std::stop_source cancelled;
    cancelled.request_stop();
    auto cancelled_result = myrd::ipc::PipeTransport::connect(
        myrd::OperationContext{
            cancelled.get_token(),
            std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    REQUIRE(!cancelled_result.ok());
    REQUIRE_EQ(cancelled_result.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::cancelled));

    const auto started = std::chrono::steady_clock::now();
    auto timed_out = myrd::ipc::PipeTransport::connect(
        myrd::OperationContext{
            {}, started + std::chrono::milliseconds(80)});
    REQUIRE(!timed_out.ok());
    REQUIRE_EQ(timed_out.error().code,
               static_cast<std::uint32_t>(
                   myrd::OperationErrorCode::timed_out));
    REQUIRE(std::chrono::steady_clock::now() - started
            < std::chrono::seconds(2));
}

TEST_CASE(public_pipe_server_shutdown_cancels_active_handler_and_client_read) {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity&,
            const myrd::ipc::Frame&)
            -> myrd::Result<myrd::ipc::Frame> {
            {
                std::lock_guard lock(mutex);
                entered = true;
            }
            cv.notify_all();
            std::stop_callback callback(
                context.stop_token, [&] { cv.notify_all(); });
            std::unique_lock lock(mutex);
            cv.wait_until(
                lock, context.deadline,
                [&] { return context.stop_token.stop_requested(); });
            return myrd::Result<myrd::ipc::Frame>::failure(myrd::Error{
                myrd::ErrorDomain::protocol,
                static_cast<std::uint32_t>(
                    myrd::OperationErrorCode::cancelled),
                L"shutdown-test", L"Handler cancelled"});
        });
    REQUIRE(server.ok());
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());

    bool client_failed = false;
    std::jthread requester([&] {
        myrd::ipc::Frame request{
            1, 0, myrd::ipc::MessageType::request, 0,
            uuid("00112233-4455-6677-8899-aabbccddeeff"),
            uuid("00000000-0000-0000-0000-000000000000"), 1, {}};
        client_failed =
            !client.value()->request(bounded_context(), request).ok();
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_until(
            lock,
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            [&] { return entered; }));
    }
    REQUIRE(server.value()->stop(bounded_context()).ok());
    requester.join();
    REQUIRE(client_failed);
}

TEST_CASE(operation_wait_reconnects_once_and_each_long_poll_is_at_most_30_seconds) {
    std::atomic_uint calls{};
    std::atomic_bool bounded{true};
    std::mutex identity_mutex;
    std::vector<std::wstring> caller_sids;
    auto server = myrd::ipc::PublicPipeServer::start(
        [&](const myrd::OperationContext& context,
            const myrd::ipc::CallerIdentity& caller,
            const myrd::ipc::Frame& request)
            -> myrd::Result<myrd::ipc::Frame> {
            {
                std::lock_guard lock(identity_mutex);
                caller_sids.push_back(caller.primary_sid);
            }
            bounded.store(
                context.deadline
                    <= std::chrono::steady_clock::now()
                        + std::chrono::seconds(31),
                std::memory_order_release);
            if (calls.fetch_add(1, std::memory_order_acq_rel) == 0) {
                return myrd::Result<myrd::ipc::Frame>::failure(myrd::Error{
                    myrd::ErrorDomain::protocol,
                    static_cast<std::uint32_t>(
                        myrd::OperationErrorCode::session_closed),
                    L"operation-wait-test", L"Simulated reconnect"});
            }
            return echo_handler()(context, caller, request);
        });
    REQUIRE(server.ok());
    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());
    myrd::ipc::Frame request{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
        static_cast<std::uint32_t>(myrd::ipc::CommandId::operation_wait), {}};
    auto response = client.value()->wait_operation(
        myrd::OperationContext{
            {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)},
        request);
    REQUIRE(response.ok());
    REQUIRE_EQ(calls.load(std::memory_order_acquire), 2U);
    REQUIRE(bounded.load(std::memory_order_acquire));
    {
        std::lock_guard lock(identity_mutex);
        REQUIRE(caller_sids.size() == 2);
        REQUIRE_EQ(caller_sids[0], current_process_sid_string());
        REQUIRE_EQ(caller_sids[1], current_process_sid_string());
    }
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_creation_contract_is_byte_mode_bounded_and_remote_rejecting) {
    REQUIRE_EQ(myrd::ipc::kPublicPipeModeFlags,
               static_cast<std::uint32_t>(
                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                   | PIPE_REJECT_REMOTE_CLIENTS));
    REQUIRE((myrd::ipc::kPublicPipeModeFlags
             & PIPE_REJECT_REMOTE_CLIENTS) != 0);
    REQUIRE_EQ(myrd::ipc::kMaximumPublicPipeInstances, 8U);
}

TEST_CASE(public_pipe_rejects_oversized_request_before_payload_and_recovers) {
    auto server = myrd::ipc::PublicPipeServer::start(echo_handler());
    REQUIRE(server.ok());
    {
        auto raw = open_raw_client();
        myrd::ipc::Frame request{
            1, 0, myrd::ipc::MessageType::request, 0,
            uuid("00112233-4455-6677-8899-aabbccddeeff"),
            uuid("00000000-0000-0000-0000-000000000000"), 1, {}};
        auto header = myrd::ipc::encode_frame(request);
        REQUIRE(header.ok());
        const auto oversized = myrd::ipc::kMaxPayloadBytes + 1U;
        header.value()[14] = static_cast<std::byte>(oversized & 0xffU);
        header.value()[15] = static_cast<std::byte>((oversized >> 8U) & 0xffU);
        header.value()[16] = static_cast<std::byte>((oversized >> 16U) & 0xffU);
        header.value()[17] = static_cast<std::byte>((oversized >> 24U) & 0xffU);
        write_chunks(raw.get(), header.value(), header.value().size());
        std::byte ignored{};
        DWORD read = 0;
        REQUIRE(!ReadFile(raw.get(), &ignored, 1, &read, nullptr));
        REQUIRE(GetLastError() == ERROR_BROKEN_PIPE
                || GetLastError() == ERROR_PIPE_NOT_CONNECTED);
    }

    auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
    REQUIRE(client.ok());
    myrd::ipc::Frame valid{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"), 1, {}};
    REQUIRE(client.value()->request(bounded_context(), valid).ok());
    REQUIRE(server.value()->stop(bounded_context()).ok());
}

TEST_CASE(public_pipe_repeated_start_stop_does_not_leak_process_handles) {
    {
        auto warmup = myrd::ipc::PublicPipeServer::start(echo_handler());
        REQUIRE(warmup.ok());
        REQUIRE(warmup.value()->stop(bounded_context()).ok());
    }
    DWORD before = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (unsigned iteration = 0; iteration < 25; ++iteration) {
        auto server = myrd::ipc::PublicPipeServer::start(echo_handler());
        REQUIRE(server.ok());
        auto client = myrd::ipc::PublicPipeClient::connect(bounded_context());
        REQUIRE(client.ok());
        myrd::ipc::Frame request{
            1, 0, myrd::ipc::MessageType::request, 0,
            uuid("00112233-4455-6677-8899-aabbccddeeff"),
            uuid("00000000-0000-0000-0000-000000000000"), 1, {}};
        REQUIRE(client.value()->request(bounded_context(), request).ok());
        REQUIRE(server.value()->stop(bounded_context()).ok());
    }
    DWORD after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
    REQUIRE(after <= before + 4U);
}
