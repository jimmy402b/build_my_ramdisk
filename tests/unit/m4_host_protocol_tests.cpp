#include "tests/support/test_framework.h"

#include "src/control/ipc_frame.h"
#include "src/host/host_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view value) {
    auto parsed = myrd::product::Uuid::parse(value);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::host::SecretBytes secret_bytes(std::byte value) {
    myrd::host::SecretBytes bytes{};
    bytes.fill(value);
    return bytes;
}

myrd::host::HandshakeNonce nonce(std::byte value) {
    myrd::host::HandshakeNonce bytes{};
    bytes.fill(value);
    return bytes;
}

myrd::host::WorkerBinding binding(
    const myrd::product::Uuid& profile_id,
    std::uint64_t generation = 7) {
    return myrd::host::WorkerBinding{
        profile_id,
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        0x01dc'0000'1234'5678ULL,
        generation,
        19,
        myrd::product::kProductVersionMajor,
        myrd::product::kProductVersionMinor,
        myrd::product::kProductVersionPatch};
}

struct SessionPair final {
    myrd::host::SessionAuthenticator service;
    myrd::host::SessionAuthenticator worker;
};

SessionPair authenticate(
    const myrd::host::WorkerBinding& expected,
    std::byte secret_value = std::byte{0x31}) {
    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(secret_value));
    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(secret_value));
    REQUIRE(service_secret.ok());
    REQUIRE(worker_secret.ok());

    auto service = myrd::host::ServiceHandshake::begin(
        expected, service_secret.move_value(), nonce(std::byte{0x41}));
    auto worker = myrd::host::WorkerHandshake::begin(
        expected, worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());

    auto challenge = worker.value().accept_hello(
        service.value().hello(), nonce(std::byte{0x51}));
    REQUIRE(challenge.ok());
    auto accepted = service.value().accept_challenge(challenge.value());
    REQUIRE(accepted.ok());
    auto worker_session = worker.value().accept_proof(accepted.value().proof);
    REQUIRE(worker_session.ok());
    return SessionPair{
        accepted.move_value().session, worker_session.move_value()};
}

myrd::host::HostMessage message(std::uint32_t command) {
    return myrd::host::HostMessage{
        uuid("33333333-3333-4333-8333-333333333333"),
        uuid("44444444-4444-4444-8444-444444444444"),
        command,
        0,
        {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}}};
}

} // namespace

TEST_CASE(host_protocol_derives_only_the_exact_private_pipe_name) {
    const auto profile = uuid("11111111-1111-4111-8111-111111111111");
    auto name = myrd::host::private_pipe_name(profile);
    REQUIRE(name.ok());
    REQUIRE_EQ(
        name.value(),
        L"\\\\.\\pipe\\MyRamDisk.Worker."
        L"11111111-1111-4111-8111-111111111111");
}

TEST_CASE(host_protocol_generates_nonzero_independent_256_bit_nonces) {
    auto first = myrd::host::generate_handshake_nonce();
    auto second = myrd::host::generate_handshake_nonce();
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(first.value().size(), 32U);
    REQUIRE(first.value() != second.value());
    myrd::host::HandshakeNonce zero{};
    REQUIRE(first.value() != zero);
    REQUIRE(second.value() != zero);
}

TEST_CASE(host_protocol_authenticates_bound_service_and_worker) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto sessions = authenticate(expected);
    REQUIRE_EQ(sessions.service.binding(), expected);
    REQUIRE_EQ(sessions.worker.binding(), expected);

    auto sealed = sessions.service.seal(message(0x1001));
    REQUIRE(sealed.ok());
    REQUIRE_EQ(sealed.value().sequence, 1ULL);
    REQUIRE_EQ(
        sealed.value().direction,
        myrd::host::MessageDirection::service_to_worker);
    auto opened = sessions.worker.open(sealed.value());
    REQUIRE(opened.ok());
    REQUIRE_EQ(opened.value(), message(0x1001));
}

TEST_CASE(host_protocol_rejects_wrong_profile_session_and_secret) {
    const auto first = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    const auto other_profile = binding(
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    const auto other_generation = binding(first.profile_id, 8);

    for (const auto& worker_binding : {other_profile, other_generation}) {
        auto service_secret = myrd::host::SessionSecret::from_bytes(
            secret_bytes(std::byte{0x31}));
        auto worker_secret = myrd::host::SessionSecret::from_bytes(
            secret_bytes(std::byte{0x31}));
        REQUIRE(service_secret.ok());
        REQUIRE(worker_secret.ok());
        auto service = myrd::host::ServiceHandshake::begin(
            first, service_secret.move_value(), nonce(std::byte{0x41}));
        auto worker = myrd::host::WorkerHandshake::begin(
            worker_binding, worker_secret.move_value());
        REQUIRE(service.ok());
        REQUIRE(worker.ok());
        auto challenge = worker.value().accept_hello(
            service.value().hello(), nonce(std::byte{0x51}));
        REQUIRE(!challenge.ok());
    }

    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x99}));
    REQUIRE(service_secret.ok());
    REQUIRE(worker_secret.ok());
    auto service = myrd::host::ServiceHandshake::begin(
        first, service_secret.move_value(), nonce(std::byte{0x41}));
    auto worker = myrd::host::WorkerHandshake::begin(
        first, worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());
    auto challenge = worker.value().accept_hello(
        service.value().hello(), nonce(std::byte{0x51}));
    REQUIRE(challenge.ok());
    REQUIRE(!service.value().accept_challenge(challenge.value()).ok());
}

