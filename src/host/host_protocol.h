#pragma once

#include "src/common/result.h"
#include "src/control/ipc_frame.h"
#include "src/product/product_version.h"
#include "src/product/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::host {

inline constexpr std::wstring_view kPrivatePipePrefix =
    L"\\\\.\\pipe\\MyRamDisk.Worker.";
inline constexpr std::uint16_t kHostProtocolMajor = 1;
inline constexpr std::uint16_t kHostProtocolMinor = 0;
inline constexpr std::size_t kSessionSecretBytes = 32;
inline constexpr std::size_t kHandshakeNonceBytes = 32;
inline constexpr std::size_t kAuthenticationTagBytes = 32;
inline constexpr std::size_t kAuthenticatedHostWireOverheadBytes = 244;
inline constexpr std::size_t kMaximumHostPayloadBytes =
    ipc::kMaxPayloadBytes - ipc::kFrameHeaderBytes
    - kAuthenticatedHostWireOverheadBytes;

using SecretBytes = std::array<std::byte, kSessionSecretBytes>;
using HandshakeNonce = std::array<std::byte, kHandshakeNonceBytes>;
using AuthenticationTag = std::array<std::byte, kAuthenticationTagBytes>;

Result<HandshakeNonce> generate_handshake_nonce();

enum class HostProtocolError : std::uint32_t {
    invalid_argument = 0x4C01,
    version_mismatch,
    binding_mismatch,
    authentication_failed,
    handshake_replay,
    sequence_mismatch,
    sequence_exhausted,
    payload_too_large,
    crypto_failure,
    lease_malformed,
    lease_binding_mismatch,
    lease_protection_failed,
    commit_indeterminate,
    unsafe_runtime_lease,
};

Result<std::wstring> private_pipe_name(
    const product::Uuid& profile_id);

struct WorkerBinding final {
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint32_t worker_pid{};
    std::uint64_t process_creation_time{};
    std::uint64_t session_generation{};
    std::uint64_t profile_revision{};
    std::uint32_t product_major{};
    std::uint32_t product_minor{};
    std::uint32_t product_patch{};

    bool operator==(const WorkerBinding&) const noexcept = default;
};

bool is_valid(const WorkerBinding& binding) noexcept;

// Move-only and zeroized on destruction. This type intentionally has no text
// formatter or implicit conversion, so secret bytes cannot enter diagnostics.
class SessionSecret final {
public:
    static Result<SessionSecret> from_bytes(const SecretBytes& bytes);
    static Result<SessionSecret> generate();

    ~SessionSecret() noexcept;
    SessionSecret(SessionSecret&& other) noexcept;
    SessionSecret& operator=(SessionSecret&& other) noexcept;
    SessionSecret(const SessionSecret&) = delete;
    SessionSecret& operator=(const SessionSecret&) = delete;

    std::span<const std::byte, kSessionSecretBytes> bytes() const noexcept;

private:
    explicit SessionSecret(const SecretBytes& bytes) noexcept;
    void clear() noexcept;

    SecretBytes bytes_{};
    bool valid_{};
};

struct ClientHello final {
    WorkerBinding binding;
    HandshakeNonce service_nonce{};

    bool operator==(const ClientHello&) const noexcept = default;
};

struct WorkerChallenge final {
    WorkerBinding binding;
    HandshakeNonce service_nonce{};
    HandshakeNonce worker_nonce{};
    AuthenticationTag proof{};

    bool operator==(const WorkerChallenge&) const noexcept = default;
};

struct ServiceProof final {
    WorkerBinding binding;
    HandshakeNonce service_nonce{};
    HandshakeNonce worker_nonce{};
    AuthenticationTag proof{};

    bool operator==(const ServiceProof&) const noexcept = default;
};

enum class MessageDirection : std::uint8_t {
    service_to_worker = 1,
    worker_to_service = 2,
};

struct HostMessage final {
    product::Uuid request_id;
    product::Uuid operation_id;
    std::uint32_t command_id{};
    std::uint32_t flags{};
    std::vector<std::byte> payload;

    bool operator==(const HostMessage&) const noexcept = default;
};

struct AuthenticatedHostFrame final {
    WorkerBinding binding;
    MessageDirection direction{};
    std::uint64_t sequence{};
    HostMessage message;
    AuthenticationTag authentication_tag{};

    bool operator==(const AuthenticatedHostFrame&) const noexcept = default;
};

// Command-specific private payload codecs. The existing product Frame remains
// the outer envelope; these functions encode every inner field explicitly and
// never cast a C++ struct onto the wire.
Result<std::vector<std::byte>> encode_client_hello(const ClientHello& value);
Result<ClientHello> decode_client_hello(std::span<const std::byte> bytes);
Result<std::vector<std::byte>> encode_worker_challenge(
    const WorkerChallenge& value);
Result<WorkerChallenge> decode_worker_challenge(
    std::span<const std::byte> bytes);
Result<std::vector<std::byte>> encode_service_proof(const ServiceProof& value);
Result<ServiceProof> decode_service_proof(std::span<const std::byte> bytes);
Result<std::vector<std::byte>> encode_authenticated_host_frame(
    const AuthenticatedHostFrame& value);
Result<AuthenticatedHostFrame> decode_authenticated_host_frame(
    std::span<const std::byte> bytes);

// Worker-side two-phase reattachment credentials. While a rotation is staged,
// both exact bindings have usable secrets so a service crash on either side of
// runtime.lease publication remains reconcilable. Commit promotes only the
// exact staged binding and securely destroys the previous credential.
class ReattachmentCredentialWindow final {
public:
    static Result<ReattachmentCredentialWindow> begin(
        WorkerBinding current_binding,
        SessionSecret current_secret);

