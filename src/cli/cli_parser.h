#pragma once

// **The CLI's own layer, and only its own layer.**
//
// Design spec 搂10.2 draws the line this file sits on: the typed command, the frame
// and the pipe client are shared with the GUI and the tests; parsing a command line
// and rendering a response as text are the CLI's alone.  Keeping them here is what
// lets the GUI be written later without inheriting a text format, and what lets the
// parser and the renderer be tested without a service, a pipe or a device.
//
// The parser's contract is narrow on purpose: it either produces a typed
// `control::Command` (the same type the service decodes) or it produces the reason
// it could not.  It never talks to anything, so every command the CLI can express is
// expressible in a test.

#include "src/common/result.h"
#include "src/control/command.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::cli {

// The exit codes, from design spec 搂10 (carried into v3 unchanged).
//
// They are the stable contract a script depends on, which is why they are named
// rather than written as literals at each return: "1" must always mean the same
// thing.
enum class CliExitCode : int {
    success = 0,
    // The command ran and the product refused or failed it.
    failure = 1,
    // The command line was wrong, or the service rejected its shape.
    usage = 2,
    // The service could not be reached, or did not speak this protocol.
    unavailable = 3,
    // The caller is not authorised.
    permission = 4,
    // The result is not known: timed out, or the operation is still resolving.
    indeterminate = 5,
};

[[nodiscard]] const char* to_string(CliExitCode code) noexcept;

struct CliRequest final {
    control::Command command;
    bool no_wait{};
    bool dry_run{};
    // Wait for the admitted operation to reach a terminal state before returning.
    // Implemented by the client with `operation wait`, not by holding a request open:
    // the service's own rule is that a client's timeout never cancels an operation
    // that is already running safely (invariant 9).
    bool wait{};
    // How long `wait` may take, and how long one exchange may take.
    std::uint32_t timeout_seconds{300};
    // Named so a test harness can run a service and a CLI over a private pipe without
    // competing with an installed service.  Production leaves it empty.
    std::wstring pipe_name;
};

enum class CliInvocationKind : std::uint8_t {
    request = 0,
    help,
    version,
};

struct CliInvocation final {
    CliInvocationKind kind{CliInvocationKind::help};
    CliRequest request{};
    // Populated when the command line could not be understood.  The CLI prints it and
    // exits with `usage`.
    std::string usage_error;
};

// `arguments` excludes the program name.
CliInvocation parse_command_line(std::span<const std::wstring> arguments);

[[nodiscard]] const char* usage_text() noexcept;

} // namespace myrd::cli
