// The per-profile worker process (solution ?2.2).
//
// It is a thin shell around three things that are already tested:
// `WorkerPrivatePipeClient` for the channel, `WorkerSession` for the command
// loop, and `DiskWorkerRuntime` for execution.  What lives here is the ordering,
// and the ordering is the security property:
//
//   1. Read the launch spec from the inherited startup handle, then close it.
//   2. Connect to the private pipe named by the spec and complete the
//      authenticated handshake.
//   3. Only then open the bundle.
//
// Step 3 after step 2 is the rule from ?2.4, and it is the reason the runtime is
// constructed at the point it is rather than earlier.  Nothing here takes an
// argument other than the inherited handle value, which is why no secret can
// appear in this process's command line.

#include "src/host/m5_worker_private_pipe.h"
#include "src/common/diagnostic_trace.h"
#include "src/host/m5_worker_runtime.h"
#include "src/host/m5_worker_session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace myrd;
using namespace myrd::m5;

constexpr auto kHandshakeTimeout = std::chrono::seconds(30);
constexpr auto kIdleCommandTimeout = std::chrono::hours(24);

OperationContext context_for(const std::chrono::milliseconds budget) {
    return {{}, std::chrono::steady_clock::now() + budget};
}

// The handle value is the only thing on this process's command line, and it is
// not a secret -- it is meaningless in any other process.  It is still parsed
// strictly, because a malformed one must fail rather than be coerced to a
// plausible number.
std::optional<std::uintptr_t> parse_handle_argument(const int argc,
                                                    wchar_t** argv) {
    if (argc != 3 || std::wstring_view(argv[1]) != L"--launch-handle") {
        return std::nullopt;
    }
    // std::from_chars has no wide-character overload, so this parses by hand:
    // hex only, no sign, no prefix, and every character consumed.
    const std::wstring_view text(argv[2]);
    if (text.empty() || text.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    for (const wchar_t unit : text) {
        std::uint64_t digit = 0;
        if (unit >= L'0' && unit <= L'9') {
            digit = static_cast<std::uint64_t>(unit - L'0');
        } else if (unit >= L'a' && unit <= L'f') {
            digit = static_cast<std::uint64_t>(unit - L'a') + 10;
        } else if (unit >= L'A' && unit <= L'F') {
            digit = static_cast<std::uint64_t>(unit - L'A') + 10;
        } else {
            return std::nullopt;
        }
        value = (value << 4) | digit;
    }
    if (value == 0) return std::nullopt;
    return static_cast<std::uintptr_t>(value);
}

// The worker's own identity for the handshake.  The pid alone is not an
// identity -- Windows recycles pids -- so the process creation time travels with
// it, and the service compares both against the process it launched.
Result<WorkerBinding> self_binding(const WorkerLaunchSpec& spec) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                         &user)) {
        return Result<WorkerBinding>::failure(
            Error{ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
                  L"self_binding", L"GetProcessTimes failed", GetLastError()});
    }
    ULARGE_INTEGER combined{};
    combined.LowPart = creation.dwLowDateTime;
    combined.HighPart = creation.dwHighDateTime;
    return Result<WorkerBinding>::success(
        WorkerBinding{static_cast<std::uint32_t>(GetCurrentProcessId()),
                      combined.QuadPart, spec.session_generation,
                      spec.profile_id});
}

// **A worker's failure also goes to a file.**  The launcher's handle list does not include the
// worker's std handles, so an installed worker's stderr goes nowhere; measured while closing
// B1: a worker that refused its pipe died with no readable message and the service only saw
// ERROR_BROKEN_PIPE.  `logs\worker.log` is the same durable, management-only directory the
// service's startup failures use.
void log_worker_failure(const std::wstring& line) {
    wchar_t program_data[MAX_PATH]{};
    const DWORD length =
        GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
    std::wstring path;
    if (length != 0 && length < MAX_PATH) {
        path.assign(program_data);
        path += L"\\MyRamDisk\\logs\\worker.log";
    }
    // **A fallback for the case this exists for.**  In a console run the durable directory may
    // not exist yet, and then the one diagnostic a refused worker has would be lost again.
    // `%TEMP%` is always writable by the account the worker runs as.
    wchar_t temp[MAX_PATH]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
    const std::wstring fallback =
        temp_length != 0 && temp_length < MAX_PATH
        ? std::wstring(temp) + L"myrd-m5-worker.log"
        : std::wstring();
    auto open = [](const std::wstring& p) {
        return p.empty() ? INVALID_HANDLE_VALUE
                         : CreateFileW(p.c_str(), FILE_APPEND_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE file = open(path);
    if (file == INVALID_HANDLE_VALUE) file = open(fallback);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::wstring text = L"myrd-m5-worker: " + line + L"\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), utf8.data(), bytes,
                            nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                  nullptr);
    }
    CloseHandle(file);
}

int fail(const wchar_t* message) {
    log_worker_failure(std::wstring(message));
    std::fwprintf(stderr, L"myrd-m5-worker: %s\n", message);
    return 1;
}

