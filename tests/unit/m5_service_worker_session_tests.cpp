// The service's end of the private channel, driven against the worker's end
// over a real named pipe.
//
// The point of testing it this way rather than against a stub is that every
// property here is a property of the *pair*: request shapes that must be the
// exact inverse of the worker's reply shapes, a sequence the worker enforces,
// and three ways a reply can be wrong that a fake would never produce.

#include "tests/support/test_framework.h"

#include "src/host/m5_worker_private_pipe.h"
#include "src/host/m5_worker_session.h"
#include "src/management/service_worker_session.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace myrd;
using namespace myrd::m5;

constexpr std::uint64_t kSessionNonce = 0x5E55107Aull;

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(20)};
}

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

product::Uuid profile_id() {
    return uuid("11111111-2222-3333-4444-555555555555");
}

product::Uuid operation_id() {
    return uuid("99999999-8888-7777-6666-555555555555");
}

StartupSecret secret() {
    StartupSecret value{};
    value.bytes[0] = std::byte{0x5A};
    return value;
}

WorkerBinding binding() {
    return WorkerBinding{4242, 0x0123456789ABCDEFull, 7, profile_id()};
}

persistence::ImageUuid image_id() {
    persistence::ImageUuid value{};
    value.bytes[0] = std::byte{0x11};
    return value;
}

// The worker's end, driven exactly as `m5_worker_main` drives it: one decoded
// request in, one reply out, over the same session the worker process uses.
struct FakeWorkerRuntime final : WorkerRuntime {
    std::uint64_t committed_generation{5};
    bool worker_alive{true};
    bool refuse_removal{};
    std::optional<WorkerPreparedCandidate> prepared;

    Result<void> start() override { return Result<void>::success(); }
    Result<WorkerRuntimeStatus> observe() override {
        WorkerRuntimeStatus status{};
        status.worker_alive = worker_alive;
        status.image_id = image_id();
        status.committed_generation = committed_generation;
        return Result<WorkerRuntimeStatus>::success(status);
    }
    Result<WorkerPreparedCandidate> prepare_checkpoint(
        const std::uint64_t native_operation_id) override {
        WorkerPreparedCandidate candidate{};
        candidate.prior_generation = committed_generation;
        candidate.candidate_generation = committed_generation + 1;
        candidate.native_operation_id = native_operation_id;
        candidate.cut_dirty_epoch = 41;
        candidate.manifest_slot = 2;
        candidate.candidate_bytes_digest.fill(std::byte{0xAB});
        prepared = candidate;
        return Result<WorkerPreparedCandidate>::success(candidate);
    }
    Result<WorkerCheckpointCommitReply> commit_checkpoint(
        const WorkerCheckpointCommit& commit) override {
        committed_generation = commit.native_operation_id -
                               kM5CheckpointOperationOffset;
        prepared.reset();
        WorkerCheckpointCommitReply reply{};
        reply.disposition = WorkerCommitDisposition::committed;
        reply.generation = committed_generation;
        auto status = observe();
        if (!status.ok()) {
            return Result<WorkerCheckpointCommitReply>::failure(status.error());
        }
        reply.status = status.move_value();
        return Result<WorkerCheckpointCommitReply>::success(std::move(reply));
    }
    Result<WorkerRuntimeStatus> abort_checkpoint(
        const std::uint64_t native_operation_id) override {
        (void)native_operation_id;
        prepared.reset();
        return observe();
    }
    Result<WorkerRecoveryProof> prove_recovery(
        const WorkerRecoveryCorrelation& correlation) override {
        WorkerRecoveryProof proof{};
        proof.outcome = WorkerRecoveryOutcome::candidate_generation_proved;
        proof.observed_generation =
            correlation.checkpoint.candidate_generation;
        proof.native_operation_id = correlation.checkpoint.native_operation_id;
        proof.manifest_slot = correlation.checkpoint.manifest_slot;
        proof.candidate_bytes_digest =
            correlation.checkpoint.candidate_bytes_digest;
        return Result<WorkerRecoveryProof>::success(proof);
    }
    Result<WorkerRuntimeStatus> remove_and_stop(
        const WorkerRemovalAuthorization& authorization) override {
        (void)authorization;
        if (refuse_removal) {
            return Result<WorkerRuntimeStatus>::failure(
                Error{ErrorDomain::internal, 1, L"remove_and_stop",
                      L"the runtime stands dirty", 0});
        }
        return observe();
    }
    Result<void> shutdown() override { return Result<void>::success(); }
    Result<void> cleanup() override {
        // The teardown is what makes the runtime stop being alive; a fake that
        // kept reporting `worker_alive` would hide the difference between
        // cleanup and shutdown.
        worker_alive = false;
        return Result<void>::success();
    }
};

