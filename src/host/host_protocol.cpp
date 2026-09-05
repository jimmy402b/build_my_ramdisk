#include "src/host/host_protocol.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace myrd::host {
namespace {

template <class T>
Result<T> protocol_error(
    HostProtocolError code,
    const wchar_t* operation,
    const wchar_t* message,
    std::uint32_t native_code = 0) {
    return Result<T>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::ntstatus,
        static_cast<std::uint32_t>(code),
        operation,
        message,
        native_code});
}

void secure_zero(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        static_cast<void>(SecureZeroMemory(bytes.data(), bytes.size()));
    }
}

bool all_zero(std::span<const std::byte> bytes) noexcept {
    std::byte combined{};
    for (const auto value : bytes) combined |= value;
    return combined == std::byte{};
}

bool constant_time_equal(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) return false;
    std::byte difference{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == std::byte{};
}

void append_bytes(
    std::vector<std::byte>& output,
    std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void append_domain(
    std::vector<std::byte>& output,
    std::string_view domain) {
    for (const auto value : domain) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    output.push_back(std::byte{});
}

template <class T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffU));
        value >>= 8U;
    }
}

void append_uuid(
    std::vector<std::byte>& output,
    const product::Uuid& value) {
    append_bytes(output, value.bytes());
}

void append_binding(
    std::vector<std::byte>& output,
    const WorkerBinding& binding) {
    append_le(output, static_cast<std::uint16_t>(kHostProtocolMajor));
    append_le(output, static_cast<std::uint16_t>(kHostProtocolMinor));
    append_le(output, binding.product_major);
    append_le(output, binding.product_minor);
    append_le(output, binding.product_patch);
    append_uuid(output, binding.profile_id);
    append_uuid(output, binding.image_id);
    append_le(output, binding.worker_pid);
    append_le(output, binding.process_creation_time);
    append_le(output, binding.session_generation);
    append_le(output, binding.profile_revision);
}

Result<AuthenticationTag> hmac_sha256(
    std::span<const std::byte> key,
    std::span<const std::byte> data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) {
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.hmac.open",
            L"Failed to open HMAC-SHA256 provider",
            static_cast<std::uint32_t>(status));
    }

    DWORD object_bytes = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_bytes),
        sizeof(object_bytes),
        &copied,
        0);
    if (status < 0 || copied != sizeof(object_bytes)) {
        static_cast<void>(BCryptCloseAlgorithmProvider(algorithm, 0));
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.hmac.properties",
            L"Failed to query HMAC provider",
            static_cast<std::uint32_t>(status));
    }

    std::vector<unsigned char> object;
    try {
        object.resize(object_bytes);
    } catch (...) {
        static_cast<void>(BCryptCloseAlgorithmProvider(algorithm, 0));
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.hmac.allocate",
            L"Failed to allocate bounded HMAC state");
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(
        algorithm,
        &hash,
        object.data(),
        static_cast<ULONG>(object.size()),
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
        static_cast<ULONG>(key.size()),
        0);
    if (status >= 0) {
        status = BCryptHashData(
            hash,
            const_cast<PUCHAR>(
                reinterpret_cast<const UCHAR*>(data.data())),
            static_cast<ULONG>(data.size()),
            0);
    }
    AuthenticationTag result{};
    if (status >= 0) {
        status = BCryptFinishHash(
            hash,
            reinterpret_cast<PUCHAR>(result.data()),
            static_cast<ULONG>(result.size()),
            0);
    }
    if (hash != nullptr) static_cast<void>(BCryptDestroyHash(hash));
    static_cast<void>(BCryptCloseAlgorithmProvider(algorithm, 0));
    secure_zero(std::as_writable_bytes(std::span(object)));
    if (status < 0) {
        secure_zero(result);
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.hmac.compute",
            L"Failed to compute HMAC-SHA256",
            static_cast<std::uint32_t>(status));
    }
    auto wrapped = Result<AuthenticationTag>::success(result);
    secure_zero(result);
    return wrapped;
}

Result<AuthenticationTag> proof_tag(
    std::string_view domain,
    const SessionSecret& secret,
    const WorkerBinding& binding,
    const HandshakeNonce& service_nonce,
    const HandshakeNonce& worker_nonce) {
    std::vector<std::byte> input;
    try {
        input.reserve(160);
        append_domain(input, domain);
        append_binding(input, binding);
        append_bytes(input, service_nonce);
        append_bytes(input, worker_nonce);
    } catch (...) {
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.proof",
            L"Failed to allocate bounded proof input");
    }
    return hmac_sha256(secret.bytes(), input);
}

struct ConnectionKeys final {
    AuthenticationTag service_to_worker{};
    AuthenticationTag worker_to_service{};

