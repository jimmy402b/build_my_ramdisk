#include "tests/support/test_framework.h"

#include "src/host/m5_worker_protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace myrd;
using namespace myrd::m5;

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

WorkerBinding binding_for(const std::uint32_t pid, const std::uint64_t session) {
    return WorkerBinding{pid, 0x0123456789ABCDEFull, session,
                         uuid("11111111-2222-3333-4444-555555555555")};
}

WorkerBinding session_binding() { return binding_for(4242, 7); }

WorkerPreparedCandidate candidate() {
    return WorkerPreparedCandidate{5, 6, 6 + kM5CheckpointOperationOffset, 99, 2,
                                   digest(0xAB)};
}

persistence::ImageUuid image_uuid(const std::uint8_t fill) {
    persistence::ImageUuid value{};
    value.bytes[0] = std::byte{fill};
    return value;
}

WorkerRuntimeStatus status() {
    WorkerRuntimeStatus value{};
    value.worker_alive = true;
    value.image_id = image_uuid(0x11);
    value.committed_generation = 6;
    value.dirty = true;
    value.dirty_epoch = 99;
    value.active_operation_id = uuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    value.active_operation_phase = 3;
    return value;
}

WorkerRecoveryCorrelation correlation() {
    persistence::CheckpointRecoveryCorrelation checkpoint{};
    checkpoint.image_uuid = image_uuid(0x11);
    checkpoint.prior_generation = 5;
    checkpoint.candidate_generation = 6;
    checkpoint.native_operation_id = 6 + kM5CheckpointOperationOffset;
    checkpoint.manifest_slot = 2;
    checkpoint.candidate_bytes_digest = digest(0xAB);
    return WorkerRecoveryCorrelation{
        uuid("99999999-8888-7777-6666-555555555555"), checkpoint};
}

WorkerRemovalAuthorization removal_authorization() {
    WorkerRemovalAuthorization value{
        uuid("77777777-6666-5555-4444-333333333333"),
        uuid("11111111-2222-3333-4444-555555555555")};
    value.image_id = image_uuid(0x11);
    value.ownership_evidence_digest = digest(0xCD);
    value.writer_lock_proved = true;
    value.device_match_proved = true;
    value.bundle_identity_proved = true;
    return value;
}

Result<std::vector<std::byte>> encode(const WorkerMessage& message) {
    return encode_worker_message(message);
}

} // namespace

// ---- the native operation id rule ----

TEST_CASE(m5_native_operation_id_never_aliases_the_m4_one_shot_id) {
    constexpr std::uint64_t prior_generation = 5;
    const std::uint64_t candidate_generation = prior_generation + 1;

    const auto m5 = m5_native_operation_id(candidate_generation);
    REQUIRE(m5.has_value());
    REQUIRE_EQ(*m5, candidate_generation + kM5CheckpointOperationOffset);

    // M4 writes checkpoint_generation + 3902, which under the same resulting
    // image is candidate_generation + 3901.  The journal must be able to tell
    // the two apart, which is what makes the reconcile path meaningful.
    const std::uint64_t m4 = prior_generation + kM5CheckpointOperationOffset;
    REQUIRE_EQ(m4, candidate_generation + kM5CheckpointOperationOffset - 1);
    REQUIRE(*m5 != m4);
}

TEST_CASE(m5_native_operation_id_refuses_the_zero_encoding) {
    // Zero is the "no operation" encoding, so the single candidate generation
    // that would produce it is refused rather than silently aliased.
    const std::uint64_t aliasing_generation =
        std::uint64_t{0} - kM5CheckpointOperationOffset;
    REQUIRE(!m5_native_operation_id(aliasing_generation).has_value());
    REQUIRE(m5_native_operation_id(aliasing_generation + 1).has_value());
}

// ---- round trip ----

TEST_CASE(m5_worker_protocol_handshake_round_trips_its_nonce_and_binding) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::handshake;
    message.sequence = 1;
    message.binding = session_binding();
    message.handshake = WorkerHandshake{kWorkerProtocolMajor,
                                        kWorkerProtocolMinor, 0xDEADBEEF};

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().handshake.has_value());
    REQUIRE_EQ(decoded.value().handshake->session_nonce, 0xDEADBEEFull);
    REQUIRE(decoded.value().binding.has_value());
    REQUIRE(*decoded.value().binding == session_binding());
    REQUIRE(authenticate_worker_message(decoded.value(), session_binding(), 0).ok());
}