TEST_CASE(host_protocol_consumes_each_handshake_proof_exactly_once) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(service_secret.ok());
    REQUIRE(worker_secret.ok());
    auto service = myrd::host::ServiceHandshake::begin(
        expected, service_secret.move_value(), nonce(std::byte{0x41}));
    auto worker = myrd::host::WorkerHandshake::begin(
        expected, worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());
    auto challenge = worker.value().accept_hello(
        service.value().hello(), nonce(std::byte{0x51}));
    REQUIRE(challenge.ok());
    auto accepted = service.value().accept_challenge(challenge.value());
    REQUIRE(accepted.ok());
    REQUIRE(!service.value().accept_challenge(challenge.value()).ok());
    auto session = worker.value().accept_proof(accepted.value().proof);
    REQUIRE(session.ok());
    REQUIRE(!worker.value().accept_proof(accepted.value().proof).ok());
}

TEST_CASE(host_protocol_failed_proof_attempt_also_consumes_handshake) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(service_secret.ok());
    REQUIRE(worker_secret.ok());
    auto service = myrd::host::ServiceHandshake::begin(
        expected, service_secret.move_value(), nonce(std::byte{0x41}));
    auto worker = myrd::host::WorkerHandshake::begin(
        expected, worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());
    auto challenge = worker.value().accept_hello(
        service.value().hello(), nonce(std::byte{0x51}));
    REQUIRE(challenge.ok());
    auto bad_challenge = challenge.value();
    bad_challenge.proof[0] ^= std::byte{0x01};
    REQUIRE(!service.value().accept_challenge(bad_challenge).ok());
    REQUIRE(!service.value().accept_challenge(challenge.value()).ok());

    auto fresh_service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    auto fresh_worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(fresh_service_secret.ok());
    REQUIRE(fresh_worker_secret.ok());
    auto fresh_service = myrd::host::ServiceHandshake::begin(
        expected, fresh_service_secret.move_value(), nonce(std::byte{0x42}));
    auto fresh_worker = myrd::host::WorkerHandshake::begin(
        expected, fresh_worker_secret.move_value());
    REQUIRE(fresh_service.ok());
    REQUIRE(fresh_worker.ok());
    auto fresh_challenge = fresh_worker.value().accept_hello(
        fresh_service.value().hello(), nonce(std::byte{0x52}));
    REQUIRE(fresh_challenge.ok());
    auto accepted = fresh_service.value().accept_challenge(
        fresh_challenge.value());
    REQUIRE(accepted.ok());
    auto bad_proof = accepted.value().proof;
    bad_proof.proof[0] ^= std::byte{0x01};
    REQUIRE(!fresh_worker.value().accept_proof(bad_proof).ok());
    REQUIRE(!fresh_worker.value().accept_proof(accepted.value().proof).ok());
}

TEST_CASE(host_protocol_rejects_a_consumed_session_secret) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(secret.ok());
    auto consumed = myrd::host::ServiceHandshake::begin(
        expected, secret.move_value(), nonce(std::byte{0x41}));
    REQUIRE(consumed.ok());
    REQUIRE(!myrd::host::WorkerHandshake::begin(
        expected, std::move(secret.value())).ok());
}

TEST_CASE(host_protocol_rejects_old_connection_frame_with_fresh_transcript) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto old_sessions = authenticate(expected);
    auto old_frame = old_sessions.service.seal(message(0x1801));
    REQUIRE(old_frame.ok());

    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(service_secret.ok());
    REQUIRE(worker_secret.ok());
    auto service = myrd::host::ServiceHandshake::begin(
        expected, service_secret.move_value(), nonce(std::byte{0x42}));
    auto worker = myrd::host::WorkerHandshake::begin(
        expected, worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());
    auto challenge = worker.value().accept_hello(
        service.value().hello(), nonce(std::byte{0x52}));
    REQUIRE(challenge.ok());
    auto accepted = service.value().accept_challenge(challenge.value());
    REQUIRE(accepted.ok());
    auto fresh_worker = worker.value().accept_proof(accepted.value().proof);
    REQUIRE(fresh_worker.ok());

    REQUIRE(!fresh_worker.value().open(old_frame.value()).ok());
    auto fresh_frame = accepted.value().session.seal(message(0x1802));
    REQUIRE(fresh_frame.ok());
    REQUIRE(fresh_worker.value().open(fresh_frame.value()).ok());
}

