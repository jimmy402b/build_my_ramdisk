#include "src/service/public_command_endpoint.h"

#include "src/common/operation_context.h"
#include "src/control/ipc_codec.h"
#include "src/management/service_response_wire.h"

#include <chrono>
#include <string>
#include <utility>

namespace myrd::service {

namespace {

Error endpoint_error(const std::uint32_t code, const wchar_t* operation,
                     const wchar_t* message) {
    return Error{ErrorDomain::protocol, code, operation, message};
}

// The answer to a request that was refused before it reached the core.  A response
// frame rather than a closed connection: a client has to be able to tell "the
// service refused this" from "the service is not there", and those are different
// exit codes (2 and 3).
Result<ipc::Frame> refused(const ipc::Frame& request, const std::string& command,
                           Error error) {
    auto payload = management::encode_service_response_wire(
        management::ServiceResponseWire{
            request.request_id,
            command,
            management::ServiceResponseDisposition::completed,
            std::nullopt,
            std::nullopt,
            {},
            {},
            error,
            std::nullopt});
    if (!payload.ok()) {
        return Result<ipc::Frame>::failure(payload.error());
    }
    return Result<ipc::Frame>::success(ipc::Frame{
        ipc::kProtocolMajor,
        ipc::kProtocolMinor,
        ipc::MessageType::response,
        0,
        request.request_id,
        request.operation_id,
        request.command_id,
        payload.move_value()});
}

} // namespace

PublicCommandEndpoint::PublicCommandEndpoint(
    management::ServiceCore& core, std::timed_mutex& execution_gate) noexcept
    : core_(core), execution_gate_(execution_gate) {}

Result<ipc::Frame> PublicCommandEndpoint::handle(
    const OperationContext& context, const ipc::CallerIdentity& caller,
    const ipc::Frame& request) {
    if (request.message_type != ipc::MessageType::request) {
        return Result<ipc::Frame>::failure(endpoint_error(
            0x2201, L"public_command_endpoint",
            L"Only a request frame may be handled"));
    }
    const auto negotiated = ipc::negotiate_protocol(
        ipc::ProtocolVersion{ipc::kProtocolMajor, ipc::kProtocolMinor},
        ipc::ProtocolVersion{request.protocol_major, request.protocol_minor});
    if (!negotiated.ok()) {
        return refused(request, "unknown", negotiated.error());
    }
    // Only the two documented control bits.  An unknown bit is refused rather than
    // ignored: a client that set one believes it asked for something, and quietly
    // not doing it is the worst of the possible answers.
    if ((request.flags & ~(ipc::kFrameFlagNoWait | ipc::kFrameFlagDryRun)) != 0) {
        return refused(request, "unknown",
                       endpoint_error(0x2202, L"public_command_endpoint",
                                      L"The request carries an unknown control bit"));
    }

    auto command = ipc::decode_command_payload(
        static_cast<ipc::CommandId>(request.command_id), request.payload,
        negotiated.value().minor);
    if (!command.ok()) {
        // The command id did not decode, so its *name* is not known either.  Reported
        // as the numeric id rather than as a plausible-looking name: a client that
        // prints a made-up command is worse than one that prints the id it sent.
        return refused(request, "command." + std::to_string(request.command_id),
                       command.error());
    }
    const std::string command_name(control::command_name(command.value()));

    management::ServiceCommandRequest service_request{};
    service_request.request_id = request.request_id;
    service_request.command = command.move_value();
    service_request.caller = caller;
    service_request.no_wait = (request.flags & ipc::kFrameFlagNoWait) != 0;
    service_request.dry_run = (request.flags & ipc::kFrameFlagDryRun) != 0;
    service_request.wait_timeout_ms = std::nullopt;
    service_request.client_wait = management::ClientWaitState::active;

    if (!control::is_mutating(service_request.command)) {
        // A query takes no gate.  The scheduler holds the gate for a whole
        // operation, so taking it here -- shared or exclusive -- would make a list
        // wait for a running create, which is precisely the property T1-04 requires
        // it not to have.  The state a query reads is read under the stores' own
        // locks; the gate would add serialization without adding exclusion.
        auto dispatched = core_.dispatch(service_request);
        if (!dispatched.ok()) {
            return refused(request, command_name, dispatched.error());
        }
        auto payload = management::encode_service_response(dispatched.value());
        if (!payload.ok()) {
            return refused(request, command_name, payload.error());
        }
        return Result<ipc::Frame>::success(ipc::Frame{
            ipc::kProtocolMajor,
            ipc::kProtocolMinor,
            ipc::MessageType::response,
            0,
            request.request_id,
            dispatched.value().operation_id.value_or(request.operation_id),
            request.command_id,
            payload.move_value()});
    }

    // A mutation's admission step writes the durable record the scheduler claims.
    // The scheduler holds this same gate while it claims and runs, so holding it
    // across `dispatch` is what stops a claim from interleaving with an admission.
    // `dispatch` returns as soon as the operation is admitted, so the hold is the
    // admission and not the operation.
    std::unique_lock lock(execution_gate_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(
            (std::max)(static_cast<std::int64_t>(1),
                       static_cast<std::int64_t>(
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               context.deadline - std::chrono::steady_clock::now())
                               .count()))))) {
        return refused(request, command_name,
                       endpoint_error(0x2203, L"public_command_endpoint",
                                      L"The execution gate was not available before "
                                      L"the request deadline"));
    }
    auto dispatched = core_.dispatch(service_request);
    lock.unlock();
    if (!dispatched.ok()) {
        return refused(request, command_name, dispatched.error());
    }
    auto payload = management::encode_service_response(dispatched.value());
    if (!payload.ok()) {
        return refused(request, command_name, payload.error());
    }
    return Result<ipc::Frame>::success(ipc::Frame{
        ipc::kProtocolMajor,
        ipc::kProtocolMinor,
        ipc::MessageType::response,
        0,
        request.request_id,
        dispatched.value().operation_id.value_or(request.operation_id),
        request.command_id,
        payload.move_value()});
}

} // namespace myrd::service
