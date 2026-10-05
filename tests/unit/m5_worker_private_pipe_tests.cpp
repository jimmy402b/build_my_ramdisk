// The private worker channel: the startup contract, its codec, and the
// authenticated handshake that has to complete before a worker may touch a
// bundle.
//
// The handshake cases run against a real named pipe and a real anonymous
// startup pipe, because the properties being tested are transport properties:
// a fake would have assumed away exactly what can go wrong.

#include "tests/support/test_framework.h"

#include "src/host/m5_worker_private_pipe.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

StartupSecret secret() {
    StartupSecret value{};
    value.bytes[0] = std::byte{0x5A};
    value.bytes[31] = std::byte{0xA5};
    return value;
}

WorkerBinding binding() {
    return WorkerBinding{4242, 0x0123456789ABCDEFull, 7, profile_id()};
}

WorkerLaunchSpec launch_spec() {
    WorkerLaunchSpec spec{profile_id(), 3};
    spec.bundle_path = L"C:\\ProgramData\\MyRamDisk\\profiles\\a\\bundle";
    spec.image_id = persistence::ImageUuid{};
    spec.image_id->bytes[0] = std::byte{0x42};
    spec.volume_mode = WorkerVolumeMode::provision_new;
    spec.logical_size_bytes = 8ull * 1024 * 1024;
    spec.mount_target = L"R:";
    spec.session_generation = 7;
    spec.private_pipe_name = L"\\\\.\\pipe\\MyRamDisk.M5.Worker.test";
    spec.session_nonce = kSessionNonce;
    spec.startup_secret = secret();
    return spec;
}

} // namespace

TEST_CASE(m5_launch_spec_round_trips_every_field) {
    const auto spec = launch_spec();
    auto encoded = encode_launch_spec(spec);
    REQUIRE(encoded.ok());
    auto decoded = decode_launch_spec(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value() == spec);
    // The secret is part of the spec, not a separate side channel, so a worker
    // that has the spec has the secret and nothing else does.
    REQUIRE(decoded.value().startup_secret == secret());
}

TEST_CASE(m5_launch_spec_round_trips_both_volume_modes) {
    // The mode selects one of two lifecycles -- create the volume or adopt the
    // one that is already there -- so a launch that lost it would run the wrong
    // one.  Both values survive the channel, and they decode as themselves
    // rather than as each other.
    for (const auto mode : {WorkerVolumeMode::adopt_existing,
                            WorkerVolumeMode::provision_new}) {
        auto spec = launch_spec();
        spec.volume_mode = mode;
        auto encoded = encode_launch_spec(spec);
        REQUIRE(encoded.ok());
        auto decoded = decode_launch_spec(encoded.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().volume_mode == mode);
    }
}

TEST_CASE(m5_launch_spec_distinguishes_an_absent_image_from_a_zero_one) {
    // "This bundle must hold this image" and "record whatever the bundle holds"
    // are different instructions, and the second is what an import asks for.  A
    // zero uuid used as the sentinel would make them the same message, so the
    // image travels as a presence bit.
    auto spec = launch_spec();
    spec.image_id.reset();
    auto encoded = encode_launch_spec(spec);
    REQUIRE(encoded.ok());
    auto decoded = decode_launch_spec(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE(!decoded.value().image_id.has_value());
    REQUIRE(decoded.value() == spec);

    // And a present image survives byte for byte, so an adopt checks the
    // identity the service named rather than one the channel invented.
    spec.image_id = persistence::ImageUuid{};
    spec.image_id->bytes[15] = std::byte{0x9C};
    auto round_trip = encode_launch_spec(spec);
    REQUIRE(round_trip.ok());
    auto back = decode_launch_spec(round_trip.value());
    REQUIRE(back.ok());
    REQUIRE(back.value().image_id.has_value());
    REQUIRE(back.value().image_id->bytes[15] == std::byte{0x9C});
}

TEST_CASE(m5_launch_spec_refuses_a_volume_mode_it_does_not_define) {
    // There is no safe default to fall back on: one mode creates a volume and
    // the other mounts an existing one, and a spec asking for neither is a spec
    // this build cannot honour.
    auto spec = launch_spec();
    spec.volume_mode = static_cast<WorkerVolumeMode>(7);
    REQUIRE(!encode_launch_spec(spec).ok());
}

TEST_CASE(m5_launch_spec_refuses_a_foreign_body) {
    auto encoded = encode_launch_spec(launch_spec());
    REQUIRE(encoded.ok());

    auto wrong_magic = encoded.value();
    wrong_magic[0] = std::byte{0x00};
    REQUIRE(!decode_launch_spec(wrong_magic).ok());

    auto truncated = encoded.value();
    truncated.resize(truncated.size() - 1);
    REQUIRE(!decode_launch_spec(truncated).ok());

    auto trailing = encoded.value();
    trailing.push_back(std::byte{0x00});
    REQUIRE(!decode_launch_spec(trailing).ok());
}

TEST_CASE(m5_private_pipe_name_is_unique_per_profile_and_session) {
    const auto other_profile =
        uuid("22222222-3333-4444-5555-666666666666");
    const auto first = private_pipe_name(profile_id(), 7);
    REQUIRE(first == private_pipe_name(profile_id(), 7));
    // A recycled name would let a previous session's instance be mistaken for
    // this one, so both axes have to move it.
    REQUIRE(first != private_pipe_name(profile_id(), 8));
    REQUIRE(first != private_pipe_name(other_profile, 7));
    REQUIRE(first.find(L"\\\\.\\pipe\\") == 0);
}

TEST_CASE(m5_startup_channel_carries_the_spec_and_then_closes) {
    auto channel = create_startup_channel();
    REQUIRE(channel.ok());
    const auto spec = launch_spec();
    REQUIRE(write_launch_spec(channel.value(), spec).ok());

    auto decoded = read_launch_spec(channel.value().child_read_handle);
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value() == spec);

    close_startup_handle(channel.value().child_read_handle);
}

