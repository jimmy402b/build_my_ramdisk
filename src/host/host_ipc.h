#pragma once

#include "src/common/result.h"
#include "src/host/host_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::host {

// FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
// FILE_WRITE_ATTRIBUTES | SYNCHRONIZE. Generic rights and
// FILE_CREATE_PIPE_INSTANCE are intentionally absent.
inline constexpr std::uint32_t kPrivatePipeClientAccessMask = 0x00100183U;
inline constexpr std::uint32_t kPrivatePipeModeFlags = 0x00000008U;

class PrivatePipeSecurityDescriptor final {
public:
    PrivatePipeSecurityDescriptor(PrivatePipeSecurityDescriptor&&) noexcept;
    PrivatePipeSecurityDescriptor& operator=(
        PrivatePipeSecurityDescriptor&&) noexcept;
    ~PrivatePipeSecurityDescriptor() noexcept;

    PrivatePipeSecurityDescriptor(
        const PrivatePipeSecurityDescriptor&) = delete;
    PrivatePipeSecurityDescriptor& operator=(
        const PrivatePipeSecurityDescriptor&) = delete;

    void* native_descriptor() const noexcept;
    void* native_security_attributes() noexcept;

private:
    struct Impl;
    explicit PrivatePipeSecurityDescriptor(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend Result<PrivatePipeSecurityDescriptor>
        create_private_pipe_security_descriptor();
};

Result<PrivatePipeSecurityDescriptor>
    create_private_pipe_security_descriptor();

class PrivatePipeInstance final {
public:
    static Result<PrivatePipeInstance> create(
        const product::Uuid& profile_id);
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    static Result<PrivatePipeInstance> create_engineering_current_user(
        const product::Uuid& profile_id);
#endif

    ~PrivatePipeInstance() noexcept;
    PrivatePipeInstance(PrivatePipeInstance&& other) noexcept;
    PrivatePipeInstance& operator=(PrivatePipeInstance&& other) noexcept;
    PrivatePipeInstance(const PrivatePipeInstance&) = delete;
    PrivatePipeInstance& operator=(const PrivatePipeInstance&) = delete;

    void* native_handle() const noexcept;
    const std::wstring& name() const noexcept;
    void* release() noexcept;

private:
    PrivatePipeInstance(void* handle, std::wstring name) noexcept;
    void close() noexcept;

    void* handle_{};
    std::wstring name_;
};

// The service writes the complete fixed-size secret record before launching
// the worker. Only this read handle is inheritable; Task 23 places it in the
// explicit child-handle allowlist and uses it as the worker's startup input.
class StartupSecretTransfer final {
public:
    ~StartupSecretTransfer() noexcept;
    StartupSecretTransfer(StartupSecretTransfer&& other) noexcept;
    StartupSecretTransfer& operator=(StartupSecretTransfer&& other) noexcept;
    StartupSecretTransfer(const StartupSecretTransfer&) = delete;
    StartupSecretTransfer& operator=(const StartupSecretTransfer&) = delete;

    void* inherited_read_handle() const noexcept;
    void* release_read_handle() noexcept;

private:
    explicit StartupSecretTransfer(void* read_handle) noexcept;
    void close() noexcept;

    void* read_handle_{};

    friend Result<StartupSecretTransfer> prepare_startup_secret_transfer(
        const WorkerBinding&, const SessionSecret&);
};

Result<StartupSecretTransfer> prepare_startup_secret_transfer(
    const WorkerBinding& binding,
    const SessionSecret& secret);

// Real process launch cannot bind a startup record to the child PID and
// creation time until CreateProcessW has returned.  This staged channel owns
// both pipe ends until the suspended child exists.  Only the read end is
// inheritable; publish writes one complete identity-bound record through the
// non-inherited parent end and permanently closes that end.
class StagedStartupSecretTransfer final {
public:
    ~StagedStartupSecretTransfer() noexcept;
    StagedStartupSecretTransfer(
        StagedStartupSecretTransfer&& other) noexcept;
    StagedStartupSecretTransfer& operator=(
        StagedStartupSecretTransfer&& other) noexcept;
    StagedStartupSecretTransfer(
        const StagedStartupSecretTransfer&) = delete;
    StagedStartupSecretTransfer& operator=(
        const StagedStartupSecretTransfer&) = delete;