TEST_CASE(m5_worker_protocol_prepare_reply_round_trips_the_frozen_candidate) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::checkpoint_prepare;
    message.sequence = 3;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.prepared = candidate();

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().prepared.has_value());
    REQUIRE(*decoded.value().prepared == candidate());
}

TEST_CASE(m5_worker_protocol_status_round_trips_the_strict_observation) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::require_runtime_observation;
    message.sequence = 4;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.status = status();

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().status.has_value());
    REQUIRE(*decoded.value().status == status());
    // The block-level dirty truth survives the wire, so a caller cannot read
    // "could not ask" as "the runtime is clean".
    REQUIRE(decoded.value().status->dirty);
    // The image identity travels with the observation, because the removal gate
    // has to check an authorization against the bundle the worker actually
    // holds rather than against the authorization's own claim.
    REQUIRE(decoded.value().status->image_id == image_uuid(0x11));
}

TEST_CASE(m5_worker_protocol_correlation_carries_the_persistence_type) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::recovery_proof;
    message.sequence = 9;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.correlation = correlation();

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().correlation.has_value());
    // Field for field, including the image uuid that has no canonical-text form
    // on this channel: the wire form and the read-side inspection's parameter
    // type are the same struct, so they cannot drift into two field sets.
    REQUIRE(decoded.value().correlation->checkpoint.image_uuid ==
            image_uuid(0x11));
    REQUIRE_EQ(decoded.value().correlation->checkpoint.prior_generation, 5ull);
    REQUIRE_EQ(decoded.value().correlation->checkpoint.candidate_generation,
               6ull);
    REQUIRE_EQ(decoded.value().correlation->checkpoint.native_operation_id,
               6 + kM5CheckpointOperationOffset);
    REQUIRE_EQ(decoded.value().correlation->checkpoint.manifest_slot,
               static_cast<std::uint8_t>(2));
    REQUIRE(decoded.value().correlation->checkpoint.candidate_bytes_digest ==
            digest(0xAB));
}

TEST_CASE(m5_worker_protocol_removal_authorization_round_trips_every_proof) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::remove_and_stop;
    message.sequence = 10;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.removal = removal_authorization();

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().removal.has_value());
    REQUIRE(*decoded.value().removal == removal_authorization());
    // Each proof is a separate wire byte, so none can be lost in packing.
    REQUIRE(decoded.value().removal->writer_lock_proved);
    REQUIRE(decoded.value().removal->device_match_proved);
    REQUIRE(decoded.value().removal->bundle_identity_proved);
}

TEST_CASE(m5_require_removal_authorized_refuses_every_partial_proof) {
    const auto binding = session_binding();
    const auto observed = image_uuid(0x11);
    REQUIRE(require_removal_authorized(removal_authorization(), binding, observed)
                .ok());

    // Each of the three proofs is separately sufficient to refuse: a removal is
    // never allowed on a partial proof.
    for (int missing = 0; missing < 3; ++missing) {
        auto partial = removal_authorization();
        if (missing == 0) partial.writer_lock_proved = false;
        if (missing == 1) partial.device_match_proved = false;
        if (missing == 2) partial.bundle_identity_proved = false;
        REQUIRE(!require_removal_authorized(partial, binding, observed).ok());
    }

    // A complete proof for another profile, or for another image, is not a
    // proof for this worker.
    auto other_profile = removal_authorization();
    other_profile.profile_id = uuid("22222222-3333-4444-5555-666666666666");
    REQUIRE(!require_removal_authorized(other_profile, binding, observed).ok());

    auto other_image = removal_authorization();
    other_image.image_id = image_uuid(0x22);
    REQUIRE(!require_removal_authorized(other_image, binding, observed).ok());
}

