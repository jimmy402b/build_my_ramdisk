#include "src/cli/cli_parser.h"
#include "src/cli/cli_renderer.h"

#include "src/common/operation_context.h"
#include "src/control/public_pipe_client.h"
#include "src/client/response_view.h"
#include "src/product/product_version.h"
#include "src/product/uuid.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace myrd;

// One exchange: build the frame, send it, decode the answer.  Every failure here is
// "the service is not usable right now", which is exit code 3 -- deliberately not
// conflated with a product refusal, which arrives as a *decoded* response and is
// classified by the renderer.
struct Exchange final {
    std::optional<client::ServiceResponseWire> response;
    cli::CliExitCode failure{cli::CliExitCode::success};
    std::string message;
};

OperationContext context_for(const std::chrono::seconds budget) {
    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() + budget;
    return context;
}

Exchange exchange(ipc::PublicPipeClient& client, const control::Command& command,
                  const bool no_wait, const bool dry_run,
                  const std::chrono::seconds budget) {
    Exchange result{};
    auto request_id = product::Uuid::random();
    if (!request_id.ok()) {
        result.failure = cli::CliExitCode::failure;
        result.message = "a request identifier could not be generated";
        return result;
    }
    auto frame = ipc::PublicPipeClient::build_command_request(
        command, request_id.value(), no_wait, dry_run);
    if (!frame.ok()) {
        result.failure = cli::CliExitCode::usage;
        result.message = "the command could not be encoded for the service";
        return result;
    }
    const auto context = context_for(budget);
    auto response = client.request(context, frame.value());
    if (!response.ok()) {
        result.failure = cli::CliExitCode::unavailable;
        result.message = "the control pipe exchange failed";
        return result;
    }
    auto decoded = client::decode_service_response(
        response.value().payload, response.value().protocol_minor);
    if (!decoded.ok()) {
        result.failure = cli::CliExitCode::unavailable;
        result.message = "the service's answer could not be decoded";
        return result;
    }
    result.response = decoded.move_value();
    return result;
}

bool is_terminal(const product::ServiceOperationState state) noexcept {
    return state == product::ServiceOperationState::succeeded
        || state == product::ServiceOperationState::failed
        || state == product::ServiceOperationState::cancelled;
}

// Waits for an admitted operation to reach a terminal state, or for the budget to run
// out.  One `operation wait` at a time rather than one long-poll held open: it is the
// same command the design gives the GUI for progress, and a client that goes away
// mid-wait therefore leaves nothing running behind it.
int wait_for_operation(ipc::PublicPipeClient& client, const product::Uuid& operation_id,
                       const std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        const auto budget = (std::min)(remaining, std::chrono::seconds(30));
        auto waited = exchange(client, control::OperationWaitCommand{operation_id},
                               false, false, budget);
        if (!waited.response.has_value()) {
            std::fprintf(stderr, "%s\n", waited.message.c_str());
            return static_cast<int>(waited.failure);
        }
        const auto& response = waited.response.value();
        if (response.error.has_value()) {
            std::printf("%s\n", cli::render_response(response).c_str());
            const auto code = cli::exit_code_for(response);
            return static_cast<int>(code);
        }
        if (response.operation.has_value() && is_terminal(response.operation->state)) {
            std::printf("%s\n", cli::render_response(response).c_str());
            const auto code = cli::exit_code_for(response);
            return static_cast<int>(code);
        }
        // The wait's own timeout is not the operation's outcome; the loop asks again
        // until the caller's budget expires.
        if (response.disposition
            == product::ServiceDisposition::wait_timeout) {
            continue;
        }
    }
    std::fprintf(stderr, "timed out waiting for the operation to finish\n");
    return static_cast<int>(cli::CliExitCode::indeterminate);
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    std::vector<std::wstring> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

    const auto invocation = cli::parse_command_line(arguments);
    if (invocation.kind == cli::CliInvocationKind::help) {
        std::printf("%s", cli::usage_text());
        return static_cast<int>(cli::CliExitCode::success);
    }
    if (invocation.kind == cli::CliInvocationKind::version) {
        std::printf("myrd-cli %s\n", product::kProductVersion);
        return static_cast<int>(cli::CliExitCode::success);
    }
    if (!invocation.usage_error.empty()) {
        std::fprintf(stderr, "myrd-cli: %s\n\n%s", invocation.usage_error.c_str(),
                     cli::usage_text());
        return static_cast<int>(cli::CliExitCode::usage);
    }

    const auto& request = invocation.request;
    const auto budget = std::chrono::seconds(request.timeout_seconds);
    const auto deadline = std::chrono::steady_clock::now() + budget;

    auto connected = ipc::PublicPipeClient::connect(
        context_for(budget),
        request.pipe_name.empty()
            ? std::wstring_view(ipc::kPublicControlPipeName)
            : std::wstring_view(request.pipe_name));
    if (!connected.ok()) {
        // **The reason is printed, not just the verdict.**  "not available" alone made a
        // legitimate client-side refusal (the pipe's server was not the service) look
        // identical to "nothing is listening"; the operator needs the distinction.
        std::fprintf(stderr,
                     "myrd-cli: the service is not available (is it installed and "
                     "running?)\n");
        if (!connected.error().message.empty()) {
            std::fprintf(stderr, "myrd-cli: %ls (native=%u)\n",
                         connected.error().message.c_str(),
                         connected.error().native_code);
        }
        return static_cast<int>(cli::CliExitCode::unavailable);
    }

    auto sent = exchange(*connected.value(), request.command, request.no_wait,
                         request.dry_run, budget);
    if (!sent.response.has_value()) {
        std::fprintf(stderr, "myrd-cli: %s\n", sent.message.c_str());
        return static_cast<int>(sent.failure);
    }
    const auto& response = sent.response.value();
    std::printf("%s\n", cli::render_response(response).c_str());
    auto code = cli::exit_code_for(response);
    if (code != cli::CliExitCode::success) return static_cast<int>(code);

    // `--wait` is the client's, not the service's: the operation is already running and
    // its durable record owns it, so abandoning the wait abandons nothing (invariant 9).
    if (request.wait && response.operation_id.has_value()
        && !(response.operation.has_value()
             && is_terminal(response.operation->state))) {
        const auto waited = wait_for_operation(*connected.value(),
                                               response.operation_id.value(), deadline);
        if (waited != static_cast<int>(cli::CliExitCode::success)) return waited;
    }
    return static_cast<int>(code);
}