    void* inherited_read_handle() const noexcept;
    void* release_read_handle() noexcept;
    void close_parent_read() noexcept;
    Result<void> publish(
        const WorkerBinding& binding,
        const SessionSecret& secret);

private:
    StagedStartupSecretTransfer(
        void* read_handle, void* write_handle) noexcept;
    void close() noexcept;

    void* read_handle_{};
    void* write_handle_{};

    friend Result<StagedStartupSecretTransfer>
        prepare_staged_startup_secret_transfer();
};

Result<StagedStartupSecretTransfer>
    prepare_staged_startup_secret_transfer();

struct StartupSecretMaterial final {
    WorkerBinding binding;
    SessionSecret secret;

    StartupSecretMaterial(
        WorkerBinding binding_value,
        SessionSecret secret_value) noexcept;
    StartupSecretMaterial(StartupSecretMaterial&&) noexcept = default;
    StartupSecretMaterial& operator=(StartupSecretMaterial&&) noexcept =
        default;
    StartupSecretMaterial(const StartupSecretMaterial&) = delete;
    StartupSecretMaterial& operator=(const StartupSecretMaterial&) = delete;
};

struct StartupBindingConstraints final {
    product::Uuid profile_id;
    std::uint32_t current_process_id{};
    std::uint64_t current_process_creation_time{};
    std::uint32_t product_major{};
    std::uint32_t product_minor{};
    std::uint32_t product_patch{};
};

// Takes ownership of inherited_read_handle and fails closed unless the
// record matches the trusted profile argument, current live process identity,
// and this worker binary's product version.
Result<StartupSecretMaterial> consume_startup_material(
    void* inherited_read_handle,
    const StartupBindingConstraints& constraints);

// Takes ownership of inherited_read_handle and closes it on every outcome.
Result<SessionSecret> consume_startup_secret(
    void* inherited_read_handle,
    const WorkerBinding& expected_binding);

inline constexpr std::wstring_view kRuntimeLeaseFileName = L"runtime.lease";
inline constexpr std::size_t kMaximumRuntimeLeaseBytes = 128U * 1024U;

struct RuntimeLeaseMaterial final {
    WorkerBinding binding;
    SessionSecret secret;

    RuntimeLeaseMaterial(
        WorkerBinding binding_value,
        SessionSecret secret_value) noexcept;
    RuntimeLeaseMaterial(RuntimeLeaseMaterial&&) noexcept = default;
    RuntimeLeaseMaterial& operator=(RuntimeLeaseMaterial&&) noexcept = default;
    RuntimeLeaseMaterial(const RuntimeLeaseMaterial&) = delete;
    RuntimeLeaseMaterial& operator=(const RuntimeLeaseMaterial&) = delete;
};

Result<std::vector<std::byte>> encode_runtime_lease(
    const WorkerBinding& binding,
    const SessionSecret& secret);
Result<RuntimeLeaseMaterial> decode_runtime_lease(
    std::span<const std::byte> bytes,
    const WorkerBinding& expected_binding);

// A successful reattachment may rotate credentials only to the exact next
// generation. Profile revision may stay equal or advance, never roll back.
Result<WorkerBinding> next_reattachment_binding(
    const WorkerBinding& current,
    std::uint64_t next_profile_revision);

class RuntimeLeaseSecurityDescriptor final {
public:
    RuntimeLeaseSecurityDescriptor(
        RuntimeLeaseSecurityDescriptor&&) noexcept;
    RuntimeLeaseSecurityDescriptor& operator=(
        RuntimeLeaseSecurityDescriptor&&) noexcept;
    ~RuntimeLeaseSecurityDescriptor() noexcept;

    RuntimeLeaseSecurityDescriptor(
        const RuntimeLeaseSecurityDescriptor&) = delete;
    RuntimeLeaseSecurityDescriptor& operator=(
        const RuntimeLeaseSecurityDescriptor&) = delete;