TEST_CASE(m5_worker_recovery_outcome_maps_every_persistence_verdict) {
    REQUIRE(worker_recovery_outcome(
                persistence::CheckpointRecoveryVerdict::CandidateGenerationProved) ==
            WorkerRecoveryOutcome::candidate_generation_proved);
    REQUIRE(worker_recovery_outcome(
                persistence::CheckpointRecoveryVerdict::PriorGenerationProved) ==
            WorkerRecoveryOutcome::prior_generation_proved);
    REQUIRE(worker_recovery_outcome(
                persistence::CheckpointRecoveryVerdict::OwnershipUnprovable) ==
            WorkerRecoveryOutcome::ownership_unprovable);
}

TEST_CASE(m5_worker_protocol_recovery_proof_round_trips_every_outcome) {
    for (const auto outcome :
         {WorkerRecoveryOutcome::prior_generation_proved,
          WorkerRecoveryOutcome::candidate_generation_proved,
          WorkerRecoveryOutcome::ownership_unprovable}) {
        WorkerMessage message{};
        message.kind = WorkerMessageKind::reply;
        message.command = WorkerCommand::recovery_proof;
        message.sequence = 5;
        message.binding = session_binding();
        message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
        message.recovery = WorkerRecoveryProof{outcome, 6,
                                               6 + kM5CheckpointOperationOffset, 2,
                                               digest(0xAB)};

        const auto bytes = encode(message);
        REQUIRE(bytes.ok());
        const auto decoded = decode_worker_message(bytes.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().recovery.has_value());
        REQUIRE(decoded.value().recovery->outcome == outcome);
    }
}

TEST_CASE(m5_worker_protocol_error_reply_round_trips_domain_and_text) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::start;
    message.sequence = 6;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.error = Error{ErrorDomain::persistence, 0x3901, L"start_aim",
                          L"M3 runtime geometry does not match", 87};

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().error.has_value());
    REQUIRE(decoded.value().error->domain == ErrorDomain::persistence);
    REQUIRE_EQ(decoded.value().error->code, 0x3901u);
    REQUIRE(decoded.value().error->operation == L"start_aim");
    REQUIRE(decoded.value().error->native_code == 87u);
    REQUIRE(!decoded.value().status.has_value());
}

// ---- authentication ----

TEST_CASE(m5_worker_protocol_rejects_a_replayed_sequence) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::status;
    message.sequence = 9;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.status = status();

    // The first delivery is accepted; the same sequence a second time is not,
    // and neither is an older one.
    REQUIRE(authenticate_worker_message(message, session_binding(), 8).ok());
    REQUIRE(!authenticate_worker_message(message, session_binding(), 9).ok());
    REQUIRE(!authenticate_worker_message(message, session_binding(), 10).ok());
}

TEST_CASE(m5_worker_protocol_rejects_a_mismatched_binding) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::status;
    message.sequence = 2;
    message.binding = binding_for(4242, 7);
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.status = status();

    REQUIRE(authenticate_worker_message(message, binding_for(4242, 7), 0).ok());
    // A recycled pid with a different creation time is a different process.
    WorkerBinding other_process{4242, 0xFFFFFFFFFFFFFFFFull, 7,
                                uuid("11111111-2222-3333-4444-555555555555")};
    REQUIRE(!authenticate_worker_message(message, other_process, 0).ok());
    // A pid that happens to match but was never bound to this session.
    REQUIRE(!authenticate_worker_message(message, binding_for(99, 7), 0).ok());
    // The same worker from the previous service session is not this session.
    REQUIRE(!authenticate_worker_message(message, binding_for(4242, 8), 0).ok());
}

TEST_CASE(m5_worker_protocol_rejects_a_message_without_a_product_operation_id) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::checkpoint_abort;
    message.sequence = 1;
    message.binding = session_binding();
    message.status = WorkerRuntimeStatus{};

    // Structurally the message is well formed, but a mutation that cannot be
    // attributed to a product operation may not be authenticated.
    REQUIRE(!authenticate_worker_message(message, session_binding(), 0).ok());
}

// ---- candidate identity ----