    ConnectionKeys(
        AuthenticationTag service_key,
        AuthenticationTag worker_key) noexcept
        : service_to_worker(service_key), worker_to_service(worker_key) {
        secure_zero(service_key);
        secure_zero(worker_key);
    }
    ~ConnectionKeys() noexcept {
        secure_zero(service_to_worker);
        secure_zero(worker_to_service);
    }
    ConnectionKeys(ConnectionKeys&& other) noexcept
        : service_to_worker(other.service_to_worker),
          worker_to_service(other.worker_to_service) {
        secure_zero(other.service_to_worker);
        secure_zero(other.worker_to_service);
    }
    ConnectionKeys& operator=(ConnectionKeys&& other) noexcept {
        if (this != &other) {
            secure_zero(service_to_worker);
            secure_zero(worker_to_service);
            service_to_worker = other.service_to_worker;
            worker_to_service = other.worker_to_service;
            secure_zero(other.service_to_worker);
            secure_zero(other.worker_to_service);
        }
        return *this;
    }
    ConnectionKeys(const ConnectionKeys&) = delete;
    ConnectionKeys& operator=(const ConnectionKeys&) = delete;
};

Result<ConnectionKeys> derive_connection_keys(
    const SessionSecret& secret,
    const WorkerBinding& binding,
    const HandshakeNonce& service_nonce,
    const HandshakeNonce& worker_nonce) {
    std::vector<std::byte> transcript;
    try {
        transcript.reserve(160);
        append_domain(transcript, "MYRD-HOST-CONNECTION-v1");
        append_binding(transcript, binding);
        append_bytes(transcript, service_nonce);
        append_bytes(transcript, worker_nonce);
    } catch (...) {
        return protocol_error<ConnectionKeys>(
            HostProtocolError::crypto_failure,
            L"host_protocol.derive",
            L"Failed to allocate bounded key transcript");
    }
    auto base = hmac_sha256(secret.bytes(), transcript);
    if (!base.ok()) return Result<ConnectionKeys>::failure(base.error());

    std::array<std::byte, 25> service_label{};
    constexpr std::string_view kServiceLabel = "MYRD-HOST-S2W-KEY-v1";
    for (std::size_t index = 0; index < kServiceLabel.size(); ++index) {
        service_label[index] = static_cast<std::byte>(
            static_cast<unsigned char>(kServiceLabel[index]));
    }
    std::array<std::byte, 25> worker_label{};
    constexpr std::string_view kWorkerLabel = "MYRD-HOST-W2S-KEY-v1";
    for (std::size_t index = 0; index < kWorkerLabel.size(); ++index) {
        worker_label[index] = static_cast<std::byte>(
            static_cast<unsigned char>(kWorkerLabel[index]));
    }
    auto service_key = hmac_sha256(base.value(), service_label);
    auto worker_key = hmac_sha256(base.value(), worker_label);
    secure_zero(base.value());
    if (!service_key.ok()) {
        return Result<ConnectionKeys>::failure(service_key.error());
    }
    if (!worker_key.ok()) {
        secure_zero(service_key.value());
        return Result<ConnectionKeys>::failure(worker_key.error());
    }
    ConnectionKeys keys(service_key.value(), worker_key.value());
    secure_zero(service_key.value());
    secure_zero(worker_key.value());
    return Result<ConnectionKeys>::success(std::move(keys));
}

Result<AuthenticationTag> frame_tag(
    std::span<const std::byte> key,
    const AuthenticatedHostFrame& frame) {
    if (frame.message.payload.size() > kMaximumHostPayloadBytes) {
        return protocol_error<AuthenticationTag>(
            HostProtocolError::payload_too_large,
            L"host_protocol.frame_mac",
            L"Private host payload exceeds the fixed bound");
    }
    std::vector<std::byte> input;
    try {
        input.reserve(160 + frame.message.payload.size());
        append_domain(input, "MYRD-HOST-MESSAGE-v1");
        append_binding(input, frame.binding);
        append_le(input, static_cast<std::uint8_t>(frame.direction));
        append_le(input, frame.sequence);
        append_uuid(input, frame.message.request_id);
        append_uuid(input, frame.message.operation_id);
        append_le(input, frame.message.command_id);
        append_le(input, frame.message.flags);
        append_le(
            input,
            static_cast<std::uint32_t>(frame.message.payload.size()));
        append_bytes(input, frame.message.payload);
    } catch (...) {
        return protocol_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_protocol.frame_mac",
            L"Failed to allocate bounded private-frame input");
    }
    return hmac_sha256(key, input);
}

inline constexpr std::size_t kWireUuidBytes = 36;
inline constexpr std::size_t kWireBindingBytes = 112;
inline constexpr std::size_t kClientHelloWireBytes = 144;
inline constexpr std::size_t kChallengeWireBytes = 208;
inline constexpr std::size_t kAuthenticatedPrefixBytes = 212;

void append_wire_uuid(
    std::vector<std::byte>& output,
    const product::Uuid& value) {
    const auto text = value.to_string();
    if (text.size() != kWireUuidBytes) throw std::bad_alloc();
    for (const auto character : text) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
}

void append_wire_binding(
    std::vector<std::byte>& output,
    const WorkerBinding& binding) {
    append_wire_uuid(output, binding.profile_id);
    append_wire_uuid(output, binding.image_id);
    append_le(output, binding.worker_pid);
    append_le(output, binding.process_creation_time);
    append_le(output, binding.session_generation);
    append_le(output, binding.profile_revision);
    append_le(output, binding.product_major);
    append_le(output, binding.product_minor);
    append_le(output, binding.product_patch);
}

