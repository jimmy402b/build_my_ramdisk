#include "src/management/service_worker_session.h"

#include <chrono>
#include <utility>

namespace myrd::management {
namespace {

Error session_error(const std::uint32_t code, const wchar_t* operation,
                    const wchar_t* message) {
    return {ErrorDomain::protocol, code, operation, message, 0};
}

Result<void> unexpected(const wchar_t* operation, const wchar_t* message) {
    return Result<void>::failure(session_error(
        static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
        operation, message));
}

} // namespace

ServiceWorkerSession::ServiceWorkerSession(
    std::unique_ptr<m5::WorkerPipeChannel> channel,
    m5::WorkerBinding binding) noexcept
    : channel_(std::move(channel)), binding_(std::move(binding)) {}

ServiceWorkerSession::~ServiceWorkerSession() noexcept = default;

Result<std::unique_ptr<ServiceWorkerSession>> ServiceWorkerSession::create(
    std::unique_ptr<m5::WorkerPipeChannel> channel, m5::WorkerBinding binding) {
    if (!channel) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            session_error(
                static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
                L"ServiceWorkerSession::create",
                L"a session needs an authenticated channel"));
    }
    return Result<std::unique_ptr<ServiceWorkerSession>>::success(
        std::unique_ptr<ServiceWorkerSession>(
            new ServiceWorkerSession(std::move(channel), std::move(binding))));
}

Result<void> ServiceWorkerSession::require_bounded(
    const OperationContext& context) const {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - std::chrono::steady_clock::now());
    if (context.deadline != std::chrono::steady_clock::time_point::max() &&
        remaining.count() > 0 &&
        static_cast<std::uint64_t>(remaining.count()) >
            kMaximumWorkerCommandTimeoutMs) {
        return Result<void>::failure(session_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"ServiceWorkerSession",
            L"the request deadline is beyond what a worker may be asked to wait"));
    }
    return Result<void>::success();
}

Result<m5::WorkerMessage> ServiceWorkerSession::exchange(
    const OperationContext& context, m5::WorkerMessage request) {
    auto bounded = require_bounded(context);
    if (!bounded.ok()) return Result<m5::WorkerMessage>::failure(bounded.error());

    request.kind = m5::WorkerMessageKind::request;
    request.sequence = next_sequence_;
    request.binding = binding_;
    const auto expected_command = request.command;
    const auto expected_operation = request.product_operation_id;

    auto written = channel_->write(context, request);
    if (!written.ok()) return Result<m5::WorkerMessage>::failure(written.error());
    // The sequence is consumed by *sending*, not by having the reply accepted.
    // The worker takes its high-water mark the moment a request authenticates,
    // before it dispatches, so a command that then fails has still used its
    // number -- reusing it would look exactly like a replay and be refused.
    ++next_sequence_;
    auto reply = channel_->read(context);
    if (!reply.ok()) return Result<m5::WorkerMessage>::failure(reply.error());

    if (reply.value().kind != m5::WorkerMessageKind::reply) {
        return Result<m5::WorkerMessage>::failure(unexpected(
            L"ServiceWorkerSession", L"the worker answered a request with a request").error());
    }
    if (reply.value().command != expected_command) {
        return Result<m5::WorkerMessage>::failure(unexpected(
            L"ServiceWorkerSession",
            L"the worker answered a different command").error());
    }
    if (!reply.value().binding.has_value() ||
        !(*reply.value().binding == binding_)) {
        // An answer from another worker is not an answer, whatever it says.
        return Result<m5::WorkerMessage>::failure(unexpected(
            L"ServiceWorkerSession",
            L"the reply is bound to a different worker").error());
    }
    if (expected_operation.has_value() &&
        (!reply.value().product_operation_id.has_value() ||
         !(*reply.value().product_operation_id == *expected_operation))) {
        return Result<m5::WorkerMessage>::failure(unexpected(
            L"ServiceWorkerSession",
            L"the reply answers a different operation").error());
    }
    // A reported failure is returned as a failure.  Reading a success payload
    // past it is how a refusal would be recorded as a success.
    if (reply.value().error.has_value()) {
        return Result<m5::WorkerMessage>::failure(*reply.value().error);
    }
    return reply;
}

