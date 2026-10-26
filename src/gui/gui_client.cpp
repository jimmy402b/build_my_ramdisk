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

} // namespace

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