template <class T>
T read_wire_le(
    std::span<const std::byte> input,
    std::size_t& offset) noexcept {
    static_assert(std::is_unsigned_v<T>);
    T value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(
            input[offset++])) << (index * 8U);
    }
    return value;
}

Result<product::Uuid> read_wire_uuid(
    std::span<const std::byte> input,
    std::size_t& offset,
    const wchar_t* operation) {
    if (offset > input.size()
        || input.size() - offset < kWireUuidBytes) {
        return protocol_error<product::Uuid>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private protocol UUID is truncated");
    }
    std::array<char, kWireUuidBytes> text{};
    for (auto& value : text) {
        value = static_cast<char>(std::to_integer<unsigned char>(
            input[offset++]));
    }
    auto parsed = product::Uuid::parse(
        std::string_view(text.data(), text.size()));
    if (!parsed.ok()) {
        return protocol_error<product::Uuid>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private protocol UUID is noncanonical");
    }
    return parsed;
}

Result<WorkerBinding> read_wire_binding(
    std::span<const std::byte> input,
    std::size_t& offset,
    const wchar_t* operation) {
    if (offset > input.size()
        || input.size() - offset < kWireBindingBytes) {
        return protocol_error<WorkerBinding>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private protocol worker binding is truncated");
    }
    auto profile = read_wire_uuid(input, offset, operation);
    auto image = read_wire_uuid(input, offset, operation);
    if (!profile.ok() || !image.ok()) {
        return protocol_error<WorkerBinding>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private protocol worker binding UUID is malformed");
    }
    WorkerBinding binding{
        profile.move_value(),
        image.move_value(),
        read_wire_le<std::uint32_t>(input, offset),
        read_wire_le<std::uint64_t>(input, offset),
        read_wire_le<std::uint64_t>(input, offset),
        read_wire_le<std::uint64_t>(input, offset),
        read_wire_le<std::uint32_t>(input, offset),
        read_wire_le<std::uint32_t>(input, offset),
        read_wire_le<std::uint32_t>(input, offset)};
    if (!is_valid(binding)) {
        return protocol_error<WorkerBinding>(
            HostProtocolError::version_mismatch,
            operation,
            L"Private protocol worker binding or product version is invalid");
    }
    return Result<WorkerBinding>::success(std::move(binding));
}

template <class T>
Result<std::vector<std::byte>> encode_handshake_record(
    const T& value,
    const wchar_t* operation) {
    if (!is_valid(value.binding)) {
        return protocol_error<std::vector<std::byte>>(
            HostProtocolError::version_mismatch,
            operation,
            L"Private handshake binding is invalid");
    }
    std::vector<std::byte> bytes;
    try {
        if constexpr (std::is_same_v<T, ClientHello>) {
            if (all_zero(value.service_nonce)) {
                return protocol_error<std::vector<std::byte>>(
                    HostProtocolError::invalid_argument,
                    operation,
                    L"Private client hello nonce is invalid");
            }
            bytes.reserve(kClientHelloWireBytes);
            append_wire_binding(bytes, value.binding);
            append_bytes(bytes, value.service_nonce);
        } else {
            if (all_zero(value.service_nonce)
                || all_zero(value.worker_nonce)
                || value.service_nonce == value.worker_nonce
                || all_zero(value.proof)) {
                return protocol_error<std::vector<std::byte>>(
                    HostProtocolError::invalid_argument,
                    operation,
                    L"Private handshake nonce or proof is invalid");
            }
            bytes.reserve(kChallengeWireBytes);
            append_wire_binding(bytes, value.binding);
            append_bytes(bytes, value.service_nonce);
            append_bytes(bytes, value.worker_nonce);
            append_bytes(bytes, value.proof);
        }
    } catch (...) {
        return protocol_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            operation,
            L"Failed to allocate bounded private handshake record");
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

template <class T>
Result<T> decode_proof_record(
    std::span<const std::byte> bytes,
    const wchar_t* operation) {
    if (bytes.size() != kChallengeWireBytes) {
        return protocol_error<T>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private handshake proof has an invalid exact length");
    }
    std::size_t offset = 0;
    auto binding = read_wire_binding(bytes, offset, operation);
    if (!binding.ok()) return Result<T>::failure(binding.error());
    HandshakeNonce service_nonce{};
    HandshakeNonce worker_nonce{};
    AuthenticationTag proof{};
    std::copy_n(
        bytes.begin() + offset, service_nonce.size(), service_nonce.begin());
    offset += service_nonce.size();
    std::copy_n(
        bytes.begin() + offset, worker_nonce.size(), worker_nonce.begin());
    offset += worker_nonce.size();
    std::copy_n(bytes.begin() + offset, proof.size(), proof.begin());
    offset += proof.size();
    if (offset != bytes.size() || all_zero(service_nonce)
        || all_zero(worker_nonce) || service_nonce == worker_nonce
        || all_zero(proof)) {
        return protocol_error<T>(
            HostProtocolError::invalid_argument,
            operation,
            L"Private handshake proof fields are invalid");
    }
    return Result<T>::success(T{
        binding.move_value(), service_nonce, worker_nonce, proof});
}

} // namespace

