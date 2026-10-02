#include "src/host/m5_worker_private_pipe.h"

#include "src/common/unique_handle.h"
#include "src/host/m5_worker_bytes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Sddl.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace myrd::m5 {
namespace {

// "M5LP" on the wire, so a launch spec and a worker message can never be
// mistaken for one another even if a handle is crossed by mistake.
constexpr std::uint32_t kLaunchSpecMagic = 0x504C354DU;
constexpr std::uint16_t kLaunchSpecVersion = 1;

constexpr std::size_t kFrameHeaderBytes = 4;
// Comfortably above one encoded message and one launch spec.
constexpr std::uint32_t kPipeBufferBytes = 128U * 1024U;

Error pipe_error(const std::uint32_t code, const wchar_t* operation,
                 const wchar_t* message, const std::uint32_t native_code = 0) {
    return {native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32, code,
            operation, message, native_code};
}

Error win32_error(const wchar_t* operation, const wchar_t* message) {
    const auto native = GetLastError();
    return pipe_error(static_cast<std::uint32_t>(OperationErrorCode::timed_out),
                      operation, message, native);
}

// The private pipe admits the service that created it, the machine's
// administrators for the same reason they can already debug the service, and
// nothing else.  The public pipe's descriptor is deliberately *not* reused: it
// grants the interactive user, which is exactly what must not be able to reach
// a worker.
//
// The service's own identity is resolved at runtime rather than hardcoded to
// SYSTEM, because a Windows service is not obliged to run as SYSTEM and a
// hardcoded DACL would make the worker unable to connect to its own service.
// Naming the creator's account is also what lets the channel be exercised
// without elevation -- and it grants the interactive user nothing in production,
// where the creator is the service account.
Result<std::wstring> private_pipe_sddl() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Result<std::wstring>::failure(
            win32_error(L"private_pipe_sddl", L"OpenProcessToken failed"));
    }
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0, &size));
    std::vector<std::byte> buffer(size);
    const bool read =
        size != 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
    static_cast<void>(CloseHandle(token));
    if (!read) {
        return Result<std::wstring>::failure(
            win32_error(L"private_pipe_sddl", L"reading the token user failed"));
    }
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
            &sid_text)) {
        return Result<std::wstring>::failure(
            win32_error(L"private_pipe_sddl", L"formatting the service sid failed"));
    }
    std::wstring sddl = L"D:P(A;;GA;;;";
    sddl += sid_text;
    sddl += L")(A;;GA;;;SY)(A;;GA;;;BA)";
    static_cast<void>(LocalFree(sid_text));
    return Result<std::wstring>::success(std::move(sddl));
}


// Milliseconds left before the context's deadline, floored at zero so an
// expired context produces an immediate, deterministic timeout rather than a
// wait that can never succeed.
DWORD remaining_ms(const OperationContext& context) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= context.deadline) return 0;
    const auto remaining = context.deadline - now;
    const auto whole =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    auto count = whole.count();
    if (whole < remaining) ++count;
    return static_cast<DWORD>((std::min)(
        count, static_cast<decltype(count)>(
                   (std::numeric_limits<DWORD>::max)() - 1U)));
}

// One overlapped transfer, bounded by the operation context.  The pipe handle
// is created with FILE_FLAG_OVERLAPPED, so every transfer goes through an event
// even when it completes immediately -- mixing synchronous calls onto an
// overlapped handle is the classic way to get a "completed" report with no data.
Result<void> overlapped_io(void* native_handle, const OperationContext& context,
                           void* buffer, const DWORD bytes, const bool writing,
                           const wchar_t* operation) {
    HANDLE handle = static_cast<HANDLE>(native_handle);
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return Result<void>::failure(win32_error(operation, L"CreateEventW failed"));

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL started =
        writing ? WriteFile(handle, buffer, bytes, nullptr, &overlapped)
                : ReadFile(handle, buffer, bytes, nullptr, &overlapped);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        const auto error = win32_error(operation, L"pipe transfer failed to start");
        static_cast<void>(CancelIoEx(handle, &overlapped));
        return Result<void>::failure(error);
    }
    const auto wait = WaitForSingleObject(event.get(), remaining_ms(context));
    if (wait != WAIT_OBJECT_0) {
        static_cast<void>(CancelIoEx(handle, &overlapped));
        DWORD ignored = 0;
        static_cast<void>(GetOverlappedResult(handle, &overlapped, &ignored, TRUE));
        if (wait == WAIT_TIMEOUT) {
            return Result<void>::failure(pipe_error(
                static_cast<std::uint32_t>(OperationErrorCode::timed_out),
                operation, L"pipe transfer exceeded its bounded wait"));
        }
        return Result<void>::failure(win32_error(operation, L"waiting for pipe I/O failed"));
    }
    if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
        return Result<void>::failure(win32_error(operation, L"pipe transfer failed"));
    }
    if (transferred != bytes) {
        return Result<void>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            operation, L"pipe transfer returned fewer bytes than requested"));
    }
    return Result<void>::success();
}