TEST_CASE(host_protocol_rejects_replay_skip_and_wrong_direction) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto sessions = authenticate(expected);

    auto first = sessions.service.seal(message(0x2001));
    auto second = sessions.service.seal(message(0x2002));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(second.value().sequence, 2ULL);

    REQUIRE(!sessions.worker.open(second.value()).ok());
    REQUIRE(sessions.worker.open(first.value()).ok());
    REQUIRE(!sessions.worker.open(first.value()).ok());
    REQUIRE(sessions.worker.open(second.value()).ok());
    REQUIRE(!sessions.service.open(second.value()).ok());
}

TEST_CASE(host_protocol_rejects_tamper_without_advancing_sequence) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto sessions = authenticate(expected);
    auto sealed = sessions.service.seal(message(0x3001));
    REQUIRE(sealed.ok());

    auto tampered = sealed.value();
    tampered.message.payload[0] ^= std::byte{0x01};
    REQUIRE(!sessions.worker.open(tampered).ok());
    REQUIRE(sessions.worker.open(sealed.value()).ok());
}

TEST_CASE(host_protocol_wire_codecs_round_trip_without_struct_casts) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(secret.ok());
    auto service = myrd::host::ServiceHandshake::begin(
        expected, secret.move_value(), nonce(std::byte{0x41}));
    REQUIRE(service.ok());
    auto hello_bytes = myrd::host::encode_client_hello(
        service.value().hello());
    REQUIRE(hello_bytes.ok());
    auto hello = myrd::host::decode_client_hello(hello_bytes.value());
    REQUIRE(hello.ok());
    REQUIRE_EQ(hello.value(), service.value().hello());

    auto worker_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x31}));
    REQUIRE(worker_secret.ok());
    auto worker = myrd::host::WorkerHandshake::begin(
        expected, worker_secret.move_value());
    REQUIRE(worker.ok());
    auto challenge = worker.value().accept_hello(
        hello.value(), nonce(std::byte{0x51}));
    REQUIRE(challenge.ok());
    auto challenge_bytes = myrd::host::encode_worker_challenge(
        challenge.value());
    REQUIRE(challenge_bytes.ok());
    auto decoded_challenge = myrd::host::decode_worker_challenge(
        challenge_bytes.value());
    REQUIRE(decoded_challenge.ok());
    REQUIRE_EQ(decoded_challenge.value(), challenge.value());

    auto accepted = service.value().accept_challenge(
        decoded_challenge.value());
    REQUIRE(accepted.ok());
    auto proof_bytes = myrd::host::encode_service_proof(
        accepted.value().proof);
    REQUIRE(proof_bytes.ok());
    auto proof = myrd::host::decode_service_proof(proof_bytes.value());
    REQUIRE(proof.ok());
    REQUIRE_EQ(proof.value(), accepted.value().proof);

    auto frame = accepted.value().session.seal(message(0x4001));
    REQUIRE(frame.ok());
    auto frame_bytes = myrd::host::encode_authenticated_host_frame(
        frame.value());
    REQUIRE(frame_bytes.ok());
    REQUIRE(frame_bytes.value().size() <= 1024U * 1024U);
    auto decoded_frame = myrd::host::decode_authenticated_host_frame(
        frame_bytes.value());
    REQUIRE(decoded_frame.ok());
    REQUIRE_EQ(decoded_frame.value(), frame.value());
}

TEST_CASE(host_protocol_wire_codecs_reject_truncation_trailing_and_bad_direction) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto sessions = authenticate(expected);
    auto frame = sessions.service.seal(message(0x5001));
    REQUIRE(frame.ok());
    auto encoded = myrd::host::encode_authenticated_host_frame(frame.value());
    REQUIRE(encoded.ok());

    for (std::size_t size = 0; size < encoded.value().size(); ++size) {
        REQUIRE(!myrd::host::decode_authenticated_host_frame(
            std::span<const std::byte>(encoded.value()).first(size)).ok());
    }
    auto trailing = encoded.value();
    trailing.push_back(std::byte{0x01});
    REQUIRE(!myrd::host::decode_authenticated_host_frame(trailing).ok());

    auto invalid = frame.value();
    invalid.direction = static_cast<myrd::host::MessageDirection>(255);
    REQUIRE(!myrd::host::encode_authenticated_host_frame(invalid).ok());
}

