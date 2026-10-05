// The per-profile worker process (solution §2.2).
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
// Step 3 after step 2 is the rule from §2.4, and it is the reason the runtime is
// constructed at the point it is rather than earlier.  Nothing here takes an
// argument other than the inherited handle value, which is why no secret can
// appear in this process's command line.

#include "src/host/m5_worker_private_pipe.h"
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

int fail(const wchar_t* message) {
    std::fwprintf(stderr, L"myrd-m5-worker: %s\n", message);
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

    auto client = WorkerPrivatePipeClient::connect(
        context_for(kHandshakeTimeout), spec.value().private_pipe_name);
    if (!client.ok()) {
        return fail(L"the private worker pipe could not be opened");
    }
    auto handshaken = client.value()->handshake(
        context_for(kHandshakeTimeout), self.value(), spec.value().session_nonce,
        spec.value().startup_secret);
    if (!handshaken.ok()) {
        return fail(L"the private channel did not authenticate");
    }

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
    config.virtual_bytes = spec.value().logical_size_bytes;
    config.drive_letter =
        spec.value().mount_target.empty() ? L'\0' : spec.value().mount_target[0];
    config.owner_token = L"myrd:worker:" + std::to_wstring(GetCurrentProcessId());
    auto runtime = DiskWorkerRuntime::open(std::move(config));
    if (!runtime.ok()) return fail(L"the worker runtime could not be prepared");

    WorkerSession session(self.value(), spec.value().session_nonce,
                          spec.value().startup_secret, *runtime.value());
    for (;;) {
        auto request = client.value()->read(context_for(kIdleCommandTimeout));
        if (!request.ok()) {
            // A closed or failed channel is the end of this worker's life, not
            // an error to retry against: the service will launch a replacement.
            break;
        }
        auto reply = session.handle(request.value());
        if (!reply.ok()) break;
        auto written = client.value()->write(context_for(kHandshakeTimeout),
                                             reply.value());
        if (!written.ok()) break;
        if (session.shutdown_requested()) break;
    }

    // Ordered teardown, once, whatever ended the loop.  A failure here is
    // reported: the service must not be told the worker stopped cleanly when it
    // did not.
    auto cleaned = runtime.value()->cleanup();
    if (!cleaned.ok()) return fail(L"the worker's resources could not be released");
    return 0;
}
