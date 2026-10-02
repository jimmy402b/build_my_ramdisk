#include "tests/support/test_framework.h"

#include "src/host/m5_worker_session.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace myrd;
using namespace myrd::m5;

constexpr std::uint64_t kSessionNonce = 0x5E55107Aull;

StartupSecret session_secret() {
    StartupSecret secret{};
    secret.bytes[0] = std::byte{0x7A};
    secret.bytes[31] = std::byte{0x0F};
    return secret;
}
constexpr std::uint64_t kConfirmedGeneration = 5;
constexpr std::uint64_t kCandidateGeneration = kConfirmedGeneration + 1;

Sha256Digest digest(const std::uint8_t fill) {
    Sha256Digest value{};
    value.fill(std::byte{fill});
    return value;
}

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

product::Uuid operation_id() {
    return uuid("99999999-8888-7777-6666-555555555555");
}

WorkerBinding session_binding() {
    return WorkerBinding{4242, 0x0123456789ABCDEFull, 7,
                         uuid("11111111-2222-3333-4444-555555555555")};
}

Error fake_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 1, operation, L"fake failure", 0};
}

// The runtime the session drives.  Every call is recorded so the cases can
// assert not just the reply but what the runtime was actually asked to do --
// in particular that a refused command never reaches it.
struct FakeWorkerRuntime final : WorkerRuntime {
    std::vector<std::string> events;
    std::uint64_t committed_generation{kConfirmedGeneration};
    bool dirty{};
    std::uint64_t dirty_epoch{};
    // When set, prepare returns a candidate whose identity the session cannot
    // re-derive, which must be refused rather than recorded.
    bool prepare_returns_unexpected_generation{};
    bool fail_start{};
    bool fail_commit{};
    bool fail_abort{};
    // What the fake publish reports.  `committed` by default so every existing
    // case keeps meaning what it meant; a case that wants one of the three
    // unsuccessful dispositions sets it and gets the matching failure evidence,
    // which the shape gate requires.
    WorkerCommitDisposition commit_disposition{WorkerCommitDisposition::committed};
    WorkerCommitFailure commit_failure{"commit", 7, 2, 0, 42, {}, 6, "fake"};
    // Drives the strict-observation gate: an observation that cannot assert a
    // live runtime must be refused, not returned.
    bool worker_alive{true};
    persistence::ImageUuid image_id{};
    std::optional<WorkerPreparedCandidate> last_prepared;
    std::optional<WorkerCheckpointCommit> last_commit;
    std::optional<std::uint64_t> last_abort;
    std::optional<WorkerRemovalAuthorization> last_removal;
    bool shutdown_called{};
    WorkerRecoveryOutcome recovery_outcome{
        WorkerRecoveryOutcome::ownership_unprovable};

    Result<void> start() override {
        events.push_back("start");
        if (fail_start) return Result<void>::failure(fake_error(L"start"));
        return Result<void>::success();
    }
    Result<WorkerRuntimeStatus> observe() override {
        events.push_back("observe");
        WorkerRuntimeStatus status{};
        status.worker_alive = worker_alive;
        status.image_id = image_id;
        status.committed_generation = committed_generation;
        status.dirty = dirty;
        status.dirty_epoch = dirty_epoch;
        return Result<WorkerRuntimeStatus>::success(status);
    }
    Result<WorkerPreparedCandidate> prepare_checkpoint(
        const std::uint64_t native_operation_id) override {
        events.push_back("prepare");
        const auto generation = prepare_returns_unexpected_generation
            ? kCandidateGeneration + 4
            : kCandidateGeneration;
        WorkerPreparedCandidate candidate{};
        candidate.prior_generation = kConfirmedGeneration;
        candidate.candidate_generation = generation;
        candidate.native_operation_id = native_operation_id;
        candidate.cut_dirty_epoch = 41;
        candidate.manifest_slot = 2;
        candidate.candidate_bytes_digest = digest(0xAB);
        last_prepared = candidate;
        return Result<WorkerPreparedCandidate>::success(candidate);
    }
    Result<WorkerCheckpointCommitReply> commit_checkpoint(
        const WorkerCheckpointCommit& commit) override {
        events.push_back("commit");
        last_commit = commit;
        if (fail_commit) {
            return Result<WorkerCheckpointCommitReply>::failure(
                fake_error(L"commit"));
        }
        WorkerCheckpointCommitReply reply{};
        reply.disposition = commit_disposition;
        reply.status.worker_alive = true;
        if (reply.landed()) committed_generation = kCandidateGeneration;
        reply.status.committed_generation = committed_generation;
        reply.generation = committed_generation;
        // A non-landed commit must carry the failure that decided it, or the
        // protocol's own shape gate refuses the reply before it is written.
        if (!reply.landed()) reply.failure = commit_failure;
        return Result<WorkerCheckpointCommitReply>::success(std::move(reply));
    }
    Result<WorkerRuntimeStatus> abort_checkpoint(
        const std::uint64_t native_operation_id) override {
        events.push_back("abort");
        last_abort = native_operation_id;
        if (fail_abort) return Result<WorkerRuntimeStatus>::failure(fake_error(L"abort"));
        return observe();
    }
    Result<WorkerRecoveryProof> prove_recovery(
        const WorkerRecoveryCorrelation& correlation) override {
        events.push_back("prove_recovery");
        WorkerRecoveryProof proof{};
        proof.outcome = recovery_outcome;
        // Read straight off the persistence correlation the service sent: there
        // is no second field set to translate from.
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
        events.push_back("remove_and_stop");
        last_removal = authorization;
        return observe();
    }
    Result<void> shutdown() override {
        events.push_back("shutdown");
        shutdown_called = true;
        return Result<void>::success();
    }
    Result<void> cleanup() override {
        events.push_back("cleanup");
        return Result<void>::success();
    }
};