Result<HandshakeNonce> generate_handshake_nonce() {
    HandshakeNonce nonce{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(nonce.data()),
        static_cast<ULONG>(nonce.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0 || all_zero(nonce)) {
        secure_zero(nonce);
        return protocol_error<HandshakeNonce>(
            HostProtocolError::crypto_failure,
            L"host_protocol.nonce_generate",
            L"Failed to generate a 256-bit handshake nonce",
            static_cast<std::uint32_t>(status));
    }
    auto wrapped = Result<HandshakeNonce>::success(nonce);
    secure_zero(nonce);
    return wrapped;
}

Result<std::vector<std::byte>> encode_client_hello(const ClientHello& value) {
    return encode_handshake_record(value, L"host_protocol.hello_encode");
}

Result<ClientHello> decode_client_hello(std::span<const std::byte> bytes) {
    if (bytes.size() != kClientHelloWireBytes) {
        return protocol_error<ClientHello>(
            HostProtocolError::invalid_argument,
            L"host_protocol.hello_decode",
            L"Private client hello has an invalid exact length");
    }
    std::size_t offset = 0;
    auto binding = read_wire_binding(
        bytes, offset, L"host_protocol.hello_decode");
    if (!binding.ok()) return Result<ClientHello>::failure(binding.error());
    HandshakeNonce service_nonce{};
    std::copy_n(
        bytes.begin() + offset, service_nonce.size(), service_nonce.begin());
    offset += service_nonce.size();
    if (offset != bytes.size() || all_zero(service_nonce)) {
        return protocol_error<ClientHello>(
            HostProtocolError::invalid_argument,
            L"host_protocol.hello_decode",
            L"Private client hello nonce is invalid");
    }
    return Result<ClientHello>::success(ClientHello{
        binding.move_value(), service_nonce});
}

Result<std::vector<std::byte>> encode_worker_challenge(
    const WorkerChallenge& value) {
    return encode_handshake_record(
        value, L"host_protocol.challenge_encode");
}

Result<WorkerChallenge> decode_worker_challenge(
    std::span<const std::byte> bytes) {
    return decode_proof_record<WorkerChallenge>(
        bytes, L"host_protocol.challenge_decode");
}

Result<std::vector<std::byte>> encode_service_proof(
    const ServiceProof& value) {
    return encode_handshake_record(value, L"host_protocol.proof_encode");
}

Result<ServiceProof> decode_service_proof(
    std::span<const std::byte> bytes) {
    return decode_proof_record<ServiceProof>(
        bytes, L"host_protocol.proof_decode");
}

Result<std::vector<std::byte>> encode_authenticated_host_frame(
    const AuthenticatedHostFrame& value) {
    if (!is_valid(value.binding)
        || (value.direction != MessageDirection::service_to_worker
            && value.direction != MessageDirection::worker_to_service)
        || value.sequence == 0 || value.message.command_id == 0
        || value.message.payload.size() > kMaximumHostPayloadBytes
        || all_zero(value.authentication_tag)) {
        return protocol_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_encode",
            L"Authenticated private frame fields are invalid");
    }
    std::vector<std::byte> bytes;
    try {
        bytes.reserve(
            kAuthenticatedHostWireOverheadBytes
            + value.message.payload.size());
        append_wire_binding(bytes, value.binding);
        bytes.push_back(static_cast<std::byte>(value.direction));
        for (std::size_t index = 0; index < 7; ++index) {
            bytes.push_back(std::byte{});
        }
        append_le(bytes, value.sequence);
        append_wire_uuid(bytes, value.message.request_id);
        append_wire_uuid(bytes, value.message.operation_id);
        append_le(bytes, value.message.command_id);
        append_le(bytes, value.message.flags);
        append_le(
            bytes,
            static_cast<std::uint32_t>(value.message.payload.size()));
        append_bytes(bytes, value.message.payload);
        append_bytes(bytes, value.authentication_tag);
    } catch (...) {
        return protocol_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_encode",
            L"Failed to allocate bounded authenticated frame bytes");
    }
    if (bytes.size() + ipc::kFrameHeaderBytes > ipc::kMaxPayloadBytes) {
        return protocol_error<std::vector<std::byte>>(
            HostProtocolError::payload_too_large,
            L"host_protocol.frame_encode",
            L"Authenticated frame exceeds the product frame bound");
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<AuthenticatedHostFrame> decode_authenticated_host_frame(
    std::span<const std::byte> bytes) {
    if (bytes.size() < kAuthenticatedHostWireOverheadBytes
        || bytes.size() + ipc::kFrameHeaderBytes
            > ipc::kMaxPayloadBytes) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Authenticated frame has an invalid bounded length");
    }
    std::size_t offset = 0;
    auto binding = read_wire_binding(
        bytes, offset, L"host_protocol.frame_decode");
    if (!binding.ok()) {
        return Result<AuthenticatedHostFrame>::failure(binding.error());
    }
    const auto direction = static_cast<MessageDirection>(
        std::to_integer<std::uint8_t>(bytes[offset++]));
    if (direction != MessageDirection::service_to_worker
        && direction != MessageDirection::worker_to_service) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Authenticated frame direction is invalid");
    }
    for (std::size_t index = 0; index < 7; ++index) {
        if (bytes[offset++] != std::byte{}) {
            return protocol_error<AuthenticatedHostFrame>(
                HostProtocolError::invalid_argument,
                L"host_protocol.frame_decode",
                L"Authenticated frame reserved bytes are nonzero");
        }
    }
    const auto sequence = read_wire_le<std::uint64_t>(bytes, offset);
    auto request = read_wire_uuid(
        bytes, offset, L"host_protocol.frame_decode");
    auto operation = read_wire_uuid(
        bytes, offset, L"host_protocol.frame_decode");
    if (!request.ok() || !operation.ok()) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Authenticated frame correlation UUID is invalid");
    }
    const auto command = read_wire_le<std::uint32_t>(bytes, offset);
    const auto flags = read_wire_le<std::uint32_t>(bytes, offset);
    const auto payload_bytes = read_wire_le<std::uint32_t>(bytes, offset);
    if (offset != kAuthenticatedPrefixBytes
        || sequence == 0 || command == 0
        || payload_bytes > kMaximumHostPayloadBytes
        || static_cast<std::size_t>(payload_bytes)
            + kAuthenticatedHostWireOverheadBytes != bytes.size()) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Authenticated frame sequence, command or payload length is invalid");
    }
    std::vector<std::byte> payload;
    try {
        payload.assign(
            bytes.begin() + offset,
            bytes.begin() + offset + payload_bytes);
    } catch (...) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Failed to allocate bounded authenticated payload");
    }
    offset += payload_bytes;
    AuthenticationTag tag{};
    std::copy_n(bytes.begin() + offset, tag.size(), tag.begin());
    offset += tag.size();
    if (offset != bytes.size() || all_zero(tag)) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.frame_decode",
            L"Authenticated frame tag or trailing bytes are invalid");
    }
    return Result<AuthenticatedHostFrame>::success(AuthenticatedHostFrame{
        binding.move_value(),
        direction,
        sequence,
        HostMessage{
            request.move_value(), operation.move_value(),
            command, flags, std::move(payload)},
        tag});
}