TEST_CASE(host_protocol_handshake_wire_records_reject_all_nonexact_lengths) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    myrd::host::ClientHello hello{expected, nonce(std::byte{0x41})};
    myrd::host::WorkerChallenge challenge{
        expected,
        nonce(std::byte{0x41}),
        nonce(std::byte{0x51}),
        [] {
            myrd::host::AuthenticationTag value{};
            value.fill(std::byte{0x61});
            return value;
        }()};
    myrd::host::ServiceProof proof{
        expected,
        challenge.service_nonce,
        challenge.worker_nonce,
        challenge.proof};

    auto hello_bytes = myrd::host::encode_client_hello(hello);
    auto challenge_bytes = myrd::host::encode_worker_challenge(challenge);
    auto proof_bytes = myrd::host::encode_service_proof(proof);
    REQUIRE(hello_bytes.ok());
    REQUIRE(challenge_bytes.ok());
    REQUIRE(proof_bytes.ok());

    for (std::size_t size = 0; size < hello_bytes.value().size(); ++size) {
        REQUIRE(!myrd::host::decode_client_hello(
            std::span<const std::byte>(hello_bytes.value()).first(size)).ok());
    }
    for (std::size_t size = 0; size < challenge_bytes.value().size(); ++size) {
        REQUIRE(!myrd::host::decode_worker_challenge(
            std::span<const std::byte>(challenge_bytes.value()).first(size))
            .ok());
        REQUIRE(!myrd::host::decode_service_proof(
            std::span<const std::byte>(proof_bytes.value()).first(size)).ok());
    }
    hello_bytes.value().push_back(std::byte{0x01});
    challenge_bytes.value().push_back(std::byte{0x01});
    proof_bytes.value().push_back(std::byte{0x01});
    REQUIRE(!myrd::host::decode_client_hello(hello_bytes.value()).ok());
    REQUIRE(!myrd::host::decode_worker_challenge(
        challenge_bytes.value()).ok());
    REQUIRE(!myrd::host::decode_service_proof(proof_bytes.value()).ok());
}

TEST_CASE(host_protocol_wire_payload_reserves_space_inside_one_mebibyte_frame) {
    const auto expected = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    auto sessions = authenticate(expected);
    auto largest = message(0x6001);
    largest.payload.assign(
        myrd::host::kMaximumHostPayloadBytes, std::byte{0x5a});
    auto frame = sessions.service.seal(largest);
    REQUIRE(frame.ok());
    auto encoded = myrd::host::encode_authenticated_host_frame(frame.value());
    REQUIRE(encoded.ok());
    REQUIRE(
        encoded.value().size() + myrd::ipc::kFrameHeaderBytes
        <= 1024U * 1024U);

    largest.payload.push_back(std::byte{0x5b});
    REQUIRE(!sessions.service.seal(largest).ok());
}

TEST_CASE(host_protocol_two_phase_rotation_retains_both_credentials_until_commit) {
    auto current = binding(
        uuid("11111111-1111-4111-8111-111111111111"));
    const auto current_bytes = secret_bytes(std::byte{0x21});
    auto current_secret = myrd::host::SessionSecret::from_bytes(current_bytes);
    REQUIRE(current_secret.ok());
    auto window = myrd::host::ReattachmentCredentialWindow::begin(
        current, current_secret.move_value());
    REQUIRE(window.ok());

    auto next = current;
    ++next.session_generation;
    ++next.profile_revision;
    const auto next_bytes = secret_bytes(std::byte{0x31});
    auto next_secret = myrd::host::SessionSecret::from_bytes(next_bytes);
    REQUIRE(next_secret.ok());
    REQUIRE(window.value().stage(next, next_secret.move_value()).ok());
    REQUIRE(window.value().accepts(current));
    REQUIRE(window.value().accepts(next));

    auto selected_current = window.value().credential_for(current);
    auto selected_next = window.value().credential_for(next);
    REQUIRE(selected_current.ok());
    REQUIRE(selected_next.ok());
    REQUIRE(std::equal(
        selected_current.value().bytes().begin(),
        selected_current.value().bytes().end(), current_bytes.begin()));
    REQUIRE(std::equal(
        selected_next.value().bytes().begin(),
        selected_next.value().bytes().end(), next_bytes.begin()));

    auto wrong_profile = next;
    wrong_profile.profile_id = uuid(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    REQUIRE(!window.value().accepts(wrong_profile));
    REQUIRE(!window.value().credential_for(wrong_profile).ok());
    REQUIRE(!window.value().commit(current).ok());
    REQUIRE(window.value().accepts(current));
    REQUIRE(window.value().accepts(next));

    REQUIRE(window.value().commit(next).ok());
    REQUIRE(!window.value().accepts(current));
    REQUIRE(window.value().accepts(next));
    REQUIRE(!window.value().credential_for(current).ok());
    REQUIRE(window.value().credential_for(next).ok());
}