TEST_CASE(m5_require_matching_candidate_refuses_any_substituted_identity) {
    const auto prepared = candidate();
    const WorkerCheckpointCommit exact{prepared.native_operation_id,
                                       prepared.manifest_slot,
                                       prepared.candidate_bytes_digest};
    REQUIRE(require_matching_candidate(exact, prepared).ok());

    // A different native id is a different operation.
    REQUIRE(!require_matching_candidate(
                 {exact.native_operation_id + 1, exact.manifest_slot,
                  exact.candidate_bytes_digest},
                 prepared).ok());
    // A different slot would publish bytes this operation never observed.
    REQUIRE(!require_matching_candidate(
                 {exact.native_operation_id, static_cast<std::uint8_t>(exact.manifest_slot + 1),
                  exact.candidate_bytes_digest},
                 prepared).ok());
    // A different digest is the case that matters: same slot, other bytes.
    REQUIRE(!require_matching_candidate(
                 {exact.native_operation_id, exact.manifest_slot, digest(0x01)},
                 prepared).ok());
}

TEST_CASE(m5_worker_protocol_commit_request_round_trips_the_digest) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::checkpoint_commit;
    message.sequence = 7;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.commit = WorkerCheckpointCommit{6 + kM5CheckpointOperationOffset, 2,
                                            digest(0xAB)};

    const auto bytes = encode(message);
    REQUIRE(bytes.ok());
    const auto decoded = decode_worker_message(bytes.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().commit.has_value());
    REQUIRE(require_matching_candidate(*decoded.value().commit, candidate()).ok());
}

// ---- fail-closed structural decoding ----

TEST_CASE(m5_worker_protocol_refuses_a_message_without_a_binding) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::handshake;
    message.sequence = 1;
    message.handshake = WorkerHandshake{};
    REQUIRE(!encode(message).ok());
}

// A structurally valid header whose binding presence bit is clear: the decoder
// must refuse it rather than treat the channel as unauthenticated.
TEST_CASE(m5_worker_protocol_refuses_a_frame_with_no_binding_bit) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::handshake;
    message.sequence = 1;
    message.binding = session_binding();
    message.handshake = WorkerHandshake{};

    auto bytes = encode(message);
    REQUIRE(bytes.ok());
    // The presence mask is the last field of the fixed header:
    // magic(4) major(2) minor(2) kind(2) command(2) sequence(8) mask(4).
    constexpr std::size_t kMaskOffset = 4 + 2 + 2 + 2 + 2 + 8;
    for (std::size_t index = 0; index < 4; ++index)
        bytes.value()[kMaskOffset + index] = std::byte{0};
    REQUIRE(!decode_worker_message(bytes.value()).ok());
}

TEST_CASE(m5_worker_protocol_refuses_truncated_magic_and_trailing_bytes) {
    WorkerMessage message{};
    message.kind = WorkerMessageKind::reply;
    message.command = WorkerCommand::status;
    message.sequence = 2;
    message.binding = session_binding();
    message.product_operation_id = uuid("99999999-8888-7777-6666-555555555555");
    message.status = status();
    const auto bytes = encode(message);
    REQUIRE(bytes.ok());

    // Truncated body.
    auto truncated = bytes.value();
    truncated.resize(truncated.size() - 1);
    REQUIRE(!decode_worker_message(truncated).ok());

    // Wrong magic -- a public-pipe frame must never decode as a worker message.
    auto wrong_magic = bytes.value();
    wrong_magic[0] = std::byte{0x00};
    REQUIRE(!decode_worker_message(wrong_magic).ok());

    // Trailing bytes mean sender and decoder disagree; accepting them would let
    // a field be dropped silently.
    auto trailing = bytes.value();
    trailing.push_back(std::byte{0x00});
    REQUIRE(!decode_worker_message(trailing).ok());

    // Shorter than the fixed header.
    REQUIRE(!decode_worker_message(
                 std::span<const std::byte>(bytes.value().data(), 8)).ok());
}

TEST_CASE(m5_worker_protocol_refuses_a_payload_its_command_does_not_define) {
    // A handshake that also carries a status would make the command ambiguous.
    WorkerMessage message{};
    message.kind = WorkerMessageKind::request;
    message.command = WorkerCommand::handshake;
    message.sequence = 1;
    message.binding = session_binding();
    message.handshake = WorkerHandshake{};
    message.status = WorkerRuntimeStatus{};
    REQUIRE(!encode(message).ok());
}