Result<void> write_bytes(void* handle, const OperationContext& context,
                         const std::span<std::byte> bytes,
                         const wchar_t* operation) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        auto written = overlapped_io(handle, context, bytes.data() + offset,
                                     chunk, true, operation);
        if (!written.ok()) return written;
        offset += chunk;
    }
    return Result<void>::success();
}

// The service may write the spec a moment before the child inherits the handle,
// and the child may connect a moment before the service finishes creating the
// named-pipe instance, so both sides retry to their deadline rather than
// treating "not yet" as "never".
Result<void> wait_for_pipe_name(const OperationContext& context,
                                const std::wstring& name) {
    for (;;) {
        if (WaitNamedPipeW(name.c_str(), remaining_ms(context))) {
            return Result<void>::success();
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            return Result<void>::failure(pipe_error(
                static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
                L"WorkerPrivatePipeClient::connect",
                L"waiting for the private worker pipe failed", error));
        }
        if (remaining_ms(context) == 0) {
            return Result<void>::failure(pipe_error(
                static_cast<std::uint32_t>(OperationErrorCode::timed_out),
                L"WorkerPrivatePipeClient::connect",
                L"no private worker pipe answered this session's name", error));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

Result<void> read_exact(void* handle, const OperationContext& context,
                        const std::span<std::byte> bytes,
                        const wchar_t* operation) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        auto read = overlapped_io(handle, context, bytes.data() + offset, chunk,
                                  false, operation);
        if (!read.ok()) return read;
        offset += chunk;
    }
    return Result<void>::success();
}

Result<WorkerLaunchSpec> decode_launch_spec_body(
    detail::WorkerBytesReader& reader) {
    auto profile_id = reader.uuid();
    if (!profile_id.has_value()) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_launch_spec",
            L"launch spec profile id is not a valid uuid"));
    }
    WorkerLaunchSpec spec{*profile_id, reader.u64()};
    spec.bundle_path = reader.text();
    spec.logical_size_bytes = reader.u64();
    spec.mount_target = reader.text();
    spec.session_generation = reader.u64();
    spec.private_pipe_name = reader.text();
    spec.session_nonce = reader.u64();
    const auto secret = reader.raw(spec.startup_secret.bytes.size());
    if (!reader.ok() || !reader.exhausted()) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_launch_spec", L"launch spec body is truncated or has trailing bytes"));
    }
    std::memcpy(spec.startup_secret.bytes.data(), secret.data(),
                spec.startup_secret.bytes.size());
    return Result<WorkerLaunchSpec>::success(std::move(spec));
}

} // namespace

Result<StartupSecret> StartupSecret::generate() {
    StartupSecret secret{};
    const auto status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(secret.bytes.data()),
        static_cast<ULONG>(secret.bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        // Fail closed rather than fall back to a weaker source: a predictable
        // startup secret is worse than a launch that does not happen.
        return Result<StartupSecret>::failure(
            Error{ErrorDomain::internal,
                  static_cast<std::uint32_t>(
                      OperationErrorCode::internal_invariant),
                  L"StartupSecret::generate",
                  L"the system random generator refused a worker startup secret",
                  static_cast<std::uint32_t>(status)});
    }
    return Result<StartupSecret>::success(secret);
}

Result<std::vector<std::byte>> encode_launch_spec(const WorkerLaunchSpec& spec) {
    detail::WorkerBytesWriter writer;
    writer.u32(kLaunchSpecMagic);
    writer.u16(kLaunchSpecVersion);
    writer.u16(0);
    writer.uuid(spec.profile_id);
    writer.u64(spec.profile_revision);
    writer.text(spec.bundle_path);
    writer.u64(spec.logical_size_bytes);
    writer.text(spec.mount_target);
    writer.u64(spec.session_generation);
    writer.text(spec.private_pipe_name);
    writer.u64(spec.session_nonce);
    writer.raw(spec.startup_secret.bytes);
    return Result<std::vector<std::byte>>::success(writer.take());
}

