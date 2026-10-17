#include "src/control/public_pipe_client.h"

#include <algorithm>
#include <chrono>

namespace myrd::ipc {

namespace {

template <class T>
Result<T> client_error(const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
        L"public_pipe_client.request", message});
}

// **One command in, one frame out -- the step that did not exist, and that nothing could work without.**
//
// The pieces were all present but never joined: the codec turns a command into a command id plus a payload,
// and a frame carries a command id plus a payload, so a client needed exactly one assignment between them and
// nothing performed it.  Every other client concern -- connecting, sending, decoding the answer, mapping
// errors to exit codes -- is work on top of this, and none of it was reachable without it.
//
// Three properties are worth stating because the assignment alone does not convey them.
//
// The `request_id` is supplied by the caller rather than minted here.  A client that retried would need the
// same id twice to be recognised as the same request, and a builder that generated its own would make that
// impossible without reaching back into the frame.
//
// The control bits come only from the two flags, because the service rejects any other bit.
//
// `operation_id` stays nil on purpose.  An operation id belongs to an operation the service has already
// admitted, so a client has none -- and a non-nil value would be claiming something the client does not own.
Result<Frame> build_command_request_impl(const control::Command& command,
                                         const product::Uuid& request_id,
                                         bool no_wait, bool dry_run) {
    auto encoded = encode_command_payload(command);
    if (!encoded) return Result<Frame>::failure(encoded.error());
    // A nil operation id, obtained the only way the type allows: `Uuid` keeps its default constructor private
    // and offers only parse and random, so "no operation" is expressed explicitly rather than by leaving a
    // field unset.  That is the type enforcing the claim made above -- a client cannot accidentally ship a
    // made-up operation id, and it cannot ship an uninitialised one either.
    auto no_operation = product::Uuid::parse("00000000-0000-0000-0000-000000000000");
    if (!no_operation) return Result<Frame>::failure(no_operation.error());
    std::uint32_t flags = 0;
    if (no_wait) flags |= kFrameFlagNoWait;
    if (dry_run) flags |= kFrameFlagDryRun;
    Frame frame{kProtocolMajor,
                kProtocolMinor,
                MessageType::request,
                flags,
                request_id,
                no_operation.value(),
                static_cast<std::uint32_t>(encoded.value().command_id),
                std::move(encoded.value().payload)};
    return Result<Frame>::success(std::move(frame));
}

class GateCompletion final {
public:
    explicit GateCompletion(InFlightGate& gate) noexcept : gate_(gate) {}
    ~GateCompletion() { static_cast<void>(gate_.complete()); }

private:
    InFlightGate& gate_;
};

} // namespace

PublicPipeClient::PublicPipeClient(
    std::unique_ptr<PipeTransport> transport) noexcept
    : transport_(std::move(transport)) {}

Result<Frame> PublicPipeClient::build_command_request(const control::Command& command,
                                                      const product::Uuid& request_id,
                                                      bool no_wait, bool dry_run) {
    return build_command_request_impl(command, request_id, no_wait, dry_run);
}

Result<std::unique_ptr<PublicPipeClient>> PublicPipeClient::connect(
    const OperationContext& context,
    const std::wstring_view pipe_name) {
    auto transport = PipeTransport::connect(context, pipe_name);
    if (!transport.ok()) {
        return Result<std::unique_ptr<PublicPipeClient>>::failure(
            transport.error());
    }
    return Result<std::unique_ptr<PublicPipeClient>>::success(
        std::unique_ptr<PublicPipeClient>(
            new PublicPipeClient(transport.move_value())));
}

Result<Frame> PublicPipeClient::request(const OperationContext& context,
                                        const Frame& request_frame) {
    if (request_frame.message_type != MessageType::request) {
        return client_error<Frame>(L"Only request frames may be sent");
    }
    auto begun = gate_.begin();
    if (!begun.ok()) return Result<Frame>::failure(begun.error());
    GateCompletion completion(gate_);

    return request_once(context, request_frame);
}

Result<Frame> PublicPipeClient::request_once(
    const OperationContext& context,
    const Frame& request_frame) {
    auto written = transport_->write_frame(context, request_frame);
    if (!written.ok()) return Result<Frame>::failure(written.error());
    auto response = transport_->read_frame(context);
    if (!response.ok()) return response;
    if (response.value().message_type != MessageType::response
        || response.value().request_id != request_frame.request_id
        || response.value().command_id != request_frame.command_id) {
        return client_error<Frame>(L"Pipe response correlation is invalid");
    }
    return response;
}

Result<Frame> PublicPipeClient::wait_operation(
    const OperationContext& context,
    const Frame& request_frame) {
    if (request_frame.command_id
        != static_cast<std::uint32_t>(CommandId::operation_wait)) {
        return client_error<Frame>(
            L"Only operation-wait may use reconnectable long polling");
    }
    if (request_frame.message_type != MessageType::request) {
        return client_error<Frame>(L"Only request frames may be sent");
    }
    auto begun = gate_.begin();
    if (!begun.ok()) return Result<Frame>::failure(begun.error());
    GateCompletion completion(gate_);

    constexpr auto maximum_long_poll = std::chrono::seconds(30);
    constexpr unsigned maximum_attempts = 2;
    for (unsigned attempt = 0; attempt < maximum_attempts; ++attempt) {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<Frame>::failure(context_result.error());
        }
        const OperationContext attempt_context{
            context.stop_token,
            (std::min)(
                context.deadline,
                std::chrono::steady_clock::now() + maximum_long_poll)};
        auto response = request_once(attempt_context, request_frame);
        if (response.ok() || attempt + 1 == maximum_attempts) return response;

        auto replacement = PipeTransport::connect(attempt_context);
        if (!replacement.ok()) {
            return Result<Frame>::failure(replacement.error());
        }
        transport_ = replacement.move_value();
    }
    return client_error<Frame>(L"Operation wait retry invariant failed");
}

} // namespace myrd::ipc
