#include "src/host/m5_worker_session.h"

#include <utility>

namespace myrd::m5 {
namespace {

Error session_error(const std::uint32_t code, const wchar_t* operation,
                    const wchar_t* message) {
    return {ErrorDomain::protocol, code, operation, message, 0};
}

} // namespace

WorkerSession::WorkerSession(WorkerBinding binding,
                             const std::uint64_t session_nonce,
                             WorkerRuntime& runtime) noexcept
    : binding_(std::move(binding)), session_nonce_(session_nonce),
      runtime_(runtime) {}

Error WorkerSession::refusal(const wchar_t* operation,
                             const wchar_t* message) const {
    return session_error(
        static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
        operation, message);
}

WorkerMessage WorkerSession::make_reply(const WorkerMessage& request) {
    WorkerMessage reply{};
    reply.kind = WorkerMessageKind::reply;
    reply.command = request.command;
    reply.sequence = next_reply_sequence_++;
    reply.binding = binding_;
    reply.product_operation_id = request.product_operation_id;
    return reply;
}

Result<WorkerMessage> WorkerSession::handle(const WorkerMessage& request) {
    // Authenticate before executing anything.  A request that is not authentic
    // for this session is answered with an error and leaves no state behind.
    const auto authenticated = authenticate_worker_message(
        request, binding_, last_accepted_sequence_);
    if (!authenticated.ok()) {
        auto reply = make_reply(request);
        reply.error = authenticated.error();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    // A request must be a request.  A reply sent to the worker is a protocol
    // error, not something to act on.
    if (request.kind != WorkerMessageKind::request) {
        auto reply = make_reply(request);
        reply.error = refusal(L"handle", L"a reply was sent to the worker");
        return Result<WorkerMessage>::success(std::move(reply));
    }

    // The sequence is consumed only once the message is accepted, so a replay
    // of an already-accepted request stays refused.
    last_accepted_sequence_ = request.sequence;

    if (cleaned_up_ && request.command != WorkerCommand::cleanup) {
        auto reply = make_reply(request);
        reply.error = refusal(
            L"handle", L"the worker has already cleaned up and cannot serve commands");
        return Result<WorkerMessage>::success(std::move(reply));
    }
    return dispatch(request);
}

Result<WorkerMessage> WorkerSession::dispatch(const WorkerMessage& request) {
    auto reply = make_reply(request);

    switch (request.command) {
    case WorkerCommand::handshake: {
        // The nonce must be the one this session issued.  Echoing a different
        // value proves the peer is answering a different session.
        if (!request.handshake.has_value() ||
            request.handshake->session_nonce != session_nonce_) {
            reply.error = refusal(L"handshake",
                                  L"handshake nonce does not match this session");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.handshake = WorkerHandshake{kWorkerProtocolMajor,
                                          kWorkerProtocolMinor, session_nonce_};
        // A handshake carries no operation id, so it must satisfy the shape
        // without one.
        reply.product_operation_id.reset();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::start: {
        const auto started = runtime_.start();
        if (!started.ok()) {
            reply.error = started.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        started_ = true;
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::status:
    case WorkerCommand::runtime_observation: {
        if (!started_) {
            reply.error = refusal(L"observe", L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::checkpoint_prepare: {
        if (!started_) {
            reply.error =
                refusal(L"prepare_checkpoint", L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (prepared_.has_value()) {
            reply.error = refusal(
                L"prepare_checkpoint", L"a candidate is already prepared");
            return Result<WorkerMessage>::success(std::move(reply));
        }

        // Only the worker can see M3, so only the worker can name the operation.
        // The candidate generation a prepare will freeze is the next one after
        // the confirmed generation, and the native id follows from it by the one
        // shared rule.  The reply then carries both, so the service can re-check
        // the identity before it records P4 rather than trusting this worker.
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const std::uint64_t expected_candidate =
            status.value().committed_generation + 1;
        const auto native_operation_id = m5_native_operation_id(expected_candidate);
        if (!native_operation_id.has_value()) {
            reply.error = refusal(
                L"prepare_checkpoint",
                L"candidate generation has no valid M5 native operation id");
            return Result<WorkerMessage>::success(std::move(reply));
        }

        const auto candidate = runtime_.prepare_checkpoint(*native_operation_id);
        if (!candidate.ok()) {
            reply.error = candidate.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // Fail closed if M3 froze something other than what this session
        // computed: an identity the service cannot re-derive is one it must not
        // record as P4 evidence.
        const auto frozen = m5_native_operation_id(candidate.value().candidate_generation);
        if (candidate.value().candidate_generation != expected_candidate ||
            !frozen.has_value() ||
            *frozen != candidate.value().native_operation_id) {
            (void)runtime_.abort_checkpoint(candidate.value().native_operation_id);
            reply.error = refusal(
                L"prepare_checkpoint",
                L"M3 froze a candidate this worker cannot re-derive");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        prepared_ = candidate.value();
        reply.prepared = candidate.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::checkpoint_commit: {
        if (!prepared_.has_value() || !request.commit.has_value()) {
            reply.error = refusal(L"commit_checkpoint",
                                  L"no candidate is prepared to commit");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The candidate stays prepared on a mismatch: the service may retry with
        // the recorded identity or abort, but it may never publish other bytes.
        const auto matching =
            require_matching_candidate(*request.commit, *prepared_);
        if (!matching.ok()) {
            reply.error = matching.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto committed = runtime_.commit_checkpoint(*request.commit);
        // Once the runtime has been asked to publish, the candidate belongs to
        // it; this session must never abort it afterwards.
        prepared_.reset();
        if (!committed.ok()) {
            reply.error = committed.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = committed.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::checkpoint_abort: {
        if (!prepared_.has_value()) {
            reply.error = refusal(L"abort_checkpoint",
                                  L"no candidate is prepared to abort");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto aborted =
            runtime_.abort_checkpoint(prepared_->native_operation_id);
        if (!aborted.ok()) {
            reply.error = aborted.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        prepared_.reset();
        reply.status = aborted.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::recovery_proof: {
        if (!request.correlation.has_value()) {
            reply.error = refusal(L"recovery_proof",
                                  L"recovery proof request carries no correlation");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto proof = runtime_.prove_recovery(*request.correlation);
        if (!proof.ok()) {
            reply.error = proof.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.recovery = proof.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::cleanup: {
        const auto cleaned = runtime_.cleanup();
        if (!cleaned.ok()) {
            reply.error = cleaned.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        cleaned_up_ = true;
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }
    }

    reply.error = refusal(L"handle", L"unknown worker command");
    return Result<WorkerMessage>::success(std::move(reply));
}

} // namespace myrd::m5