    void* native_descriptor() const noexcept;
    void* native_security_attributes() noexcept;

private:
    struct Impl;
    explicit RuntimeLeaseSecurityDescriptor(
        std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;

    friend Result<RuntimeLeaseSecurityDescriptor>
        create_runtime_lease_security_descriptor();
};

Result<RuntimeLeaseSecurityDescriptor>
    create_runtime_lease_security_descriptor();

struct RuntimeLeaseTemp final {
    std::uint64_t value{};
    bool operator==(const RuntimeLeaseTemp&) const noexcept = default;
};

// A path-free capability specialized for one profile's runtime.lease. Its
// production implementation creates every temp with the exact SYSTEM-only
// descriptor before secret ciphertext is written.
class IRuntimeLeaseDirectory {
public:
    virtual ~IRuntimeLeaseDirectory() noexcept = default;
    virtual Result<RuntimeLeaseTemp> create_secure_temp() = 0;
    virtual Result<void> write_temp(
        RuntimeLeaseTemp temp,
        std::span<const std::byte> bytes) = 0;
    virtual Result<void> flush_temp(RuntimeLeaseTemp temp) = 0;
    virtual Result<std::vector<std::byte>> read_temp(
        RuntimeLeaseTemp temp) = 0;
    virtual Result<void> publish_temp(RuntimeLeaseTemp temp) = 0;
    virtual Result<void> flush_directory() = 0;
    virtual Result<std::vector<std::byte>> read_current() = 0;
    virtual void discard_temp(RuntimeLeaseTemp temp) noexcept = 0;
};

Result<void> publish_runtime_lease_atomically(
    IRuntimeLeaseDirectory& directory,
    const WorkerBinding& binding,
    const SessionSecret& secret);
Result<RuntimeLeaseMaterial> load_runtime_lease(
    IRuntimeLeaseDirectory& directory,
    const WorkerBinding& expected_binding);

// Reads and validates only the authenticated public binding. No protected
// secret bytes are returned. Callers must still pin and compare the live
// process identity before using load_runtime_lease with this binding.
Result<WorkerBinding> inspect_runtime_lease_binding(
    IRuntimeLeaseDirectory& directory);

inline constexpr std::uint32_t kReattachmentStageCommand = 0x52544101U;
inline constexpr std::uint32_t kReattachmentStageAck = 0x52544102U;
inline constexpr std::uint32_t kReattachmentCommitCommand = 0x52544103U;
inline constexpr std::uint32_t kReattachmentCommitAck = 0x52544104U;
inline constexpr std::uint32_t kReattachmentStateQuery = 0x52544105U;
inline constexpr std::uint32_t kReattachmentStateResponse = 0x52544106U;
inline constexpr std::uint32_t kReattachmentRecoveryFinalize = 0x52544107U;
inline constexpr std::uint32_t kReattachmentRecoveryFinalizeAck = 0x52544108U;
inline constexpr std::uint32_t kReattachmentRecoveryConfirm = 0x52544109U;
inline constexpr std::uint32_t kReattachmentRecoveryConfirmAck = 0x5254410aU;
inline constexpr std::uint32_t kWorkerRuntimeSnapshotQuery = 0x52545301U;
inline constexpr std::uint32_t kWorkerRuntimeSnapshotResponse = 0x52545302U;
inline constexpr std::uint32_t kWorkerCheckpointRequest = 0x43504b01U;
inline constexpr std::uint32_t kWorkerCheckpointResponse = 0x43504b02U;
inline constexpr std::uint32_t kWorkerControlledStopRequest = 0x53545001U;
inline constexpr std::uint32_t kWorkerControlledStopResponse = 0x53545002U;
inline constexpr std::uint32_t kPrivateWireClientHello = 0x50575001U;
inline constexpr std::uint32_t kPrivateWireWorkerChallenge = 0x50575002U;
inline constexpr std::uint32_t kPrivateWireServiceProof = 0x50575003U;
inline constexpr std::uint32_t kPrivateWireAuthenticated = 0x50575004U;
inline constexpr std::uint32_t kPrivateWireExchangeRequest = 0x50575005U;
inline constexpr std::uint32_t kPrivateWireExchangeResponse = 0x50575006U;
inline constexpr std::wstring_view kRuntimeIdentityFileName =
    L"runtime.identity";
inline constexpr std::wstring_view kRuntimeIdentityPreviousFileName =
    L"runtime.identity.prev";

struct WorkerRuntimeDeviceIdentity final {
    std::uint32_t physical_drive_number{};
    std::uint64_t length_bytes{};
    std::uint32_t port_number{};
    std::uint8_t path_id{};
    std::uint8_t target_id{};
    std::uint8_t lun{};
    product::Uuid gpt_disk_id;
    product::Uuid partition_id;
    product::Uuid volume_id;

