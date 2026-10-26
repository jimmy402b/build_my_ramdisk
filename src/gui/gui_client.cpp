#include "src/gui/gui_client.h"

#include "src/client/response_view.h"
#include "src/control/command.h"
#include "src/control/public_pipe_client.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace myrd::gui {
namespace {

std::string describe_failure(const Error& error) {
    const auto text = ipc::utf16_to_utf8(error.message);
    return text.ok() ? std::move(text.value()) : std::string("the service reported a failure");
}


// The one exchange both entry points share.  A response with an error is a refusal: the service
// answered, and what it said is the message.
CommandOutcome exchange_command(ipc::PublicPipeClient& client,
                                const control::Command& command,
                                const std::chrono::milliseconds budget,
                                const bool no_wait) {
    CommandOutcome outcome{};
    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() + budget;
    auto request_id = product::Uuid::random();
    if (!request_id.ok()) {
        outcome.message = "a request identifier could not be generated";
        return outcome;
    }
    auto frame = ipc::PublicPipeClient::build_command_request(
        command, request_id.value(), no_wait, /*dry_run=*/false);
    if (!frame.ok()) {
        outcome.message = "the request could not be encoded";
        return outcome;
    }
    auto response = client.request(context, frame.value());
    if (!response.ok()) {
        outcome.message = "the service did not answer";
        return outcome;
    }
    auto decoded = client::decode_service_response(
        response.value().payload, response.value().protocol_minor);
    if (!decoded.ok()) {
        outcome.message = "the service's answer could not be read";
        return outcome;
    }
    outcome.response = decoded.move_value();
    outcome.accepted = !outcome.response->error.has_value();
    if (!outcome.accepted) {
        outcome.message = describe_failure(outcome.response->error.value());
    }
    return outcome;
}

} // namespace

CommandOutcome send_command(const control::Command& command,
                            const std::chrono::milliseconds budget) {
    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() + budget;
    auto client = ipc::PublicPipeClient::connect(context);
    if (!client.ok()) {
        CommandOutcome outcome{};
        outcome.message = "the service is not running";
        return outcome;
    }
    // `no_wait`: the answer to "start this disk" is an admission, not a completion.  Waiting for the
    // operation here is what would freeze a window, so the wait happens in the progress timer.
    return exchange_command(*client.value(), command, budget, /*no_wait=*/true);
}

OperationProgress poll_operation(const product::Uuid& operation_id,
                                 const std::chrono::milliseconds budget) {
    OperationProgress progress{};
    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() + budget;
    auto client = ipc::PublicPipeClient::connect(context);
    if (!client.ok()) {
        progress.message = "the service is not running";
        return progress;
    }
    const auto outcome = exchange_command(
        *client.value(), control::OperationShowCommand{operation_id}, budget,
        /*no_wait=*/false);
    if (!outcome.accepted || !outcome.response.has_value()
        || !outcome.response->operation.has_value()) {
        progress.message = outcome.message.empty() ? "the operation could not be observed"
                                                   : outcome.message;
        return progress;
    }
    progress.observed = true;
    progress.state = outcome.response->operation->state;
    progress.phase = outcome.response->operation->phase;
    return progress;
}

DiskSnapshot load_disks(const std::chrono::milliseconds budget) {
    DiskSnapshot snapshot{};

    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() + budget;
    auto client = ipc::PublicPipeClient::connect(context);
    if (!client.ok()) {
        // A tray application starts before the service does, or beside a service the user has
        // stopped.  That is a reason, not an error.
        snapshot.unavailable_reason = "the service is not running";
        return snapshot;
    }

    auto request_id = product::Uuid::random();
    if (!request_id.ok()) {
        snapshot.unavailable_reason = "a request identifier could not be generated";
        return snapshot;
    }
    control::Command command = control::DiskListCommand{};
    auto frame = ipc::PublicPipeClient::build_command_request(
        command, request_id.value(), /*no_wait=*/false, /*dry_run=*/false);
    if (!frame.ok()) {
        snapshot.unavailable_reason = "the request could not be encoded";
        return snapshot;
    }
    auto response = client.value()->request(context, frame.value());
    if (!response.ok()) {
        snapshot.unavailable_reason = "the service did not answer";
        return snapshot;
    }
    auto decoded = client::decode_service_response(
        response.value().payload, response.value().protocol_minor);
    if (!decoded.ok()) {
        snapshot.unavailable_reason = "the service's answer could not be read";
        return snapshot;
    }
    const auto& wire = decoded.value();
    if (wire.error.has_value()) {
        snapshot.unavailable_reason = describe_failure(wire.error.value());
        return snapshot;
    }
    snapshot.disks = wire.profiles;
    return snapshot;
}

} // namespace myrd::gui