ReattachmentCredentialWindow::Credential::Credential(
    WorkerBinding binding_value,
    SessionSecret secret_value) noexcept
    : binding(std::move(binding_value)), secret(std::move(secret_value)) {}

ReattachmentCredentialWindow::ReattachmentCredentialWindow(
    WorkerBinding current_binding,
    SessionSecret current_secret) noexcept
    : current_(std::move(current_binding), std::move(current_secret)) {}

Result<ReattachmentCredentialWindow> ReattachmentCredentialWindow::begin(
    WorkerBinding current_binding,
    SessionSecret current_secret) {
    if (!is_valid(current_binding) || all_zero(current_secret.bytes())) {
        return protocol_error<ReattachmentCredentialWindow>(
            HostProtocolError::lease_binding_mismatch,
            L"host_protocol.rotation_begin",
            L"Initial reattachment credential binding is invalid");
    }
    return Result<ReattachmentCredentialWindow>::success(
        ReattachmentCredentialWindow(
            std::move(current_binding), std::move(current_secret)));
}

const WorkerBinding& ReattachmentCredentialWindow::current_binding()
    const noexcept {
    return current_.binding;
}

const WorkerBinding* ReattachmentCredentialWindow::pending_binding()
    const noexcept {
    return pending_.has_value() ? &pending_->binding : nullptr;
}

bool ReattachmentCredentialWindow::accepts(
    const WorkerBinding& binding) const noexcept {
    return binding == current_.binding
        || (pending_.has_value() && binding == pending_->binding);
}

Result<SessionSecret> ReattachmentCredentialWindow::credential_for(
    const WorkerBinding& binding) const {
    const SessionSecret* source = nullptr;
    if (binding == current_.binding) {
        source = &current_.secret;
    } else if (pending_.has_value() && binding == pending_->binding) {
        source = &pending_->secret;
    }
    if (source == nullptr) {
        return protocol_error<SessionSecret>(
            HostProtocolError::lease_binding_mismatch,
            L"host_protocol.rotation_select",
            L"Reattachment credential binding is not current or staged");
    }
    SecretBytes copy{};
    std::copy(source->bytes().begin(), source->bytes().end(), copy.begin());
    auto result = SessionSecret::from_bytes(copy);
    secure_zero(copy);
    return result;
}

Result<void> ReattachmentCredentialWindow::stage(
    WorkerBinding next_binding,
    SessionSecret next_secret) {
    const auto& current = current_.binding;
    const bool exact_identity = is_valid(next_binding)
        && next_binding.profile_id == current.profile_id
        && next_binding.image_id == current.image_id
        && next_binding.worker_pid == current.worker_pid
        && next_binding.process_creation_time == current.process_creation_time
        && next_binding.product_major == current.product_major
        && next_binding.product_minor == current.product_minor
        && next_binding.product_patch == current.product_patch;
    if (pending_.has_value() || !exact_identity
        || all_zero(next_secret.bytes())
        || current.session_generation
            == (std::numeric_limits<std::uint64_t>::max)()
        || next_binding.session_generation != current.session_generation + 1
        || next_binding.profile_revision < current.profile_revision) {
        return protocol_error<void>(
            HostProtocolError::lease_binding_mismatch,
            L"host_protocol.rotation_stage",
            L"Reattachment credential is not the exact monotonic successor");
    }
    pending_.emplace(
        std::move(next_binding), std::move(next_secret));
    return Result<void>::success();
}