Result<WorkerLaunchSpec> decode_launch_spec(
    const std::span<const std::byte> bytes) {
    if (bytes.size() > kPipeBufferBytes) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_launch_spec", L"launch spec exceeds the maximum size"));
    }
    detail::WorkerBytesReader reader(bytes);
    if (reader.u32() != kLaunchSpecMagic || reader.u16() != kLaunchSpecVersion ||
        reader.u16() != 0) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_launch_spec", L"launch spec header is not this protocol"));
    }
    return decode_launch_spec_body(reader);
}

std::wstring private_pipe_name(const product::Uuid& profile_id,
                               const std::uint64_t session_generation) {
    // The profile id is already canonical lowercase hex, so it needs no
    // escaping, and the session generation is what makes two sessions of the
    // same profile distinct names rather than two attempts at one name.
    std::wstring name = L"\\\\.\\pipe\\MyRamDisk.M5.Worker.";
    for (const char unit : profile_id.to_string()) {
        name.push_back(static_cast<wchar_t>(unit));
    }
    name.push_back(L'.');
    name += std::to_wstring(session_generation);
    return name;
}

Result<StartupChannel> create_startup_channel() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, kPipeBufferBytes)) {
        return Result<StartupChannel>::failure(
            win32_error(L"create_startup_channel", L"CreatePipe failed"));
    }
    // Only the child's end is inherited.  If the write end were inheritable the
    // child would hold a writer open and its own read would never see EOF.
    if (!SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0)) {
        const auto error =
            win32_error(L"create_startup_channel", L"SetHandleInformation failed");
        static_cast<void>(CloseHandle(read_end));
        static_cast<void>(CloseHandle(write_end));
        return Result<StartupChannel>::failure(error);
    }
    return Result<StartupChannel>::success(
        StartupChannel{read_end, write_end});
}

Result<void> write_launch_spec(StartupChannel& channel,
                               const WorkerLaunchSpec& spec) {
    if (channel.parent_write_handle == nullptr) {
        return Result<void>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"write_launch_spec", L"the startup channel was already closed"));
    }
    auto encoded = encode_launch_spec(spec);
    if (!encoded.ok()) {
        close_startup_handle(channel.parent_write_handle);
        channel.parent_write_handle = nullptr;
        return Result<void>::failure(encoded.error());
    }
    // The startup channel carries one message and then closes, so it needs no
    // framing and no operation context: a failure here fails the launch.
    OperationContext context{};
    auto written = write_bytes(channel.parent_write_handle, context,
                               encoded.value(), L"write_launch_spec");
    close_startup_handle(channel.parent_write_handle);
    channel.parent_write_handle = nullptr;
    return written;
}

Result<WorkerLaunchSpec> read_launch_spec(void* inherited_handle) {
    if (inherited_handle == nullptr) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"read_launch_spec", L"no startup handle was inherited"));
    }
    std::array<std::byte, 4096> buffer{};
    DWORD total = 0;
    // The spec is a single bounded write followed by EOF, so one read that
    // reports a short count *is* the whole message.
    if (!ReadFile(static_cast<HANDLE>(inherited_handle), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &total, nullptr)) {
        return Result<WorkerLaunchSpec>::failure(
            win32_error(L"read_launch_spec", L"ReadFile failed on the startup channel"));
    }
    if (total == 0 || total > buffer.size()) {
        return Result<WorkerLaunchSpec>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"read_launch_spec", L"the startup channel carried no usable launch spec"));
    }
    return decode_launch_spec(
        std::span<const std::byte>(buffer.data(), total));
}

void close_startup_handle(void* handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle)));
    }
}

// ---------------------------------------------------------------------------
// WorkerPipeChannel
// ---------------------------------------------------------------------------

WorkerPipeChannel::WorkerPipeChannel(void* handle) noexcept : handle_(handle) {}

WorkerPipeChannel::~WorkerPipeChannel() noexcept { close(); }

WorkerPipeChannel::WorkerPipeChannel(WorkerPipeChannel&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

WorkerPipeChannel& WorkerPipeChannel::operator=(WorkerPipeChannel&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void WorkerPipeChannel::close() noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(FlushFileBuffers(static_cast<HANDLE>(handle_)));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
        handle_ = nullptr;
    }
}