WorkerMessage request(const WorkerCommand command, const std::uint64_t sequence) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = command;
    message.sequence = sequence;
    message.binding = session_binding();
    // Every command but the handshake must be attributable to a product
    // operation, so the helper always supplies one.
    message.product_operation_id = operation_id();
    return message;
}

bool has_event(const FakeWorkerRuntime& runtime, const char* event) {
    for (const auto& value : runtime.events)
        if (value == event) return true;
    return false;
}

} // namespace

TEST_CASE(m5_worker_session_handshake_echoes_the_session_nonce) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    auto handshake = request(WorkerCommand::handshake, 1);
    handshake.handshake = WorkerHandshake{
        kWorkerProtocolMajor, kWorkerProtocolMinor, kSessionNonce,
        session_secret()};
    const auto reply = session.handle(handshake);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().kind == WorkerMessageKind::reply);
    REQUIRE(reply.value().handshake.has_value());
    REQUIRE_EQ(reply.value().handshake->session_nonce, kSessionNonce);
    REQUIRE(!reply.value().error.has_value());
    REQUIRE_EQ(session.last_accepted_sequence(), 1ull);
}

TEST_CASE(m5_worker_session_refuses_a_handshake_for_another_session) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    auto handshake = request(WorkerCommand::handshake, 1);
    handshake.handshake = WorkerHandshake{
        kWorkerProtocolMajor, kWorkerProtocolMinor, kSessionNonce + 1,
        session_secret()};
    const auto reply = session.handle(handshake);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
}

TEST_CASE(m5_worker_session_refuses_a_replayed_request_without_executing_it) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    const auto first = session.handle(request(WorkerCommand::start, 4));
    REQUIRE(first.ok());
    REQUIRE(!first.value().error.has_value());
    REQUIRE_EQ(runtime.events.size(), 2u); // start, observe

    const auto replay = session.handle(request(WorkerCommand::start, 4));
    REQUIRE(replay.ok());
    REQUIRE(replay.value().error.has_value());
    // The replay never reached the runtime.
    REQUIRE_EQ(runtime.events.size(), 2u);
}

TEST_CASE(m5_worker_session_refuses_a_request_bound_to_another_worker) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    auto foreign = request(WorkerCommand::start, 1);
    foreign.binding = WorkerBinding{777, 0x0123456789ABCDEFull, 7,
                                    uuid("11111111-2222-3333-4444-555555555555")};
    const auto reply = session.handle(foreign);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(runtime.events.empty());
}

TEST_CASE(m5_worker_session_observation_before_start_is_refused) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    auto observe = request(WorkerCommand::require_runtime_observation, 1);
    observe.product_operation_id = operation_id();
    const auto reply = session.handle(observe);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(!has_event(runtime, "observe"));
}

TEST_CASE(m5_worker_session_strict_observation_refuses_an_unproved_runtime) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    // An observation that cannot assert a live runtime is refused rather than
    // returned: a caller deciding whether a mutation is safe must never read
    // "could not observe" as "the runtime is clean".
    runtime.worker_alive = false;
    const auto refused =
        session.handle(request(WorkerCommand::require_runtime_observation, 2));
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(!refused.value().status.has_value());

    // The informational command still reports what it saw, so the two are
    // genuinely different questions.
    const auto reported =
        session.handle(request(WorkerCommand::status, 3));
    REQUIRE(reported.ok());
    REQUIRE(reported.value().status.has_value());
    REQUIRE(!reported.value().status->worker_alive);
}