Result<void> ReattachmentCredentialWindow::commit(
    const WorkerBinding& committed_binding) {
    if (!pending_.has_value()
        || committed_binding != pending_->binding) {
        return protocol_error<void>(
            HostProtocolError::lease_binding_mismatch,
            L"host_protocol.rotation_commit",
            L"Reattachment commit does not match the staged credential");
    }
    current_ = std::move(pending_.value());
    pending_.reset();
    return Result<void>::success();
}

Result<std::wstring> private_pipe_name(const product::Uuid& profile_id) {
    if (all_zero(profile_id.bytes())) {
        return protocol_error<std::wstring>(
            HostProtocolError::invalid_argument,
            L"host_protocol.pipe_name",
            L"Private pipe profile UUID must be nonzero");
    }
    try {
        const auto narrow = profile_id.to_string();
        std::wstring name(kPrivatePipePrefix);
        name.append(narrow.begin(), narrow.end());
        return Result<std::wstring>::success(std::move(name));
    } catch (...) {
        return protocol_error<std::wstring>(
            HostProtocolError::invalid_argument,
            L"host_protocol.pipe_name",
            L"Failed to construct the bounded private pipe name");
    }
}

bool is_valid(const WorkerBinding& binding) noexcept {
    return !all_zero(binding.profile_id.bytes())
        && !all_zero(binding.image_id.bytes())
        && binding.worker_pid != 0
        && binding.process_creation_time != 0
        && binding.session_generation != 0
        && binding.profile_revision != 0
        && binding.product_major == product::kProductVersionMajor
        && binding.product_minor == product::kProductVersionMinor
        && binding.product_patch == product::kProductVersionPatch;
}

SessionSecret::SessionSecret(const SecretBytes& bytes) noexcept
    : bytes_(bytes), valid_(true) {}

Result<SessionSecret> SessionSecret::from_bytes(const SecretBytes& bytes) {
    if (all_zero(bytes)) {
        return protocol_error<SessionSecret>(
            HostProtocolError::invalid_argument,
            L"host_protocol.secret",
            L"A session secret must not be all zero");
    }
    return Result<SessionSecret>::success(SessionSecret(bytes));
}

Result<SessionSecret> SessionSecret::generate() {
    SecretBytes bytes{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        secure_zero(bytes);
        return protocol_error<SessionSecret>(
            HostProtocolError::crypto_failure,
            L"host_protocol.secret_generate",
            L"Failed to generate a 256-bit worker session secret",
            static_cast<std::uint32_t>(status));
    }
    auto result = from_bytes(bytes);
    secure_zero(bytes);
    return result;
}

SessionSecret::~SessionSecret() noexcept { clear(); }

SessionSecret::SessionSecret(SessionSecret&& other) noexcept
    : bytes_(other.bytes_), valid_(other.valid_) {
    other.clear();
}

SessionSecret& SessionSecret::operator=(SessionSecret&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = other.bytes_;
        valid_ = other.valid_;
        other.clear();
    }
    return *this;
}

std::span<const std::byte, kSessionSecretBytes>
SessionSecret::bytes() const noexcept {
    return bytes_;
}

void SessionSecret::clear() noexcept {
    secure_zero(bytes_);
    valid_ = false;
}

SessionAuthenticator::SessionAuthenticator(
    WorkerBinding binding,
    SessionRole role,
    AuthenticationTag service_to_worker_key,
    AuthenticationTag worker_to_service_key) noexcept
    : binding_(std::move(binding)),
      role_(role),
      service_to_worker_key_(service_to_worker_key),
      worker_to_service_key_(worker_to_service_key),
      valid_(true) {
    secure_zero(service_to_worker_key);
    secure_zero(worker_to_service_key);
}

SessionAuthenticator::~SessionAuthenticator() noexcept { clear(); }

SessionAuthenticator::SessionAuthenticator(
    SessionAuthenticator&& other) noexcept
    : binding_(other.binding_),
      role_(other.role_),
      service_to_worker_key_(other.service_to_worker_key_),
      worker_to_service_key_(other.worker_to_service_key_),
      next_send_sequence_(other.next_send_sequence_),
      next_receive_sequence_(other.next_receive_sequence_),
      valid_(other.valid_) {
    other.clear();
}

SessionAuthenticator& SessionAuthenticator::operator=(
    SessionAuthenticator&& other) noexcept {
    if (this != &other) {
        clear();
        binding_ = other.binding_;
        role_ = other.role_;
        service_to_worker_key_ = other.service_to_worker_key_;
        worker_to_service_key_ = other.worker_to_service_key_;
        next_send_sequence_ = other.next_send_sequence_;
        next_receive_sequence_ = other.next_receive_sequence_;
        valid_ = other.valid_;
        other.clear();
    }
    return *this;
}

const WorkerBinding& SessionAuthenticator::binding() const noexcept {
    return binding_;
}

