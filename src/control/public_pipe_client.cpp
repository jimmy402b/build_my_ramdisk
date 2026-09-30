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