// Runs the worker's end of the channel in its own thread, exactly as the worker
// process does: read, dispatch, write, until the channel closes.
class WorkerPeer final {
public:
    WorkerPeer() {
        name_ = private_pipe_name(profile_id(), 7);
        auto server = WorkerPrivatePipeServer::create(name_);
        REQUIRE(server.ok());
        server_ = server.move_value();
    }

    // The worker's end is the *client*: it connects to the name it was told and
    // authenticates itself, exactly as `m5_worker_main` does.  The service end
    // owns the listening instance.
    void serve_until_closed() {
        thread_ = std::thread([this]() {
            auto client = WorkerPrivatePipeClient::connect(context(), name_);
            REQUIRE(client.ok());
            REQUIRE(client.value()
                        ->handshake(context(), binding(), kSessionNonce, secret())
                        .ok());
            WorkerSession session(binding(), kSessionNonce, secret(), runtime);
            for (;;) {
                auto request = client.value()->read(context());
                if (!request.ok()) return;
                auto reply = session.handle(request.value());
                if (!reply.ok()) return;
                if (!client.value()->write(context(), reply.value()).ok()) return;
                if (session.shutdown_requested()) return;
            }
        });
    }

    Result<std::unique_ptr<management::ServiceWorkerSession>> accept() {
        auto channel = server_->accept_and_handshake(
            context(), binding(), kSessionNonce, secret());
        if (!channel.ok()) {
            return Result<std::unique_ptr<management::ServiceWorkerSession>>::failure(
                channel.error());
        }
        return management::ServiceWorkerSession::create(channel.move_value(),
                                                        binding());
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    FakeWorkerRuntime runtime;

private:
    std::wstring name_;
    std::unique_ptr<WorkerPrivatePipeServer> server_;
    std::thread thread_;
};

} // namespace

TEST_CASE(m5_service_worker_session_status_round_trips_over_the_real_wire) {
    WorkerPeer peer;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());

    // The worker answers no status before its runtime is started, so the
    // service's first command is necessarily the start.
    REQUIRE(session.value()->start(context(), operation_id()).ok());
    auto status = session.value()->status(context());
    REQUIRE(status.ok());
    REQUIRE(status.value().worker_alive);
    REQUIRE_EQ(status.value().committed_generation, 5ull);
    REQUIRE(status.value().image_id == image_id());
    // The handshake was sequence 1, start was 2 and status was 3: each accepted
    // command advances by exactly one, and the worker refuses anything that
    // does not.
    REQUIRE_EQ(session.value()->last_sequence(), 3ull);

    REQUIRE(session.value()
                ->shutdown(context(), operation_id())
                .ok());
    peer.join();
}

TEST_CASE(m5_service_worker_session_prepare_and_commit_the_same_candidate) {
    WorkerPeer peer;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());
    REQUIRE(session.value()->start(context(), operation_id()).ok());

    auto prepared = session.value()->prepare_checkpoint(context(), operation_id());
    REQUIRE(prepared.ok());
    REQUIRE_EQ(prepared.value().prior_generation, 5ull);
    REQUIRE_EQ(prepared.value().candidate_generation, 6ull);
    // The worker named the id by the shared rule; the service never sent one.
    REQUIRE_EQ(prepared.value().native_operation_id,
               6ull + kM5CheckpointOperationOffset);
    REQUIRE_EQ(prepared.value().manifest_slot, static_cast<std::uint8_t>(2));

    WorkerCheckpointCommit commit{};
    commit.native_operation_id = prepared.value().native_operation_id;
    commit.manifest_slot = prepared.value().manifest_slot;
    commit.candidate_bytes_digest = prepared.value().candidate_bytes_digest;
    auto committed = session.value()->commit_checkpoint(context(),
                                                        operation_id(), commit);
    REQUIRE(committed.ok());
    // The disposition is what the caller acts on; the observation rides along.
    REQUIRE(committed.value().disposition == WorkerCommitDisposition::committed);
    REQUIRE(committed.value().status.committed_generation == 6ull);

    REQUIRE(session.value()->shutdown(context(), operation_id()).ok());
    peer.join();
}