TEST_CASE(m5_private_pipe_completes_an_authenticated_handshake) {
    const auto name = private_pipe_name(profile_id(), 7);
    auto server = WorkerPrivatePipeServer::create(name);
    REQUIRE(server.ok());

    std::thread worker([&]() {
        auto client = WorkerPrivatePipeClient::connect(context(), name);
        REQUIRE(client.ok());
        REQUIRE(client.value()
                    ->handshake(context(), binding(), kSessionNonce, secret())
                    .ok());
        // One command after the handshake, to prove the channel is usable and
        // not merely authenticated.
        WorkerMessage observe{};
        observe.kind = WorkerMessageKind::request;
        observe.command = WorkerCommand::status;
        observe.sequence = 2;
        observe.binding = binding();
        // A status query is not operation-scoped, so it carries no operation id
        // and no payload: it asks for a status, it does not supply one.
        REQUIRE(client.value()->write(context(), observe).ok());
        auto reply = client.value()->read(context());
        REQUIRE(reply.ok());
        REQUIRE(reply.value().kind == WorkerMessageKind::reply);
    });

    auto channel = server.value()->accept_and_handshake(
        context(), binding(), kSessionNonce, secret());
    REQUIRE(channel.ok());
    auto request = channel.value()->read(context());
    REQUIRE(request.ok());
    REQUIRE(request.value().command == WorkerCommand::status);

    WorkerMessage reply{};
    reply.kind = WorkerMessageKind::reply;
    reply.command = WorkerCommand::status;
    reply.sequence = 1;
    reply.binding = binding();
    reply.status = WorkerRuntimeStatus{};
    reply.status->worker_alive = true;
    reply.status->committed_generation = 4;
    REQUIRE(channel.value()->write(context(), reply).ok());
    worker.join();
}

TEST_CASE(m5_private_pipe_refuses_a_worker_without_the_startup_secret) {
    const auto name = private_pipe_name(profile_id(), 7);
    auto server = WorkerPrivatePipeServer::create(name);
    REQUIRE(server.ok());

    std::thread impostor([&]() {
        auto client = WorkerPrivatePipeClient::connect(context(), name);
        REQUIRE(client.ok());
        // Correct nonce, correct binding, wrong secret: an impostor that can
        // open the pipe name still cannot prove it read the startup channel.
        StartupSecret forged{};
        const auto result =
            client.value()->handshake(context(), binding(), kSessionNonce, forged);
        REQUIRE(!result.ok());
    });

    auto channel = server.value()->accept_and_handshake(
        context(), binding(), kSessionNonce, secret());
    REQUIRE(!channel.ok());
    impostor.join();
}

TEST_CASE(m5_private_pipe_refuses_a_worker_bound_to_another_profile) {
    const auto name = private_pipe_name(profile_id(), 7);
    auto server = WorkerPrivatePipeServer::create(name);
    REQUIRE(server.ok());

    std::thread foreign_worker([&]() {
        auto client = WorkerPrivatePipeClient::connect(context(), name);
        REQUIRE(client.ok());
        WorkerBinding other{4242, 0x0123456789ABCDEFull, 7,
                            uuid("22222222-3333-4444-5555-666666666666")};
        REQUIRE(!client.value()
                     ->handshake(context(), other, kSessionNonce, secret())
                     .ok());
    });

    auto channel = server.value()->accept_and_handshake(
        context(), binding(), kSessionNonce, secret());
    REQUIRE(!channel.ok());
    foreign_worker.join();
}

TEST_CASE(m5_private_pipe_refuses_a_recycled_session_name) {
    const auto name = private_pipe_name(profile_id(), 7);
    auto first = WorkerPrivatePipeServer::create(name);
    REQUIRE(first.ok());
    // FILE_FLAG_FIRST_PIPE_INSTANCE: a second instance for the same session is
    // an error, not a second listener someone else could connect to.
    auto second = WorkerPrivatePipeServer::create(name);
    REQUIRE(!second.ok());
}