Result<void> WorkerPipeChannel::write(const OperationContext& context,
                                      const WorkerMessage& message) {
    auto encoded = encode_worker_message(message);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    std::vector<std::byte> frame;
    frame.reserve(kFrameHeaderBytes + encoded.value().size());
    const auto length = static_cast<std::uint32_t>(encoded.value().size());
    for (int shift = 0; shift < 32; shift += 8) {
        frame.push_back(static_cast<std::byte>((length >> shift) & 0xFFU));
    }
    frame.insert(frame.end(), encoded.value().begin(), encoded.value().end());
    return write_bytes(handle_, context, frame, L"WorkerPipeChannel::write");
}

Result<WorkerMessage> WorkerPipeChannel::read(const OperationContext& context) {
    std::array<std::byte, kFrameHeaderBytes> header{};
    auto read = read_exact(handle_, context, header, L"WorkerPipeChannel::read");
    if (!read.ok()) return Result<WorkerMessage>::failure(read.error());
    std::uint32_t length = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        length |= static_cast<std::uint32_t>(
                      std::to_integer<std::uint8_t>(header[shift / 8]))
                  << shift;
    }
    // Bounded before allocation: a hostile or corrupt peer must not be able to
    // ask for an arbitrary buffer.
    if (length == 0 || length > kMaxWorkerMessageBytes) {
        return Result<WorkerMessage>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"WorkerPipeChannel::read", L"private frame length is out of range"));
    }
    std::vector<std::byte> body(length);
    read = read_exact(handle_, context, body, L"WorkerPipeChannel::read");
    if (!read.ok()) return Result<WorkerMessage>::failure(read.error());
    return decode_worker_message(body);
}

// ---------------------------------------------------------------------------
// WorkerPrivatePipeServer
// ---------------------------------------------------------------------------

WorkerPrivatePipeServer::WorkerPrivatePipeServer(void* handle) noexcept
    : handle_(handle) {}

WorkerPrivatePipeServer::~WorkerPrivatePipeServer() noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(DisconnectNamedPipe(static_cast<HANDLE>(handle_)));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
        handle_ = nullptr;
    }
}

Result<std::unique_ptr<WorkerPrivatePipeServer>> WorkerPrivatePipeServer::create(
    std::wstring pipe_name) {
    auto sddl = private_pipe_sddl();
    if (!sddl.ok()) {
        return Result<std::unique_ptr<WorkerPrivatePipeServer>>::failure(
            sddl.error());
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.value().c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return Result<std::unique_ptr<WorkerPrivatePipeServer>>::failure(
            win32_error(L"WorkerPrivatePipeServer::create",
                        L"the private pipe security descriptor was refused"));
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE handle = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, kPipeBufferBytes, kPipeBufferBytes, 0, &attributes);
    const auto error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<WorkerPrivatePipeServer>>::failure(
            pipe_error(static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
                       L"WorkerPrivatePipeServer::create",
                       L"a private worker pipe with this session's name already exists",
                       error));
    }
    return Result<std::unique_ptr<WorkerPrivatePipeServer>>::success(
        std::unique_ptr<WorkerPrivatePipeServer>(
            new WorkerPrivatePipeServer(handle)));
}

Result<std::unique_ptr<WorkerPipeChannel>>
WorkerPrivatePipeServer::accept_and_handshake(
    const OperationContext& context, const WorkerBinding& expected_binding,
    const std::uint64_t session_nonce, const StartupSecret& startup_secret) {
    HANDLE handle = static_cast<HANDLE>(handle_);
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(
            win32_error(L"accept_and_handshake", L"CreateEventW failed"));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (!ConnectNamedPipe(handle, &overlapped)) {
        const auto pending = GetLastError();
        if (pending == ERROR_PIPE_CONNECTED) {
            // A client that connected between CreateNamedPipeW and here never
            // raises the event, so this is success, not a wait.
        } else if (pending == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event.get(), remaining_ms(context)) !=
                WAIT_OBJECT_0) {
                static_cast<void>(CancelIoEx(handle, &overlapped));
                return Result<std::unique_ptr<WorkerPipeChannel>>::failure(
                    pipe_error(static_cast<std::uint32_t>(
                                   OperationErrorCode::timed_out),
                               L"accept_and_handshake",
                               L"no worker connected before the deadline"));
            }
        } else {
            return Result<std::unique_ptr<WorkerPipeChannel>>::failure(
                win32_error(L"accept_and_handshake", L"ConnectNamedPipe failed"));
        }
    }

    std::unique_ptr<WorkerPipeChannel> channel(new WorkerPipeChannel(handle_));
    handle_ = nullptr;  // the channel now owns it
    auto request = channel->read(context);
    if (!request.ok()) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(request.error());
    }
    const auto& message = request.value();
    if (message.kind != WorkerMessageKind::request ||
        message.command != WorkerCommand::handshake ||
        !message.handshake.has_value() || !message.binding.has_value()) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"accept_and_handshake",
            L"the first private message was not a handshake"));
    }
    // The nonce and the secret are checked before the binding is, because a peer
    // that cannot prove it read the startup channel has not earned an answer
    // about which worker it is.
    if (message.handshake->session_nonce != session_nonce) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"accept_and_handshake",
            L"handshake nonce is not this session's"));
    }
    if (!(message.handshake->startup_secret == startup_secret)) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"accept_and_handshake",
            L"handshake startup secret is not the one this session delivered"));
    }
    if (!(*message.binding == expected_binding)) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"accept_and_handshake",
            L"handshake binding is not the worker this session launched"));
    }

    WorkerMessage reply{};
    reply.kind = WorkerMessageKind::reply;
    reply.command = WorkerCommand::handshake;
    reply.sequence = 1;
    reply.binding = expected_binding;
    reply.handshake =
        WorkerHandshake{kWorkerProtocolMajor, kWorkerProtocolMinor,
                        session_nonce, startup_secret};
    auto written = channel->write(context, reply);
    if (!written.ok()) {
        return Result<std::unique_ptr<WorkerPipeChannel>>::failure(written.error());
    }
    return Result<std::unique_ptr<WorkerPipeChannel>>::success(
        std::move(channel));
}