TEST_CASE(m5_service_worker_session_refuses_a_commit_of_other_bytes) {
    WorkerPeer peer;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());
    REQUIRE(session.value()->start(context(), operation_id()).ok());
    auto prepared = session.value()->prepare_checkpoint(context(), operation_id());
    REQUIRE(prepared.ok());

    // The worker refuses a candidate it did not freeze; the session must return
    // that refusal rather than a payload.
    WorkerCheckpointCommit forged{};
    forged.native_operation_id = prepared.value().native_operation_id;
    forged.manifest_slot = prepared.value().manifest_slot;
    forged.candidate_bytes_digest.fill(std::byte{0x00});
    auto refused = session.value()->commit_checkpoint(context(), operation_id(),
                                                      forged);
    REQUIRE(!refused.ok());

    // The refusal left the candidate prepared, and an open two-phase boundary
    // must be resolved before the worker may be told to go away (design spec
    // 3.5) -- otherwise the service loses the only evidence of an operation it
    // has not reconciled.
    REQUIRE(!session.value()->shutdown(context(), operation_id()).ok());
    REQUIRE(session.value()->abort_checkpoint(context(), operation_id(), 0).ok());

    REQUIRE(session.value()->shutdown(context(), operation_id()).ok());
    peer.join();
}

TEST_CASE(m5_service_worker_session_propagates_a_worker_refusal) {
    WorkerPeer peer;
    peer.runtime.refuse_removal = true;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());
    REQUIRE(session.value()->start(context(), operation_id()).ok());

    // A removal the worker refuses is the worker's reason, not a generic
    // protocol failure: the caller has to act on *why*.
    WorkerRemovalAuthorization authorization{operation_id(), profile_id()};
    authorization.image_id = image_id();
    authorization.writer_lock_proved = true;
    authorization.device_match_proved = true;
    authorization.bundle_identity_proved = true;
    auto refused = session.value()->remove_and_stop(context(), operation_id(),
                                                    authorization);
    REQUIRE(!refused.ok());
    REQUIRE(refused.error().operation == L"remove_and_stop");

    REQUIRE(session.value()->shutdown(context(), operation_id()).ok());
    peer.join();
}

TEST_CASE(m5_service_worker_session_recovery_proof_reports_the_verdict) {
    WorkerPeer peer;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());
    // A recovery proof is deliberately callable before the runtime starts: a
    // fresh worker decides what the bundle proves *before* it opens it.
    persistence::CheckpointRecoveryCorrelation checkpoint{};
    checkpoint.image_uuid = image_id();
    checkpoint.prior_generation = 5;
    checkpoint.candidate_generation = 6;
    checkpoint.native_operation_id = 6 + kM5CheckpointOperationOffset;
    checkpoint.manifest_slot = 2;
    checkpoint.candidate_bytes_digest.fill(std::byte{0xAB});
    const WorkerRecoveryCorrelation correlation{operation_id(), checkpoint};

    auto proof = session.value()->recovery_proof(context(), correlation);
    REQUIRE(proof.ok());
    REQUIRE(proof.value().outcome ==
            WorkerRecoveryOutcome::candidate_generation_proved);
    REQUIRE_EQ(proof.value().native_operation_id,
               6ull + kM5CheckpointOperationOffset);

    REQUIRE(session.value()->shutdown(context(), operation_id()).ok());
    peer.join();
}

TEST_CASE(m5_service_worker_session_cleanup_is_the_explicit_teardown) {
    WorkerPeer peer;
    peer.serve_until_closed();
    auto session = peer.accept();
    REQUIRE(session.ok());
    REQUIRE(session.value()->start(context(), operation_id()).ok());

    // `cleanup` is the ordered teardown the service can ask for *instead of*
    // shutdown.  The two are not interchangeable: shutdown tells the worker to
    // terminate and the worker tears itself down on the way out, whereas cleanup
    // dismantles while the worker stays alive to be asked for more.
    auto cleaned = session.value()->cleanup(context(), operation_id());
    REQUIRE(cleaned.ok());
    REQUIRE(!cleaned.value().worker_alive);

    // A dismantled worker serves nothing further, including a later shutdown:
    // it is refused rather than served against a runtime that is gone.
    REQUIRE(!session.value()->shutdown(context(), operation_id()).ok());
    peer.join();
}
