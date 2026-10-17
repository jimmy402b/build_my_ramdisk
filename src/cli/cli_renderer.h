#pragma once

// **Rendering a typed response, and nothing else.**
//
// Two rules decide what this file may do.  It renders only the wire projection --
// never a `ManagedProfileSnapshot`, so a path cannot leak into the output by being
// convenient to print.  And it renders only what the response *states*: the verdict
// is `aggregate`, because that is the value in which the service has already folded
// "the durable record says running but the worker is gone"; reading `runtime` alone
// would print the record's opinion and call it the truth (design spec 搂10.3).
//
// The exit code is here rather than in the client because it is the same decision:
// both are "what does this response mean".  A script depends on the mapping, so it
// is one function with one test table rather than a condition at each call site.

#include "src/cli/cli_parser.h"
#include "src/management/service_response_wire.h"

#include <string>

namespace myrd::cli {

// The human-readable form of one response.  Multi-line, no trailing newline.
[[nodiscard]] std::string render_response(
    const management::ServiceResponseWire& response);

// The process exit code for one response.
[[nodiscard]] CliExitCode exit_code_for(
    const management::ServiceResponseWire& response);

} // namespace myrd::cli