// A worker that dies reports *why*.  This process's stderr goes to whoever
// launched it, and "the worker's resources could not be released" names the
// consequence rather than the cause -- which, when the channel simply closed,
// is the only record of what the teardown actually hit.
int fail_with(const wchar_t* message, const Error& error) {
    wchar_t line[1024]{};
    std::swprintf(line, 1024, L"%s (op=%ls msg=%ls native=%u code=%u)",
                  message, error.operation.c_str(), error.message.c_str(),
                  error.native_code, error.code);
    log_worker_failure(std::wstring(line));
    std::fwprintf(stderr, L"myrd-m5-worker: %s\n", line);
    return 1;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    const auto handle_value = parse_handle_argument(argc, argv);
    if (!handle_value.has_value()) {
        return fail(L"usage: myrd-m5-worker --launch-handle <hex handle value>");
    }
    void* startup_handle = reinterpret_cast<void*>(*handle_value);
    // A handle value from a command line is unverified input.  The type check
    // costs one call and refuses anything that is not the pipe it should be.
    if (GetFileType(static_cast<HANDLE>(startup_handle)) != FILE_TYPE_PIPE) {
        return fail(L"the inherited startup handle is not a pipe");
    }

    auto spec = read_launch_spec(startup_handle);
    // Closed immediately: once the spec is in memory nothing else may read the
    // channel, and holding it open would keep the secret reachable.
    close_startup_handle(startup_handle);
    if (!spec.ok()) return fail(L"the launch spec could not be read");

    auto self = self_binding(spec.value());
    if (!self.ok()) return fail(L"the worker could not determine its own identity");

    // **The worker's startup phases are traced, because the reclaim trace starts too late to
    // explain a worker that hangs before it.**
    //
    // A cleanup reclaim was measured hanging with **no trace file at all** -- not even the first
    // marker inside `M3AimRuntime::start` -- while an identical reclaim from the other case
    // produced a full trace.  So the block is somewhere in these phases, and the only way to say
    // which is to name them here.
    //
    // The environment is not the explanation and this is worth stating: the launcher calls
    // `CreateProcessW` with a null environment block and `CREATE_UNICODE_ENVIRONMENT`, so the
    // child **inherits** these variables; a worker that got as far as any traced code would have
    // written the file.
    diagnostic_trace("worker: launch spec read, connecting");
    auto client = WorkerPrivatePipeClient::connect(
        context_for(kHandshakeTimeout), spec.value().private_pipe_name);
    if (!client.ok()) {
        return fail(L"the private worker pipe could not be opened");
    }
    diagnostic_trace("worker: pipe connected, authenticating");
    auto handshaken = client.value()->handshake(
        context_for(kHandshakeTimeout), self.value(), spec.value().session_nonce,
        spec.value().startup_secret);
    if (!handshaken.ok()) {
        return fail(L"the private channel did not authenticate");
    }
    diagnostic_trace("worker: handshaken, opening the bundle");

    // Past this point the channel is authenticated and the bundle may be
    // opened.  Constructing the runtime before this line would open a bundle
    // for a peer that had not yet proved it was the worker the service launched.
    WorkerRuntimeConfig config{};
    config.bundle = std::filesystem::path(spec.value().bundle_path);
    config.manifest_name = "disk.rdm";
    // Absent for an import, which records the bundle it finds rather than naming
    // the image it expects; refused for a create, which has no bundle to inspect.
    config.image_id = spec.value().image_id;
    config.volume_mode = spec.value().volume_mode;
    config.volume_guid = spec.value().volume_guid;
    config.volume_label = spec.value().volume_label;
    config.virtual_bytes = spec.value().logical_size_bytes;
    // The budget the service recorded for this profile.  Zero is "the launch has no
    // budget", which the runtime keeps as its own default.
    config.ram_limit_bytes = spec.value().ram_limit_bytes;
    // **The create-only cluster size (R-17a).**  The spec carries it; the runtime hands it to
    // the provisioning helper.  Without this line the value reached the worker and stopped
    // here (measured: the spec said 8192 and the volume was formatted with 4096).
    config.cluster_size_bytes = spec.value().cluster_size_bytes;
    config.drive_letter =
        spec.value().mount_target.empty() ? L'\0' : spec.value().mount_target[0];
    config.owner_token = L"myrd:worker:" + std::to_wstring(GetCurrentProcessId());
    auto runtime = DiskWorkerRuntime::open(std::move(config));
    if (!runtime.ok()) return fail(L"the worker runtime could not be prepared");
    diagnostic_trace("worker: runtime open, serving");

    WorkerSession session(self.value(), spec.value().session_nonce,
                          spec.value().startup_secret, *runtime.value());
    for (;;) {
        auto request = client.value()->read(context_for(kIdleCommandTimeout));
        if (request.ok()) {
            diagnostic_trace("worker: command received");
        }
        if (!request.ok()) {
            // A closed or failed channel is the end of this worker's life, not
            // an error to retry against: the service will launch a replacement.
            break;
        }
        auto reply = session.handle(request.value());
        // **Where the worker's answer is lost.**  A create on the guest ends with the m3 layer returning from a clean
        // stop and the service never hearing a failure, four minutes later -- so the gap is one of these two calls.
        // 'command handled' without 'reply written' is a channel problem; neither is a block inside the action.
        diagnostic_trace("worker: command handled");
        if (!reply.ok()) {
            // A failed action used to end the loop silently, so the service saw only a broken
            // pipe and the reason was lost.  Record it before leaving; the reply cannot be
            // sent because the action produced an error rather than a message.
            log_worker_failure(
                std::wstring(L"command failed: op=") + reply.error().operation +
                L" msg=" + reply.error().message + L" native=" +
                std::to_wstring(reply.error().native_code) + L" code=" +
                std::to_wstring(reply.error().code));
            break;
        }
        auto written = client.value()->write(context_for(kHandshakeTimeout),
                                             reply.value());
        diagnostic_trace("worker: reply written");
        if (!written.ok()) break;
        if (session.shutdown_requested()) break;
    }

    // Ordered teardown, once, whatever ended the loop.  A failure here is
    // reported: the service must not be told the worker stopped cleanly when it
    // did not.
    auto cleaned = runtime.value()->cleanup();
    if (!cleaned.ok()) {
        return fail_with(L"the worker's resources could not be released",
                         cleaned.error());
    }
    return 0;
}