// ---------------------------------------------------------------------------
// WorkerPrivatePipeClient
// ---------------------------------------------------------------------------

WorkerPrivatePipeClient::WorkerPrivatePipeClient(
    WorkerPipeChannel channel) noexcept
    : channel_(std::move(channel)) {}

WorkerPrivatePipeClient::~WorkerPrivatePipeClient() noexcept = default;

Result<std::unique_ptr<WorkerPrivatePipeClient>>
WorkerPrivatePipeClient::connect(const OperationContext& context,
                                 const std::wstring_view pipe_name) {
    const std::wstring terminated(pipe_name);
    auto available = wait_for_pipe_name(context, terminated);
    if (!available.ok()) {
        return Result<std::unique_ptr<WorkerPrivatePipeClient>>::failure(
            available.error());
    }
    HANDLE handle = CreateFileW(
        terminated.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<WorkerPrivatePipeClient>>::failure(
            win32_error(L"WorkerPrivatePipeClient::connect",
                        L"opening the private worker pipe failed"));
    }
    return Result<std::unique_ptr<WorkerPrivatePipeClient>>::success(
        std::unique_ptr<WorkerPrivatePipeClient>(
            new WorkerPrivatePipeClient(WorkerPipeChannel(handle))));
}

Result<void> WorkerPrivatePipeClient::handshake(
    const OperationContext& context, const WorkerBinding& self,
    const std::uint64_t session_nonce, const StartupSecret& startup_secret) {
    WorkerMessage request{};
    request.kind = WorkerMessageKind::request;
    request.command = WorkerCommand::handshake;
    request.sequence = 1;
    request.binding = self;
    request.handshake =
        WorkerHandshake{kWorkerProtocolMajor, kWorkerProtocolMinor,
                        session_nonce, startup_secret};
    auto written = write(context, request);
    if (!written.ok()) return written;

    auto reply = read(context);
    if (!reply.ok()) return Result<void>::failure(reply.error());
    const auto& message = reply.value();
    if (message.kind != WorkerMessageKind::reply ||
        message.command != WorkerCommand::handshake ||
        !message.handshake.has_value() || !message.binding.has_value()) {
        return Result<void>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"WorkerPrivatePipeClient::handshake",
            L"the service did not answer the handshake"));
    }
    if (message.handshake->session_nonce != session_nonce ||
        !(message.handshake->startup_secret == startup_secret)) {
        return Result<void>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"WorkerPrivatePipeClient::handshake",
            L"the handshake acknowledgement is not this session's"));
    }
    // A worker that accepted a binding other than its own would then be
    // answering for a profile it is not.
    if (!(*message.binding == self)) {
        return Result<void>::failure(pipe_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"WorkerPrivatePipeClient::handshake",
            L"the acknowledged binding is not this worker's"));
    }
    return Result<void>::success();
}

Result<void> WorkerPrivatePipeClient::write(const OperationContext& context,
                                            const WorkerMessage& message) {
    return channel_.write(context, message);
}

Result<WorkerMessage> WorkerPrivatePipeClient::read(
    const OperationContext& context) {
    return channel_.read(context);
}

} // namespace myrd::m5