Result<m5::WorkerRuntimeStatus> ServiceWorkerSession::status(
    const OperationContext& context) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::status;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<m5::WorkerRuntimeStatus>::failure(reply.error());
    if (!reply.value().status.has_value()) {
        return Result<m5::WorkerRuntimeStatus>::failure(unexpected(
            L"ServiceWorkerSession::status",
            L"the status reply carries no status").error());
    }
    return Result<m5::WorkerRuntimeStatus>::success(*reply.value().status);
}

Result<m5::WorkerRuntimeStatus> ServiceWorkerSession::require_runtime_observation(
    const OperationContext& context) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::require_runtime_observation;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<m5::WorkerRuntimeStatus>::failure(reply.error());
    if (!reply.value().status.has_value()) {
        return Result<m5::WorkerRuntimeStatus>::failure(unexpected(
            L"ServiceWorkerSession::require_runtime_observation",
            L"the observation reply carries no status").error());
    }
    // Belt and braces: the worker already refuses an observation that cannot
    // assert liveness, but a caller must not have to trust that this reply went
    // through the strict path.
    if (!reply.value().status->worker_alive) {
        return Result<m5::WorkerRuntimeStatus>::failure(session_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"ServiceWorkerSession::require_runtime_observation",
            L"the observation does not assert a live runtime"));
    }
    return Result<m5::WorkerRuntimeStatus>::success(*reply.value().status);
}

Result<m5::WorkerPreparedCandidate> ServiceWorkerSession::prepare_checkpoint(
    const OperationContext& context, const product::Uuid& operation_id) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::checkpoint_prepare;
    request.product_operation_id = operation_id;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) {
        return Result<m5::WorkerPreparedCandidate>::failure(reply.error());
    }
    if (!reply.value().prepared.has_value()) {
        return Result<m5::WorkerPreparedCandidate>::failure(unexpected(
            L"ServiceWorkerSession::prepare_checkpoint",
            L"the prepare reply carries no candidate").error());
    }
    return Result<m5::WorkerPreparedCandidate>::success(*reply.value().prepared);
}

Result<m5::WorkerCheckpointCommitReply> ServiceWorkerSession::commit_checkpoint(
    const OperationContext& context, const product::Uuid& operation_id,
    const m5::WorkerCheckpointCommit& commit) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::checkpoint_commit;
    request.product_operation_id = operation_id;
    request.commit = commit;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) {
        return Result<m5::WorkerCheckpointCommitReply>::failure(reply.error());
    }
    // The whole reply, not just its observation: the disposition is what the
    // service promotes, rolls back or parks on, and the failure evidence is what
    // it validates its own durable record against.  Returning the status alone
    // discarded both, which is how every unsuccessful commit came to look alike.
    if (!reply.value().commit_reply.has_value()) {
        return Result<m5::WorkerCheckpointCommitReply>::failure(unexpected(
            L"ServiceWorkerSession::commit_checkpoint",
            L"the commit reply carries no disposition").error());
    }
    return Result<m5::WorkerCheckpointCommitReply>::success(
        *reply.value().commit_reply);
}

Result<m5::WorkerRuntimeStatus> ServiceWorkerSession::abort_checkpoint(
    const OperationContext& context, const product::Uuid& operation_id,
    const std::uint64_t native_operation_id) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::checkpoint_abort;
    request.product_operation_id = operation_id;
    // The candidate's native id is deliberately not sent: the worker aborts the
    // one it is holding, so the only thing that could come of naming one is
    // releasing a prepare that belongs to another operation.
    (void)native_operation_id;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<m5::WorkerRuntimeStatus>::failure(reply.error());
    if (!reply.value().status.has_value()) {
        return Result<m5::WorkerRuntimeStatus>::failure(unexpected(
            L"ServiceWorkerSession::abort_checkpoint",
            L"the abort reply carries no status").error());
    }
    return Result<m5::WorkerRuntimeStatus>::success(*reply.value().status);
}