    ReattachmentCredentialWindow(ReattachmentCredentialWindow&&) noexcept =
        default;
    ReattachmentCredentialWindow& operator=(
        ReattachmentCredentialWindow&&) noexcept = default;
    ReattachmentCredentialWindow(const ReattachmentCredentialWindow&) = delete;
    ReattachmentCredentialWindow& operator=(
        const ReattachmentCredentialWindow&) = delete;

    const WorkerBinding& current_binding() const noexcept;
    const WorkerBinding* pending_binding() const noexcept;
    bool accepts(const WorkerBinding& binding) const noexcept;
    Result<SessionSecret> credential_for(
        const WorkerBinding& binding) const;
    Result<void> stage(
        WorkerBinding next_binding,
        SessionSecret next_secret);
    Result<void> commit(const WorkerBinding& committed_binding);

private:
    struct Credential final {
        WorkerBinding binding;
        SessionSecret secret;

        Credential(
            WorkerBinding binding_value,
            SessionSecret secret_value) noexcept;
        Credential(Credential&&) noexcept = default;
        Credential& operator=(Credential&&) noexcept = default;
        Credential(const Credential&) = delete;
        Credential& operator=(const Credential&) = delete;
    };

    ReattachmentCredentialWindow(
        WorkerBinding current_binding,
        SessionSecret current_secret) noexcept;

    Credential current_;
    std::optional<Credential> pending_;
};

enum class SessionRole : std::uint8_t { service = 1, worker = 2 };

class SessionAuthenticator final {
public:
    ~SessionAuthenticator() noexcept;
    SessionAuthenticator(SessionAuthenticator&& other) noexcept;
    SessionAuthenticator& operator=(SessionAuthenticator&& other) noexcept;
    SessionAuthenticator(const SessionAuthenticator&) = delete;
    SessionAuthenticator& operator=(const SessionAuthenticator&) = delete;

    const WorkerBinding& binding() const noexcept;
    Result<AuthenticatedHostFrame> seal(const HostMessage& message);
    Result<HostMessage> open(const AuthenticatedHostFrame& frame);

private:
    SessionAuthenticator(
        WorkerBinding binding,
        SessionRole role,
        AuthenticationTag service_to_worker_key,
        AuthenticationTag worker_to_service_key) noexcept;
    void clear() noexcept;

    WorkerBinding binding_;
    SessionRole role_{};
    AuthenticationTag service_to_worker_key_{};
    AuthenticationTag worker_to_service_key_{};
    std::uint64_t next_send_sequence_{1};
    std::uint64_t next_receive_sequence_{1};
    bool valid_{};

    friend class ServiceHandshake;
    friend class WorkerHandshake;
};

struct ServiceHandshakeAcceptance final {
    ServiceProof proof;
    SessionAuthenticator session;

    ServiceHandshakeAcceptance(
        ServiceProof proof_value,
        SessionAuthenticator session_value) noexcept;
    ServiceHandshakeAcceptance(ServiceHandshakeAcceptance&&) noexcept = default;
    ServiceHandshakeAcceptance& operator=(
        ServiceHandshakeAcceptance&&) noexcept = default;
    ServiceHandshakeAcceptance(const ServiceHandshakeAcceptance&) = delete;
    ServiceHandshakeAcceptance& operator=(
        const ServiceHandshakeAcceptance&) = delete;
};

class ServiceHandshake final {
public:
    static Result<ServiceHandshake> begin(
        WorkerBinding expected,
        SessionSecret secret,
        HandshakeNonce service_nonce);

    ~ServiceHandshake() noexcept = default;
    ServiceHandshake(ServiceHandshake&&) noexcept = default;
    ServiceHandshake& operator=(ServiceHandshake&&) noexcept = default;
    ServiceHandshake(const ServiceHandshake&) = delete;
    ServiceHandshake& operator=(const ServiceHandshake&) = delete;

    const ClientHello& hello() const noexcept;
    Result<ServiceHandshakeAcceptance> accept_challenge(
        const WorkerChallenge& challenge);

private:
    ServiceHandshake(
        WorkerBinding expected,
        SessionSecret secret,
        HandshakeNonce service_nonce) noexcept;

    WorkerBinding expected_;
    SessionSecret secret_;
    ClientHello hello_;
    bool consumed_{};
};

class WorkerHandshake final {
public:
    static Result<WorkerHandshake> begin(
        WorkerBinding expected,
        SessionSecret secret);

    ~WorkerHandshake() noexcept = default;
    WorkerHandshake(WorkerHandshake&&) noexcept = default;
    WorkerHandshake& operator=(WorkerHandshake&&) noexcept = default;
    WorkerHandshake(const WorkerHandshake&) = delete;
    WorkerHandshake& operator=(const WorkerHandshake&) = delete;

    Result<WorkerChallenge> accept_hello(
        const ClientHello& hello,
        HandshakeNonce worker_nonce);
    Result<SessionAuthenticator> accept_proof(const ServiceProof& proof);

private:
    WorkerHandshake(
        WorkerBinding expected,
        SessionSecret secret) noexcept;

    WorkerBinding expected_;
    SessionSecret secret_;
    std::optional<ClientHello> hello_;
    std::optional<WorkerChallenge> challenge_;
    bool challenge_issued_{};
    bool proof_consumed_{};
    bool authenticated_{};
};

} // namespace myrd::host