TEST_CASE(m5_worker_session_prepare_names_the_native_id_by_the_shared_rule) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
    prepare.product_operation_id = operation_id();
    const auto reply = session.handle(prepare);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().prepared.has_value());
    REQUIRE_EQ(reply.value().prepared->candidate_generation, kCandidateGeneration);
    REQUIRE_EQ(reply.value().prepared->native_operation_id,
               kCandidateGeneration + kM5CheckpointOperationOffset);
    REQUIRE(session.checkpoint_prepared());
}

// A candidate the service could not re-derive must never become P4 evidence, so
// the worker releases it instead of reporting it.
TEST_CASE(m5_worker_session_refuses_and_releases_an_underivable_candidate) {
    FakeWorkerRuntime runtime;
    runtime.prepare_returns_unexpected_generation = true;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
    prepare.product_operation_id = operation_id();
    const auto reply = session.handle(prepare);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(!session.checkpoint_prepared());
    REQUIRE(has_event(runtime, "abort"));
}

TEST_CASE(m5_worker_session_commit_must_repeat_the_prepared_candidate) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
    prepare.product_operation_id = operation_id();
    REQUIRE(session.handle(prepare).ok());

    const auto frozen = *runtime.last_prepared;

    // Wrong digest: same slot and native id, other bytes.  The candidate stays
    // prepared and the runtime is never asked to publish.
    auto wrong = request(WorkerCommand::checkpoint_commit, 3);
    wrong.product_operation_id = operation_id();
    wrong.commit = WorkerCheckpointCommit{frozen.native_operation_id,
                                          frozen.manifest_slot, digest(0x01)};
    const auto refused = session.handle(wrong);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(session.checkpoint_prepared());
    REQUIRE(!has_event(runtime, "commit"));

    // The exact identity publishes.
    auto exact = request(WorkerCommand::checkpoint_commit, 4);
    exact.product_operation_id = operation_id();
    exact.commit = WorkerCheckpointCommit{frozen.native_operation_id,
                                          frozen.manifest_slot,
                                          frozen.candidate_bytes_digest};
    const auto committed = session.handle(exact);
    REQUIRE(committed.ok());
    REQUIRE(!committed.value().error.has_value());
    REQUIRE(committed.value().commit_reply.has_value());
    REQUIRE(committed.value().commit_reply->disposition
            == WorkerCommitDisposition::committed);
    REQUIRE(committed.value().commit_reply->landed());
    REQUIRE(!committed.value().commit_reply->failure.has_value());
    REQUIRE(!session.checkpoint_prepared());
}

TEST_CASE(m5_worker_session_commit_reply_preserves_each_disposition) {
    // The three unsuccessful dispositions are three different answers with
    // opposite safe handling, so the session must relay whichever one the
    // runtime reported rather than a collapsed summary.  This is the case that
    // would have caught the defect where every non-committed outcome became one
    // generic error.
    const std::array<WorkerCommitDisposition, 3> dispositions{
        WorkerCommitDisposition::not_committed,
        WorkerCommitDisposition::commit_indeterminate,
        WorkerCommitDisposition::committed_needs_reconciliation};

    for (const auto disposition : dispositions) {
        FakeWorkerRuntime runtime;
        WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                             runtime);
        REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

        auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
        prepare.product_operation_id = operation_id();
        REQUIRE(session.handle(prepare).ok());
        const auto frozen = *runtime.last_prepared;

        runtime.commit_disposition = disposition;
        auto commit = request(WorkerCommand::checkpoint_commit, 3);
        commit.product_operation_id = operation_id();
        commit.commit = WorkerCheckpointCommit{frozen.native_operation_id,
                                              frozen.manifest_slot,
                                              frozen.candidate_bytes_digest};
        const auto reply = session.handle(commit);
        REQUIRE(reply.ok());
        // A refusal here would mean the disposition was reported as an error,
        // which is exactly the collapse this case exists to prevent.
        REQUIRE(!reply.value().error.has_value());
        REQUIRE(reply.value().commit_reply.has_value());
        REQUIRE(reply.value().commit_reply->disposition == disposition);
        // `committed_needs_reconciliation` landed, so it carries no failure;
        // the other two did not, so they must.  The distinction is the whole
        // point of keeping four cases rather than three.
        const bool landed =
            disposition == WorkerCommitDisposition::committed_needs_reconciliation;
        REQUIRE(reply.value().commit_reply->landed() == landed);
        REQUIRE(reply.value().commit_reply->failure.has_value() == !landed);
    }
}

