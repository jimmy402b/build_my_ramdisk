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
                             StartupSecret startup_secret,
                             WorkerRuntime& runtime) noexcept
    : binding_(std::move(binding)), session_nonce_(session_nonce),
      startup_secret_(std::move(startup_secret)), runtime_(runtime) {}

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
    // Once the service has asked this worker to terminate, only the idempotent
    // commands may still run.  Serving a mutation after a shutdown request would
    // be work the service has already stopped waiting for.
    if (shutdown_requested_ && request.command != WorkerCommand::cleanup &&
        request.command != WorkerCommand::shutdown) {
        auto reply = make_reply(request);
        reply.error = refusal(
            L"handle", L"the worker has been asked to shut down and cannot serve commands");
        return Result<WorkerMessage>::success(std::move(reply));
    }
    return dispatch(request);
}

Result<WorkerMessage> WorkerSession::dispatch(const WorkerMessage& request) {
    auto reply = make_reply(request);

    switch (request.command) {
    case WorkerCommand::handshake: {
        // The nonce and the startup secret must both be the values this session
        // issued.  The nonce proves the peer is answering *this* session; the
        // secret proves it read the inherited startup channel rather than
        // merely being able to open the pipe name.  Neither is sufficient
        // alone, so both are required.
        if (!request.handshake.has_value() ||
            request.handshake->session_nonce != session_nonce_) {
            reply.error = refusal(L"handshake",
                                  L"handshake nonce does not match this session");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (!(request.handshake->startup_secret == startup_secret_)) {
            reply.error = refusal(
                L"handshake",
                L"handshake startup secret is not the one this session delivered");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.handshake = WorkerHandshake{kWorkerProtocolMajor,
                                          kWorkerProtocolMinor, session_nonce_,
                                          startup_secret_};
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

    case WorkerCommand::create_bundle: {
        const auto started = runtime_.start();
        if (!started.ok()) {
            reply.error = started.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The runtime is running either way, so the session says so either way:
        // reporting "not started" while a volume is mounted would be a lie about
        // its own state, and the shutdown path depends on knowing which it is.
        started_ = true;
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // What this command promises and `start` does not: the bundle the
        // service is about to register has an identity.  A runtime that came up
        // without one has not answered the question it was asked, and reporting
        // success would let a registration be recorded that nothing proves.
        if (!status.value().bundle.has_value() ||
            !status.value().bundle->complete()) {
            reply.error = refusal(
                L"create_bundle",
                L"the runtime started without recording the bundle's identity");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::inspect_bundle: {
        const auto status = runtime_.inspect_bundle();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The same promise `create_bundle` makes, for the same reason: the
        // service is about to record this as a registration, so a reply without
        // the bundle's identity has not answered the question.
        if (!status.value().bundle.has_value() ||
            !status.value().bundle->complete()) {
            reply.error = refusal(
                L"inspect_bundle",
                L"the bundle could not be inspected for a recordable identity");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // Deliberately not `started_`: nothing was started.  A later `status`
        // read is refused, which is the truth about this worker's state, and the
        // reply says so in its own `worker_alive`.
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::status: {
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

    case WorkerCommand::require_runtime_observation: {
        // Strict, unlike `status`.  The caller is deciding whether a mutation is
        // safe, so "could not observe a live runtime" must be a failure rather
        // than a status the caller could mistake for "the runtime is clean".
        if (!started_) {
            reply.error = refusal(L"require_runtime_observation",
                                  L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (!status.value().worker_alive) {
            reply.error = refusal(
                L"require_runtime_observation",
                L"the runtime cannot be observed alive, so nothing is proved clean");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::freeze_writes: {
        if (!started_) {
            reply.error =
                refusal(L"freeze_writes", L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto frozen = runtime_.freeze_writes();
        if (!frozen.ok()) {
            reply.error = frozen.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The observation is what answers the command: `writes_frozen` is the
        // fact the caller is asking for, and a bare success would leave it
        // trusting that this call, rather than the runtime, took the lock.
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.move_value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::release_write_freeze: {
        if (!started_) {
            reply.error = refusal(L"release_write_freeze",
                                  L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto released = runtime_.release_write_freeze();
        if (!released.ok()) {
            reply.error = released.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The observation is the answer, and on this path it is required to say
        // the freeze is *gone*: the caller is about to let a still-running
        // profile accept writes again, and a success that left the lock held
        // would be recorded as a resumed disk that cannot be written to.
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (status.value().writes_frozen) {
            reply.error = refusal(L"release_write_freeze",
                                  L"the volume write freeze is still held");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.move_value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::reclaim_leftover: {
        // The runtime this command runs against was launched in `reclaim_existing` mode,
        // so starting it already adopted the leftover's volume **without assigning a
        // drive letter**.  What remains is the teardown, and it is the *same* teardown
        // every other path uses -- `cleanup` runs `DiskRuntime::shutdown_clean`, which
        // removes the drive letter, dismounts and stops AIM.
        //
        // So this command does not duplicate `cleanup`; it differs in what it requires.
        // `cleanup` is idempotent housekeeping that a session may run at any point, and a
        // reclaim that never started holds nothing to dismantle -- so a loop reported as
        // a leftover's teardown must refuse here rather than answer success, which is the
        // one outcome that would let a still-mounted residue be recorded as cleared.
        if (!started_) {
            reply.error = refusal(L"reclaim_leftover",
                                  L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto dismantled = runtime_.cleanup();
        if (!dismantled.ok()) {
            reply.error = dismantled.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        cleaned_up_ = true;
        // The answer is an observation rather than a bare success, for the same reason
        // `release_write_freeze` answers with one: the caller has to be able to see that
        // the volume is gone, and "the call returned" is not that fact.
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        reply.status = status.move_value();
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
        // The whole reply travels, not just its observation: the disposition is
        // what the service promotes or rolls back on, and the failure evidence
        // is what it validates its own record against.
        reply.commit_reply = committed.value();
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
        reply.abort_reply = aborted.value();
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

    case WorkerCommand::remove_and_stop: {
        if (!started_) {
            reply.error = refusal(L"remove_and_stop",
                                  L"the runtime has not been started");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (prepared_.has_value()) {
            // A prepared-but-unpublished candidate is an open two-phase
            // boundary.  Removing the bundle underneath it would destroy the
            // evidence of an operation the service has not resolved yet.
            reply.error = refusal(
                L"remove_and_stop",
                L"a checkpoint candidate is prepared and must be resolved first");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        if (!request.removal.has_value()) {
            reply.error = refusal(L"remove_and_stop",
                                  L"removal request carries no authorization");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        auto status = runtime_.observe();
        if (!status.ok()) {
            reply.error = status.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // Check the proof against the image this worker actually holds, not
        // against the authorization's own claim about it.  A complete proof for
        // a different bundle is not a proof for this one.
        const auto authorized = require_removal_authorized(
            *request.removal, binding_, status.value().image_id);
        if (!authorized.ok()) {
            reply.error = authorized.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto removed = runtime_.remove_and_stop(*request.removal);
        if (!removed.ok()) {
            reply.error = removed.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        // The removal answer carries the generation the removal happened at and,
        // on a refusal, why -- neither of which a status can express, and the
        // reason is the only part the caller can act on.
        reply.removal_reply = removed.value();
        return Result<WorkerMessage>::success(std::move(reply));
    }

    case WorkerCommand::shutdown: {
        if (prepared_.has_value()) {
            // §3.5: an unresolved session is resolved before the worker goes
            // away.  Terminating here would abandon a candidate whose P4 record
            // the service still has to reconcile, which is exactly the state
            // the recovery proof exists to avoid.
            reply.error = refusal(
                L"shutdown",
                L"a checkpoint candidate is prepared and must be resolved first");
            return Result<WorkerMessage>::success(std::move(reply));
        }
        const auto stopped = runtime_.shutdown();
        if (!stopped.ok()) {
            reply.error = stopped.error();
            return Result<WorkerMessage>::success(std::move(reply));
        }
        shutdown_requested_ = true;
        if (started_) {
            auto status = runtime_.observe();
            if (!status.ok()) {
                reply.error = status.error();
                return Result<WorkerMessage>::success(std::move(reply));
            }
            reply.status = status.value();
        }
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