Result<m5::WorkerRecoveryProof> ServiceWorkerSession::recovery_proof(
    const OperationContext& context,
    const m5::WorkerRecoveryCorrelation& correlation) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::recovery_proof;
    request.product_operation_id = correlation.product_operation_id;
    request.correlation = correlation;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) {
        return Result<m5::WorkerRecoveryProof>::failure(reply.error());
    }
    if (!reply.value().recovery.has_value()) {
        return Result<m5::WorkerRecoveryProof>::failure(unexpected(
            L"ServiceWorkerSession::recovery_proof",
            L"the recovery reply carries no proof").error());
    }
    return Result<m5::WorkerRecoveryProof>::success(*reply.value().recovery);
}

Result<m5::WorkerRemoveAndStopReply> ServiceWorkerSession::remove_and_stop(
    const OperationContext& context, const product::Uuid& operation_id,
    const m5::WorkerRemovalAuthorization& authorization) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::remove_and_stop;
    request.product_operation_id = operation_id;
    request.removal = authorization;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) {
        return Result<m5::WorkerRemoveAndStopReply>::failure(reply.error());
    }
    // The removal answer, not an observation: the caller has to learn the
    // generation the removal happened at, and on a refusal the reason -- which
    // is what tells it whether to resolve the dirty runtime against durable
    // truth or to treat the worker as unresponsive.
    if (!reply.value().removal_reply.has_value()) {
        return Result<m5::WorkerRemoveAndStopReply>::failure(unexpected(
            L"ServiceWorkerSession::remove_and_stop",
            L"the removal reply carries no answer").error());
    }
    return Result<m5::WorkerRemoveAndStopReply>::success(
        *reply.value().removal_reply);
}

Result<void> ServiceWorkerSession::shutdown(const OperationContext& context,
                                            const product::Uuid& operation_id) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::shutdown;
    // Shutting a worker down is not operation-scoped, so the operation id is
    // deliberately not put on the wire; the shape table would refuse it as a
    // stray field, which is the point of stating the shape.
    (void)operation_id;
    // The reply carries a status only when the runtime had been started; a
    // worker that was never started is still a worker that was told to stop.
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<void>::failure(reply.error());
    return Result<void>::success();
}

Result<m5::WorkerRuntimeStatus> ServiceWorkerSession::cleanup(
    const OperationContext& context, const product::Uuid& operation_id) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::cleanup;
    request.product_operation_id = operation_id;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<m5::WorkerRuntimeStatus>::failure(reply.error());
    if (!reply.value().status.has_value()) {
        return Result<m5::WorkerRuntimeStatus>::failure(unexpected(
            L"ServiceWorkerSession::cleanup",
            L"the cleanup reply carries no status").error());
    }
    return Result<m5::WorkerRuntimeStatus>::success(*reply.value().status);
}

Result<m5::WorkerRuntimeStatus> ServiceWorkerSession::start(
    const OperationContext& context, const product::Uuid& operation_id) {
    m5::WorkerMessage request{};
    request.command = m5::WorkerCommand::start;
    request.product_operation_id = operation_id;
    auto reply = exchange(context, std::move(request));
    if (!reply.ok()) return Result<m5::WorkerRuntimeStatus>::failure(reply.error());
    if (!reply.value().status.has_value()) {
        return Result<m5::WorkerRuntimeStatus>::failure(unexpected(
            L"ServiceWorkerSession::start",
            L"the start reply carries no status").error());
    }
    return Result<m5::WorkerRuntimeStatus>::success(*reply.value().status);
}

} // namespace myrd::management