    bool operator==(
        const WorkerRuntimeDeviceIdentity&) const noexcept = default;
};

struct WorkerRuntimeStatus final {
    WorkerBinding binding;
    WorkerRuntimeDeviceIdentity device;
    std::uint32_t aim_device_id{};
    std::uint64_t committed_generation{};
    std::wstring proxy_native_path;
    std::uint64_t heartbeat_sequence{};
    std::uint64_t snapshot_sequence{};
    std::uint64_t dirty_epoch{};
    bool dirty{};
    bool checkpoint_policy_enabled{};
    std::uint64_t checkpoint_policy_revision{};
    std::uint64_t checkpoint_deadline_unix_ms{};
    std::optional<product::Uuid> active_operation_id;
    std::uint32_t active_operation_phase{};
    bool active_operation_noncancellable{};

    bool operator==(const WorkerRuntimeStatus&) const noexcept = default;
};

Result<std::vector<std::byte>> encode_worker_runtime_status(
    const WorkerRuntimeStatus& status);
Result<WorkerRuntimeStatus> decode_worker_runtime_status(
    std::span<const std::byte> payload,
    const WorkerBinding& authenticated_binding);
// Dead-worker recovery may observe the narrow crash window after the durable
// lease has advanced from N to N+1 but before the service has replaced the
// protected runtime identity snapshot.  Accept only that exact predecessor;
// online/authenticated status decoding remains exact above.
Result<WorkerRuntimeStatus> decode_persisted_worker_runtime_status(
    std::span<const std::byte> payload,
    const WorkerBinding& current_lease_binding);

struct WorkerCheckpointReply final {
    bool confirmed{};
    std::uint64_t generation{};
    bool operator==(const WorkerCheckpointReply&) const noexcept = default;
};

struct WorkerControlledStopReply final {
    bool safely_stopped{};
    bool checkpoint_completed{};
    std::uint64_t generation{};
    bool operator==(const WorkerControlledStopReply&) const noexcept = default;
};

Result<std::vector<std::byte>> encode_worker_checkpoint_reply(
    const WorkerCheckpointReply& reply);
Result<WorkerCheckpointReply> decode_worker_checkpoint_reply(
    std::span<const std::byte> payload);
Result<std::vector<std::byte>> encode_worker_controlled_stop_reply(
    const WorkerControlledStopReply& reply);
Result<WorkerControlledStopReply> decode_worker_controlled_stop_reply(
    std::span<const std::byte> payload);

class IWorkerReattachmentTransport {
public:
    virtual ~IWorkerReattachmentTransport() noexcept = default;
    virtual Result<WorkerChallenge> begin_attachment(
        const HandshakeNonce& manager_instance_nonce,
        const ClientHello& hello) = 0;
    virtual Result<void> authenticate_attachment(
        const HandshakeNonce& manager_instance_nonce,
        const ServiceProof& proof) = 0;
    virtual Result<AuthenticatedHostFrame> exchange(
        const HandshakeNonce& manager_instance_nonce,
        const AuthenticatedHostFrame& request) = 0;
    virtual void disconnect_attachment(
        const HandshakeNonce& manager_instance_nonce) noexcept = 0;
};

using AuthenticatedWorkerRequestHandler = std::function<
    Result<HostMessage>(const HostMessage&)>;

// Worker-side protocol state. The manager slot is atomically reserved before
// handshake and becomes attached only after proof verification. A second
// nonce is denied until the owning transport reports closure. Rotation is
// idempotent for one exact N+1 successor and invalidates the N session before
// any application request can continue.
class WorkerReattachmentEndpoint final
    : public IWorkerReattachmentTransport {
public:
    static Result<std::shared_ptr<WorkerReattachmentEndpoint>> create(
        WorkerBinding current_binding,
        SessionSecret current_secret,
        AuthenticatedWorkerRequestHandler handler = {});

    ~WorkerReattachmentEndpoint() noexcept override;
    WorkerReattachmentEndpoint(WorkerReattachmentEndpoint&&) = delete;
    WorkerReattachmentEndpoint& operator=(
        WorkerReattachmentEndpoint&&) = delete;
    WorkerReattachmentEndpoint(const WorkerReattachmentEndpoint&) = delete;
    WorkerReattachmentEndpoint& operator=(
        const WorkerReattachmentEndpoint&) = delete;

    Result<WorkerChallenge> begin_attachment(
        const HandshakeNonce& manager_instance_nonce,
        const ClientHello& hello) override;
    Result<void> authenticate_attachment(
        const HandshakeNonce& manager_instance_nonce,
        const ServiceProof& proof) override;
    Result<AuthenticatedHostFrame> exchange(
        const HandshakeNonce& manager_instance_nonce,
        const AuthenticatedHostFrame& request) override;
    void disconnect_attachment(
        const HandshakeNonce& manager_instance_nonce) noexcept override;

    WorkerBinding current_binding() const;
    bool manager_attached() const noexcept;

private:
    struct Impl;
    explicit WorkerReattachmentEndpoint(
        std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

enum class ServiceReattachmentDisposition : std::uint8_t {
    rotated_successor = 0,
    recovered_current,
};

class ServiceReattachmentResult final {
public:
    ServiceReattachmentResult(
        WorkerBinding binding_value,
        HandshakeNonce manager_nonce_value,
        SessionAuthenticator session_value,
        ServiceReattachmentDisposition disposition,
        std::shared_ptr<IWorkerReattachmentTransport> transport) noexcept;
    ~ServiceReattachmentResult() noexcept;
    ServiceReattachmentResult(ServiceReattachmentResult&&) noexcept;
    ServiceReattachmentResult& operator=(
        ServiceReattachmentResult&&) noexcept;
    ServiceReattachmentResult(const ServiceReattachmentResult&) = delete;
    ServiceReattachmentResult& operator=(
        const ServiceReattachmentResult&) = delete;

    const WorkerBinding& binding() const noexcept;
    const HandshakeNonce& manager_instance_nonce() const noexcept;
    SessionAuthenticator& session() noexcept;
    ServiceReattachmentDisposition disposition() const noexcept;
    Result<HostMessage> exchange(const HostMessage& request);
    void disconnect() noexcept;

private:
    WorkerBinding binding_;
    HandshakeNonce manager_instance_nonce_{};
    SessionAuthenticator session_;
    ServiceReattachmentDisposition disposition_{};
    std::shared_ptr<IWorkerReattachmentTransport> transport_;
};

// Establish N, stage/publish/commit exactly N+1, invalidate N, and establish a
// fresh authenticated N+1 session. Commit-indeterminate lease publication is
// reconciled by reading the exact current lease; it never advances to N+2.
Result<ServiceReattachmentResult> reattach_and_rotate_runtime_lease(
    IRuntimeLeaseDirectory& directory,
    std::shared_ptr<IWorkerReattachmentTransport> transport,
    const WorkerBinding& current_binding,
    std::uint64_t next_profile_revision);

Result<std::unique_ptr<IRuntimeLeaseDirectory>>
    open_runtime_lease_directory(
        std::wstring_view trusted_program_data_root,
        const product::Uuid& profile_id);

} // namespace myrd::host