Result<AuthenticatedHostFrame> SessionAuthenticator::seal(
    const HostMessage& message) {
    if (!valid_ || message.command_id == 0) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::invalid_argument,
            L"host_protocol.seal",
            L"Private message state or command is invalid");
    }
    if (message.payload.size() > kMaximumHostPayloadBytes) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::payload_too_large,
            L"host_protocol.seal",
            L"Private message payload exceeds the fixed bound");
    }
    if (next_send_sequence_ == 0
        || next_send_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return protocol_error<AuthenticatedHostFrame>(
            HostProtocolError::sequence_exhausted,
            L"host_protocol.seal",
            L"Private message sequence is exhausted");
    }
    const auto direction = role_ == SessionRole::service
        ? MessageDirection::service_to_worker
        : MessageDirection::worker_to_service;
    AuthenticatedHostFrame frame{
        binding_, direction, next_send_sequence_, message, {}};
    const auto& key = direction == MessageDirection::service_to_worker
        ? service_to_worker_key_ : worker_to_service_key_;
    auto tag = frame_tag(key, frame);
    if (!tag.ok()) {
        return Result<AuthenticatedHostFrame>::failure(tag.error());
    }
    frame.authentication_tag = tag.move_value();
    ++next_send_sequence_;
    return Result<AuthenticatedHostFrame>::success(std::move(frame));
}

Result<HostMessage> SessionAuthenticator::open(
    const AuthenticatedHostFrame& frame) {
    const auto expected_direction = role_ == SessionRole::service
        ? MessageDirection::worker_to_service
        : MessageDirection::service_to_worker;
    if (!valid_ || !is_valid(frame.binding)
        || frame.binding != binding_
        || frame.direction != expected_direction) {
        return protocol_error<HostMessage>(
            HostProtocolError::binding_mismatch,
            L"host_protocol.open",
            L"Private message binding or direction is invalid");
    }
    if (frame.sequence != next_receive_sequence_) {
        return protocol_error<HostMessage>(
            HostProtocolError::sequence_mismatch,
            L"host_protocol.open",
            L"Private message sequence is not the exact next value");
    }
    if (frame.message.command_id == 0
        || frame.message.payload.size() > kMaximumHostPayloadBytes) {
        return protocol_error<HostMessage>(
            frame.message.payload.size() > kMaximumHostPayloadBytes
                ? HostProtocolError::payload_too_large
                : HostProtocolError::invalid_argument,
            L"host_protocol.open",
            L"Private message command or payload is invalid");
    }
    const auto& key = expected_direction
            == MessageDirection::service_to_worker
        ? service_to_worker_key_ : worker_to_service_key_;
    auto expected = frame_tag(key, frame);
    if (!expected.ok()) return Result<HostMessage>::failure(expected.error());
    const bool matched = constant_time_equal(
        expected.value(), frame.authentication_tag);
    secure_zero(expected.value());
    if (!matched) {
        return protocol_error<HostMessage>(
            HostProtocolError::authentication_failed,
            L"host_protocol.open",
            L"Private message authentication failed");
    }
    if (next_receive_sequence_
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return protocol_error<HostMessage>(
            HostProtocolError::sequence_exhausted,
            L"host_protocol.open",
            L"Private receive sequence is exhausted");
    }
    ++next_receive_sequence_;
    return Result<HostMessage>::success(frame.message);
}

void SessionAuthenticator::clear() noexcept {
    secure_zero(service_to_worker_key_);
    secure_zero(worker_to_service_key_);
    valid_ = false;
    next_send_sequence_ = 0;
    next_receive_sequence_ = 0;
}

ServiceHandshakeAcceptance::ServiceHandshakeAcceptance(
    ServiceProof proof_value,
    SessionAuthenticator session_value) noexcept
    : proof(std::move(proof_value)), session(std::move(session_value)) {}

ServiceHandshake::ServiceHandshake(
    WorkerBinding expected,
    SessionSecret secret,
    HandshakeNonce service_nonce) noexcept
    : expected_(std::move(expected)),
      secret_(std::move(secret)),
      hello_{expected_, service_nonce} {}

Result<ServiceHandshake> ServiceHandshake::begin(
    WorkerBinding expected,
    SessionSecret secret,
    HandshakeNonce service_nonce) {
    if (!is_valid(expected) || all_zero(secret.bytes())
        || all_zero(service_nonce)) {
        return protocol_error<ServiceHandshake>(
            HostProtocolError::invalid_argument,
            L"host_protocol.service_begin",
            L"Service handshake binding or nonce is invalid");
    }
    return Result<ServiceHandshake>::success(ServiceHandshake(
        std::move(expected), std::move(secret), service_nonce));
}

const ClientHello& ServiceHandshake::hello() const noexcept { return hello_; }