TEST_CASE(m5_worker_session_commit_without_a_prepared_candidate_is_refused) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto commit = request(WorkerCommand::checkpoint_commit, 2);
    commit.product_operation_id = operation_id();
    commit.commit = WorkerCheckpointCommit{1, 2, digest(0xAB)};
    const auto reply = session.handle(commit);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(!has_event(runtime, "commit"));
}

TEST_CASE(m5_worker_session_abort_releases_the_prepared_candidate) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
    prepare.product_operation_id = operation_id();
    REQUIRE(session.handle(prepare).ok());
    const auto frozen = *runtime.last_prepared;

    auto abort = request(WorkerCommand::checkpoint_abort, 3);
    abort.product_operation_id = operation_id();
    const auto reply = session.handle(abort);
    REQUIRE(reply.ok());
    REQUIRE(!reply.value().error.has_value());
    REQUIRE(!session.checkpoint_prepared());
    REQUIRE_EQ(*runtime.last_abort, frozen.native_operation_id);

    // Once released it can no longer be committed.
    auto commit = request(WorkerCommand::checkpoint_commit, 4);
    commit.product_operation_id = operation_id();
    commit.commit = WorkerCheckpointCommit{frozen.native_operation_id,
                                           frozen.manifest_slot,
                                           frozen.candidate_bytes_digest};
    const auto refused = session.handle(commit);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
}

// A failed abort leaves the candidate prepared, because the transaction is
// still live and the service must be able to retry rather than assume release.
TEST_CASE(m5_worker_session_failed_abort_keeps_the_candidate_prepared) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    auto prepare = request(WorkerCommand::checkpoint_prepare, 2);
    prepare.product_operation_id = operation_id();
    REQUIRE(session.handle(prepare).ok());

    runtime.fail_abort = true;
    auto abort = request(WorkerCommand::checkpoint_abort, 3);
    abort.product_operation_id = operation_id();
    const auto reply = session.handle(abort);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(session.checkpoint_prepared());
}

TEST_CASE(m5_worker_session_recovery_proof_needs_the_p4_correlation) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    auto missing = request(WorkerCommand::recovery_proof, 1);
    missing.product_operation_id = operation_id();
    const auto refused = session.handle(missing);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(!has_event(runtime, "prove_recovery"));

    auto proof = request(WorkerCommand::recovery_proof, 2);
    proof.product_operation_id = operation_id();
    persistence::CheckpointRecoveryCorrelation checkpoint{};
    checkpoint.prior_generation = kConfirmedGeneration;
    checkpoint.candidate_generation = kCandidateGeneration;
    checkpoint.native_operation_id =
        kCandidateGeneration + kM5CheckpointOperationOffset;
    checkpoint.manifest_slot = 2;
    checkpoint.candidate_bytes_digest = digest(0xAB);
    proof.correlation =
        WorkerRecoveryCorrelation{operation_id(), checkpoint};
    runtime.recovery_outcome = WorkerRecoveryOutcome::candidate_generation_proved;
    const auto reply = session.handle(proof);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().recovery.has_value());
    REQUIRE(reply.value().recovery->outcome ==
            WorkerRecoveryOutcome::candidate_generation_proved);
}

TEST_CASE(m5_worker_session_cleanup_runs_once_and_locks_out_later_commands) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    const auto cleaned = session.handle(request(WorkerCommand::cleanup, 2));
    REQUIRE(cleaned.ok());
    REQUIRE(!cleaned.value().error.has_value());
    REQUIRE(session.cleaned_up());

    auto later = request(WorkerCommand::start, 3);
    const auto refused = session.handle(later);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    // start ran exactly once, before cleanup.
    std::size_t starts = 0;
    for (const auto& event : runtime.events)
        if (event == "start") ++starts;
    REQUIRE_EQ(starts, 1u);
}

// ---- remove_and_stop and shutdown ----

namespace {

WorkerRemovalAuthorization removal_authorization(
    const persistence::ImageUuid& image_id) {
    WorkerRemovalAuthorization value{operation_id(),
                                     session_binding().profile_id};
    value.image_id = image_id;
    value.ownership_evidence_digest = digest(0xCD);
    value.writer_lock_proved = true;
    value.device_match_proved = true;
    value.bundle_identity_proved = true;
    return value;
}

} // namespace

TEST_CASE(m5_worker_session_remove_and_stop_requires_the_full_ownership_proof) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    runtime.image_id.bytes[0] = std::byte{0x11};
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    // A partial proof is refused before the runtime is asked to remove
    // anything: there is no partial removal to fall back to.
    auto partial = request(WorkerCommand::remove_and_stop, 2);
    partial.removal = removal_authorization(runtime.image_id);
    partial.removal->device_match_proved = false;
    const auto refused = session.handle(partial);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(!has_event(runtime, "remove_and_stop"));

    // A complete proof that names a different image is not a proof for this
    // bundle either.
    auto other_image = request(WorkerCommand::remove_and_stop, 3);
    other_image.removal = removal_authorization(runtime.image_id);
    other_image.removal->image_id.bytes[0] = std::byte{0x22};
    const auto mismatched = session.handle(other_image);
    REQUIRE(mismatched.ok());
    REQUIRE(mismatched.value().error.has_value());
    REQUIRE(!has_event(runtime, "remove_and_stop"));

    auto authorized = request(WorkerCommand::remove_and_stop, 4);
    authorized.removal = removal_authorization(runtime.image_id);
    const auto removed = session.handle(authorized);
    REQUIRE(removed.ok());
    REQUIRE(!removed.value().error.has_value());
    REQUIRE(removed.value().status.has_value());
    REQUIRE(has_event(runtime, "remove_and_stop"));
    REQUIRE(runtime.last_removal.has_value());
}

TEST_CASE(m5_worker_session_remove_and_stop_refuses_an_open_two_phase_boundary) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    runtime.image_id.bytes[0] = std::byte{0x11};
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());
    REQUIRE(session
                .handle(request(WorkerCommand::checkpoint_prepare, 2))
                .ok());

    // Removing the bundle would destroy the evidence of an operation the
    // service has not resolved yet.
    auto remove = request(WorkerCommand::remove_and_stop, 3);
    remove.removal = removal_authorization(runtime.image_id);
    const auto refused = session.handle(remove);
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(!has_event(runtime, "remove_and_stop"));
}

TEST_CASE(m5_worker_session_shutdown_refuses_an_open_two_phase_boundary) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());
    REQUIRE(session
                .handle(request(WorkerCommand::checkpoint_prepare, 2))
                .ok());

    const auto refused = session.handle(request(WorkerCommand::shutdown, 3));
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());
    REQUIRE(!runtime.shutdown_called);
    REQUIRE(!session.shutdown_requested());
}

TEST_CASE(m5_worker_session_shutdown_stops_serving_without_dismantling) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);
    REQUIRE(session.handle(request(WorkerCommand::start, 1)).ok());

    const auto stopped = session.handle(request(WorkerCommand::shutdown, 2));
    REQUIRE(stopped.ok());
    REQUIRE(!stopped.value().error.has_value());
    REQUIRE(runtime.shutdown_called);
    REQUIRE(session.shutdown_requested());
    // Shutdown is not a synonym for cleanup: it asks the worker to go away and
    // dismantles nothing, so the ordered teardown is still available after it.
    REQUIRE(!has_event(runtime, "cleanup"));

    const auto refused = session.handle(request(WorkerCommand::status, 3));
    REQUIRE(refused.ok());
    REQUIRE(refused.value().error.has_value());

    // Cleanup is still served, which is the whole reason the two commands are
    // not the same command.
    const auto cleaned = session.handle(request(WorkerCommand::cleanup, 4));
    REQUIRE(cleaned.ok());
    REQUIRE(!cleaned.value().error.has_value());
    REQUIRE(session.cleaned_up());
}

TEST_CASE(m5_worker_session_refuses_a_handshake_without_the_startup_secret) {
    FakeWorkerRuntime runtime;
    WorkerSession session(session_binding(), kSessionNonce, session_secret(),
                         runtime);

    // The right nonce and the wrong secret: the peer answered this session but
    // never read the inherited startup channel, so it is not the worker the
    // service launched.
    auto handshake = request(WorkerCommand::handshake, 1);
    StartupSecret forged{};
    handshake.handshake = WorkerHandshake{kWorkerProtocolMajor,
                                          kWorkerProtocolMinor, kSessionNonce,
                                          forged};
    const auto reply = session.handle(handshake);
    REQUIRE(reply.ok());
    REQUIRE(reply.value().error.has_value());
    REQUIRE(!reply.value().handshake.has_value());
}