Result<ServiceHandshakeAcceptance> ServiceHandshake::accept_challenge(
    const WorkerChallenge& challenge) {
    if (consumed_) {
        return protocol_error<ServiceHandshakeAcceptance>(
            HostProtocolError::handshake_replay,
            L"host_protocol.service_challenge",
            L"A worker challenge was already consumed");
    }
    consumed_ = true;
    if (challenge.binding != expected_
        || challenge.service_nonce != hello_.service_nonce
        || all_zero(challenge.worker_nonce)
        || challenge.worker_nonce == hello_.service_nonce) {
        return protocol_error<ServiceHandshakeAcceptance>(
            HostProtocolError::binding_mismatch,
            L"host_protocol.service_challenge",
            L"Worker challenge binding is invalid");
    }
    auto expected_proof = proof_tag(
        "MYRD-HOST-WORKER-PROOF-v1",
        secret_, expected_, hello_.service_nonce, challenge.worker_nonce);
    if (!expected_proof.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            expected_proof.error());
    }
    const bool matched = constant_time_equal(
        expected_proof.value(), challenge.proof);
    secure_zero(expected_proof.value());
    if (!matched) {
        return protocol_error<ServiceHandshakeAcceptance>(
            HostProtocolError::authentication_failed,
            L"host_protocol.service_challenge",
            L"Worker challenge authentication failed");
    }
    auto service_proof = proof_tag(
        "MYRD-HOST-SERVICE-PROOF-v1",
        secret_, expected_, hello_.service_nonce, challenge.worker_nonce);
    if (!service_proof.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            service_proof.error());
    }
    auto keys = derive_connection_keys(
        secret_, expected_, hello_.service_nonce, challenge.worker_nonce);
    if (!keys.ok()) {
        secure_zero(service_proof.value());
        return Result<ServiceHandshakeAcceptance>::failure(keys.error());
    }
    ServiceProof proof{
        expected_, hello_.service_nonce, challenge.worker_nonce,
        service_proof.move_value()};
    return Result<ServiceHandshakeAcceptance>::success(
        ServiceHandshakeAcceptance(
            std::move(proof),
            SessionAuthenticator(
                expected_,
                SessionRole::service,
                keys.value().service_to_worker,
                keys.value().worker_to_service)));
}

WorkerHandshake::WorkerHandshake(
    WorkerBinding expected,
    SessionSecret secret) noexcept
    : expected_(std::move(expected)), secret_(std::move(secret)) {}

Result<WorkerHandshake> WorkerHandshake::begin(
    WorkerBinding expected,
    SessionSecret secret) {
    if (!is_valid(expected) || all_zero(secret.bytes())) {
        return protocol_error<WorkerHandshake>(
            HostProtocolError::invalid_argument,
            L"host_protocol.worker_begin",
            L"Worker handshake binding is invalid");
    }
    return Result<WorkerHandshake>::success(WorkerHandshake(
        std::move(expected), std::move(secret)));
}

Result<WorkerChallenge> WorkerHandshake::accept_hello(
    const ClientHello& hello,
    HandshakeNonce worker_nonce) {
    if (challenge_issued_ || authenticated_) {
        return protocol_error<WorkerChallenge>(
            HostProtocolError::handshake_replay,
            L"host_protocol.worker_hello",
            L"A client hello was already consumed");
    }
    if (hello.binding != expected_
        || all_zero(hello.service_nonce)
        || all_zero(worker_nonce)
        || hello.service_nonce == worker_nonce) {
        return protocol_error<WorkerChallenge>(
            HostProtocolError::binding_mismatch,
            L"host_protocol.worker_hello",
            L"Client hello binding or nonce is invalid");
    }
    auto proof = proof_tag(
        "MYRD-HOST-WORKER-PROOF-v1",
        secret_, expected_, hello.service_nonce, worker_nonce);
    if (!proof.ok()) return Result<WorkerChallenge>::failure(proof.error());
    hello_.emplace(hello);
    challenge_.emplace(WorkerChallenge{
        expected_, hello.service_nonce, worker_nonce, proof.move_value()});
    challenge_issued_ = true;
    return Result<WorkerChallenge>::success(challenge_.value());
}

Result<SessionAuthenticator> WorkerHandshake::accept_proof(
    const ServiceProof& proof) {
    if (!challenge_issued_ || proof_consumed_) {
        return protocol_error<SessionAuthenticator>(
            HostProtocolError::handshake_replay,
            L"host_protocol.worker_proof",
            L"Service proof is missing or already consumed");
    }
    proof_consumed_ = true;
    if (proof.binding != expected_
        || proof.service_nonce != hello_->service_nonce
        || proof.worker_nonce != challenge_->worker_nonce) {
        return protocol_error<SessionAuthenticator>(
            HostProtocolError::binding_mismatch,
            L"host_protocol.worker_proof",
            L"Service proof binding is invalid");
    }
    auto expected_proof = proof_tag(
        "MYRD-HOST-SERVICE-PROOF-v1",
        secret_, expected_, proof.service_nonce, proof.worker_nonce);
    if (!expected_proof.ok()) {
        return Result<SessionAuthenticator>::failure(expected_proof.error());
    }
    const bool matched = constant_time_equal(
        expected_proof.value(), proof.proof);
    secure_zero(expected_proof.value());
    if (!matched) {
        return protocol_error<SessionAuthenticator>(
            HostProtocolError::authentication_failed,
            L"host_protocol.worker_proof",
            L"Service proof authentication failed");
    }
    auto keys = derive_connection_keys(
        secret_, expected_, proof.service_nonce, proof.worker_nonce);
    if (!keys.ok()) return Result<SessionAuthenticator>::failure(keys.error());
    authenticated_ = true;
    return Result<SessionAuthenticator>::success(
        SessionAuthenticator(
            expected_,
            SessionRole::worker,
            keys.value().service_to_worker,
            keys.value().worker_to_service));
}

} // namespace myrd::host
