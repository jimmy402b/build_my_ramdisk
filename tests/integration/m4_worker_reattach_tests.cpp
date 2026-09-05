#include "src/host/host_ipc.h"
#include "src/management/worker_supervisor.h"
#include "src/management/service_core.h"
#include "tests/support/m4_fake_clock.h"
#include "tests/support/test_framework.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace myrd;
namespace host = myrd::host;
namespace management = myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

host::WorkerBinding initial_binding() {
    return {
        uuid("11111111-1111-4111-8111-111111111111"),
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        0x01dc000012345678ULL,
        1,
        9,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

host::SecretBytes secret_bytes() {
    host::SecretBytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(0x40U + index);
    }
    return bytes;
}

host::SessionSecret secret() {
    auto value = host::SessionSecret::from_bytes(secret_bytes());
    REQUIRE(value.ok());
    return value.move_value();
}

Error injected_error(const wchar_t* operation) {
    return {
        ErrorDomain::win32,
        static_cast<std::uint32_t>(host::HostProtocolError::invalid_argument),
        operation,
        L"injected reattachment failure",
        29U};
}

enum class PublishFailure { none, before_replace, after_replace };

class LeaseDirectory final : public host::IRuntimeLeaseDirectory {
public:
    Result<host::RuntimeLeaseTemp> create_secure_temp() override {
        std::scoped_lock lock(mutex_);
        temp_.clear();
        temp_exists_ = true;
        return Result<host::RuntimeLeaseTemp>::success({++token_});
    }
    Result<void> write_temp(
        host::RuntimeLeaseTemp,
        std::span<const std::byte> bytes) override {
        std::scoped_lock lock(mutex_);
        if (!temp_exists_) {
            return Result<void>::failure(injected_error(L"lease.write"));
        }
        temp_.assign(bytes.begin(), bytes.end());
        return Result<void>::success();
    }
    Result<void> flush_temp(host::RuntimeLeaseTemp) override {
        return Result<void>::success();
    }
    Result<std::vector<std::byte>> read_temp(
        host::RuntimeLeaseTemp) override {
        std::scoped_lock lock(mutex_);
        return Result<std::vector<std::byte>>::success(temp_);
    }
    Result<void> publish_temp(host::RuntimeLeaseTemp) override {
        std::scoped_lock lock(mutex_);
        if (failure == PublishFailure::before_replace) {
            return Result<void>::failure(
                injected_error(L"lease.publish.before"));
        }
        current_ = replacement_on_publish.has_value()
            ? replacement_on_publish.value() : temp_;
        temp_.clear();
        temp_exists_ = false;
        if (failure == PublishFailure::after_replace) {
            return Result<void>::failure(
                injected_error(L"lease.publish.after"));
        }
        return Result<void>::success();
    }
    Result<void> flush_directory() override {
        return Result<void>::success();
    }
    Result<std::vector<std::byte>> read_current() override {
        std::scoped_lock lock(mutex_);
        if (!current_.has_value()) {
            return Result<std::vector<std::byte>>::failure(
                injected_error(L"lease.read_current"));
        }
        return Result<std::vector<std::byte>>::success(current_.value());
    }
    void discard_temp(host::RuntimeLeaseTemp) noexcept override {
        std::scoped_lock lock(mutex_);
        temp_.clear();
        temp_exists_ = false;
    }

    PublishFailure failure{PublishFailure::none};
    std::optional<std::vector<std::byte>> replacement_on_publish;

private:
    std::mutex mutex_;
    std::optional<std::vector<std::byte>> current_;
    std::vector<std::byte> temp_;
    std::uint64_t token_{};
    bool temp_exists_{};
};

host::WorkerRuntimeDeviceIdentity host_device() {
    return {
        9,
        8ULL << 30,
        3,
        0,
        2,
        0,
        uuid("33333333-3333-4333-8333-333333333333"),
        uuid("44444444-4444-4444-8444-444444444444"),
        uuid("55555555-5555-4555-8555-555555555555")};
}

class PersistentWorker final {
public:
    static std::unique_ptr<PersistentWorker> create(
        const host::WorkerBinding& binding) {
        auto worker = std::unique_ptr<PersistentWorker>(
            new PersistentWorker(binding));
        auto endpoint = host::WorkerReattachmentEndpoint::create(
            binding,
            secret(),
            [raw = worker.get()](const host::HostMessage& request) {
                return raw->handle(request);
            });
        REQUIRE(endpoint.ok());
        worker->endpoint_ = endpoint.move_value();
        return worker;
    }

    host::WorkerReattachmentEndpoint& endpoint() noexcept {
        return *endpoint_;
    }

    std::shared_ptr<host::IWorkerReattachmentTransport> transport() const {
        return endpoint_;
    }

    std::shared_ptr<host::WorkerReattachmentEndpoint> endpoint_owner() const {
        return endpoint_;
    }

    void adopt_binding(const host::WorkerBinding& binding) {
        std::scoped_lock lock(mutex_);
        runtime_.binding = binding;
        ++runtime_.heartbeat_sequence;
        ++runtime_.snapshot_sequence;
    }

    host::WorkerRuntimeStatus status() const {
        std::scoped_lock lock(mutex_);
        return runtime_;
    }

private:
    explicit PersistentWorker(const host::WorkerBinding& binding)
        : runtime_{
              binding,
              host_device(),
              0x00020100U,
              7,
              L"\\BaseNamedObjects\\Global\\myrd-m1-0123456789abcdef0123456789abcdef",
              10,
              20,
              91,
              true,
              true,
              6,
              99'000,
              uuid("66666666-6666-4666-8666-666666666666"),
              4,
              true} {}

    Result<host::HostMessage> handle(const host::HostMessage& request) {
        if (request.command_id == host::kWorkerCheckpointRequest
            && request.payload.empty() && request.flags == 30'000) {
            auto payload = host::encode_worker_checkpoint_reply({true, 7});
            if (!payload.ok()) {
                return Result<host::HostMessage>::failure(payload.error());
            }
            return Result<host::HostMessage>::success(host::HostMessage{
                request.request_id,
                request.operation_id,
                host::kWorkerCheckpointResponse,
                0,
                payload.move_value()});
        }
        if (request.command_id != host::kWorkerRuntimeSnapshotQuery
            || !request.payload.empty()) {
            return Result<host::HostMessage>::failure(
                injected_error(L"worker.status.query"));
        }
        const auto endpoint_binding = endpoint_->current_binding();
        std::scoped_lock lock(mutex_);
        if (endpoint_binding != runtime_.binding) {
            return Result<host::HostMessage>::failure(
                injected_error(L"worker.status.binding"));
        }
        auto payload = host::encode_worker_runtime_status(runtime_);
        if (!payload.ok()) {
            return Result<host::HostMessage>::failure(payload.error());
        }
        return Result<host::HostMessage>::success(host::HostMessage{
            request.request_id,
            request.operation_id,
            host::kWorkerRuntimeSnapshotResponse,
            0,
            payload.move_value()});
    }

    mutable std::mutex mutex_;
    host::WorkerRuntimeStatus runtime_;
    std::shared_ptr<host::WorkerReattachmentEndpoint> endpoint_;
};

management::WorkerRuntimeIdentity management_identity(
    const host::WorkerBinding& binding) {
    return {
        binding.profile_id,
        binding.image_id,
        binding.worker_pid,
        binding.process_creation_time,
        binding.session_generation,
        binding.profile_revision,
        binding.product_major,
        binding.product_minor,
        binding.product_patch};
}

management::WorkerRuntimeSnapshot management_snapshot(
    host::WorkerRuntimeStatus value) {
    management::WorkerDeviceIdentity device{
        value.device.physical_drive_number,
        value.device.length_bytes,
        value.device.port_number,
        value.device.path_id,
        value.device.target_id,
        value.device.lun,
        value.device.gpt_disk_id,
        value.device.partition_id,
        value.device.volume_id};
    return {
        management_identity(value.binding),
        std::move(device),
        value.heartbeat_sequence,
        value.snapshot_sequence,
        value.dirty_epoch,
        value.dirty,
        value.checkpoint_policy_enabled,
        value.checkpoint_policy_revision,
        value.checkpoint_deadline_unix_ms,
        std::move(value.active_operation_id),
        value.active_operation_phase,
        value.active_operation_noncancellable};
}

host::WorkerBinding host_binding(
    const management::WorkerRuntimeIdentity& value) {
    return {
        value.profile_id,
        value.image_id,
        value.worker_pid,
        value.process_creation_time,
        value.session_generation,
        value.profile_revision,
        value.product_major,
        value.product_minor,
        value.product_patch};
}

struct ProcessState final {
    std::uint32_t pid{4242};
    std::uint64_t creation_time{0x01dc000012345678ULL};
    bool running{true};
    std::atomic<std::size_t> pins{};
};

class ProcessPin final : public management::IWorkerProcessIdentityPin {
public:
    explicit ProcessPin(ProcessState& state) noexcept : state_(&state) {
        ++state_->pins;
    }
    ~ProcessPin() noexcept override { --state_->pins; }
    std::uint32_t process_id() const noexcept override { return state_->pid; }
    std::uint64_t process_creation_time() const noexcept override {
        return state_->creation_time;
    }
    bool is_running() const noexcept override { return state_->running; }

private:
    ProcessState* state_{};
};

class ProcessQuery final : public management::IWorkerProcessIdentityQuery {
public:
    explicit ProcessQuery(ProcessState& state) noexcept : state_(&state) {}
    Result<std::unique_ptr<management::IWorkerProcessIdentityPin>> pin_process(
        std::uint32_t pid) override {
        if (pid != state_->pid) {
            return Result<std::unique_ptr<
                management::IWorkerProcessIdentityPin>>::failure(
                    injected_error(L"process.pin"));
        }
        std::unique_ptr<management::IWorkerProcessIdentityPin> pin =
            std::make_unique<ProcessPin>(*state_);
        return Result<std::unique_ptr<
            management::IWorkerProcessIdentityPin>>::success(
                std::move(pin));
    }

private:
    ProcessState* state_{};
};

class ProtocolConnection final : public management::IWorkerConnection {
public:
    explicit ProtocolConnection(
        host::ServiceReattachmentResult result) noexcept
        : result_(std::move(result)) {}

private:
    host::ServiceReattachmentResult result_;
};

class ProtocolConnector final
    : public management::IAuthenticatedWorkerConnector {
public:
    ProtocolConnector(PersistentWorker& worker, LeaseDirectory& directory)
        noexcept : worker_(&worker), directory_(&directory) {}

    Result<management::WorkerConnectionOpen> reattach_and_rotate(
        const management::WorkerRuntimeIdentity& expected,
        const std::uint64_t next_profile_revision,
        const product::Uuid&) override {
        auto reattached = host::reattach_and_rotate_runtime_lease(
            *directory_,
            worker_->transport(),
            host_binding(expected),
            next_profile_revision);
        if (!reattached.ok()) {
            return Result<management::WorkerConnectionOpen>::failure(
                reattached.error());
        }
        worker_->adopt_binding(reattached.value().binding());
        auto query = reattached.value().session().seal(host::HostMessage{
            reattached.value().binding().profile_id,
            reattached.value().binding().image_id,
            host::kWorkerRuntimeSnapshotQuery,
            0,
            {}});
        if (!query.ok()) {
            return Result<management::WorkerConnectionOpen>::failure(
                query.error());
        }
        auto response = worker_->endpoint().exchange(
            reattached.value().manager_instance_nonce(), query.value());
        if (!response.ok()) {
            return Result<management::WorkerConnectionOpen>::failure(
                response.error());
        }
        if (tamper_next_snapshot) {
            response.value().authentication_tag[0] ^= std::byte{0x01};
            tamper_next_snapshot = false;
        }
        auto opened = reattached.value().session().open(response.value());
        if (!opened.ok()) {
            return Result<management::WorkerConnectionOpen>::failure(
                opened.error());
        }
        if (opened.value().command_id
                != host::kWorkerRuntimeSnapshotResponse
            || opened.value().request_id
                != reattached.value().binding().profile_id
            || opened.value().operation_id
                != reattached.value().binding().image_id) {
            return Result<management::WorkerConnectionOpen>::failure(
                injected_error(L"connector.status.correlation"));
        }
        auto status = host::decode_worker_runtime_status(
            opened.value().payload, reattached.value().binding());
        if (!status.ok()) {
            return Result<management::WorkerConnectionOpen>::failure(
                status.error());
        }
        auto snapshot = management_snapshot(status.move_value());
        const auto disposition = reattached.value().disposition()
                == host::ServiceReattachmentDisposition::recovered_current
            ? management::WorkerReattachmentDisposition::recovered_current
            : management::WorkerReattachmentDisposition::rotated_successor;
        std::unique_ptr<management::IWorkerConnection> connection =
            std::make_unique<ProtocolConnection>(reattached.move_value());
        return Result<management::WorkerConnectionOpen>::success(
            management::WorkerConnectionOpen(
                std::move(connection),
                std::move(snapshot),
                disposition));
    }

    bool tamper_next_snapshot{};

private:
    PersistentWorker* worker_{};
    LeaseDirectory* directory_{};
};

class Scheduler final : public management::IReattachmentCheckpointScheduler {
public:
    Disposition enqueue_immediate_checkpoint_intent(
        const Intent& intent) noexcept override {
        for (const auto& existing : intents) {
            if (existing.equivalent_checkpoint_truth(intent)) {
                return Disposition::already_pending;
            }
        }
        intents.push_back(intent);
        return Disposition::queued;
    }
    std::vector<Intent> intents;
};

void publish_initial_lease(
    LeaseDirectory& directory,
    const host::WorkerBinding& binding) {
    auto value = secret();
    auto published = host::publish_runtime_lease_atomically(
        directory, binding, value);
    REQUIRE(published.ok());
}

class CommitAckLossTransport final
    : public host::IWorkerReattachmentTransport {
public:
    explicit CommitAckLossTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner) noexcept
        : inner_(std::move(inner)) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        auto response = inner_->exchange(nonce, request);
        ++exchange_count_;
        if (exchange_count_ == 3) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.commit_ack_lost"));
        }
        return response;
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    std::size_t exchange_count_{};
};

class OldSessionProbeTransport final
    : public host::IWorkerReattachmentTransport {
public:
    explicit OldSessionProbeTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner) noexcept
        : inner_(std::move(inner)) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        auto response = inner_->exchange(nonce, request);
        ++exchange_count_;
        if (exchange_count_ == 3 && response.ok()) {
            old_session_rejected = !inner_->exchange(nonce, request).ok();
        }
        return response;
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

    bool old_session_rejected{};

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    std::size_t exchange_count_{};
};

class CommitDeliveryLossTransport final
    : public host::IWorkerReattachmentTransport {
public:
    explicit CommitDeliveryLossTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner) noexcept
        : inner_(std::move(inner)) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        ++exchange_count_;
        if (exchange_count_ == 3) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.commit_not_delivered"));
        }
        return inner_->exchange(nonce, request);
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    std::size_t exchange_count_{};
};

class FinalStateResponseLossTransport final
    : public host::IWorkerReattachmentTransport {
public:
    explicit FinalStateResponseLossTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner) noexcept
        : inner_(std::move(inner)) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        auto response = inner_->exchange(nonce, request);
        ++exchange_count_;
        if (exchange_count_ == 4) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.final_state_response_lost"));
        }
        return response;
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    std::size_t exchange_count_{};
};

class FinalizeAckLossTransport final
    : public host::IWorkerReattachmentTransport {
public:
    explicit FinalizeAckLossTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner) noexcept
        : inner_(std::move(inner)) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        auto response = inner_->exchange(nonce, request);
        ++exchange_count_;
        if (exchange_count_ == 5) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.finalize_ack_lost"));
        }
        return response;
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    std::size_t exchange_count_{};
};

class ConfirmLossTransport final
    : public host::IWorkerReattachmentTransport {
public:
    enum class Point : std::uint8_t {
        before_delivery = 0,
        after_delivery,
    };

    ConfirmLossTransport(
        std::shared_ptr<host::WorkerReattachmentEndpoint> inner,
        const Point point) noexcept
        : inner_(std::move(inner)), point_(point) {}

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& nonce,
        const host::ClientHello& hello) override {
        return inner_->begin_attachment(nonce, hello);
    }
    Result<void> authenticate_attachment(
        const host::HandshakeNonce& nonce,
        const host::ServiceProof& proof) override {
        return inner_->authenticate_attachment(nonce, proof);
    }
    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& nonce,
        const host::AuthenticatedHostFrame& request) override {
        ++exchange_count_;
        if (exchange_count_ == 6 && point_ == Point::before_delivery) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.confirm_not_delivered"));
        }
        auto response = inner_->exchange(nonce, request);
        if (exchange_count_ == 6 && point_ == Point::after_delivery) {
            return Result<host::AuthenticatedHostFrame>::failure(
                injected_error(L"transport.confirm_ack_lost"));
        }
        return response;
    }
    void disconnect_attachment(
        const host::HandshakeNonce& nonce) noexcept override {
        inner_->disconnect_attachment(nonce);
    }

private:
    std::shared_ptr<host::WorkerReattachmentEndpoint> inner_;
    Point point_{};
    std::size_t exchange_count_{};
};

TEST_CASE(worker_runtime_status_codec_is_exact_and_binding_authenticated) {
    auto worker = PersistentWorker::create(initial_binding());
    auto status = worker->status();
    auto encoded = host::encode_worker_runtime_status(status);
    REQUIRE(encoded.ok());
    auto decoded = host::decode_worker_runtime_status(
        encoded.value(), status.binding);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), status);

    auto missing_device = status;
    missing_device.aim_device_id = 0x00ffffffU;
    REQUIRE(!host::encode_worker_runtime_status(missing_device).ok());
    auto missing_generation = status;
    missing_generation.committed_generation = 0;
    REQUIRE(!host::encode_worker_runtime_status(missing_generation).ok());
    auto foreign_proxy = status;
    foreign_proxy.proxy_native_path = L"\\BaseNamedObjects\\Foreign";
    REQUIRE(!host::encode_worker_runtime_status(foreign_proxy).ok());

    auto wrong = status.binding;
    ++wrong.session_generation;
    auto rejected = host::decode_worker_runtime_status(encoded.value(), wrong);
    REQUIRE(!rejected.ok());
    encoded.value().push_back(std::byte{});
    REQUIRE(!host::decode_worker_runtime_status(
        encoded.value(), status.binding).ok());
}

TEST_CASE(
    persisted_runtime_status_recovery_accepts_only_the_immediate_lease_predecessor) {
    auto worker = PersistentWorker::create(initial_binding());
    const auto status = worker->status();
    auto encoded = host::encode_worker_runtime_status(status);
    REQUIRE(encoded.ok());

    auto successor = host::next_reattachment_binding(
        status.binding, status.binding.profile_revision);
    REQUIRE(successor.ok());
    auto recovered = host::decode_persisted_worker_runtime_status(
        encoded.value(), successor.value());
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value(), status);

    auto skipped = successor.value();
    ++skipped.session_generation;
    REQUIRE(!host::decode_persisted_worker_runtime_status(
        encoded.value(), skipped).ok());

    auto changed_process = successor.value();
    ++changed_process.worker_pid;
    REQUIRE(!host::decode_persisted_worker_runtime_status(
        encoded.value(), changed_process).ok());

    auto changed_revision = successor.value();
    ++changed_revision.profile_revision;
    REQUIRE(!host::decode_persisted_worker_runtime_status(
        encoded.value(), changed_revision).ok());
}

TEST_CASE(worker_attachment_slot_allows_only_one_concurrent_manager) {
    const auto binding = initial_binding();
    auto worker = PersistentWorker::create(binding);
    auto manager_one = host::generate_handshake_nonce();
    auto manager_two = host::generate_handshake_nonce();
    auto service_one = host::generate_handshake_nonce();
    auto service_two = host::generate_handshake_nonce();
    REQUIRE(manager_one.ok() && manager_two.ok());
    REQUIRE(service_one.ok() && service_two.ok());
    const host::ClientHello hello_one{binding, service_one.value()};
    const host::ClientHello hello_two{binding, service_two.value()};

    auto first = std::async(std::launch::async, [&] {
        return worker->endpoint().begin_attachment(
            manager_one.value(), hello_one);
    });
    auto second = std::async(std::launch::async, [&] {
        return worker->endpoint().begin_attachment(
            manager_two.value(), hello_two);
    });
    auto first_result = first.get();
    auto second_result = second.get();
    REQUIRE(first_result.ok() != second_result.ok());
    worker->endpoint().disconnect_attachment(
        first_result.ok() ? manager_one.value() : manager_two.value());
    REQUIRE(!worker->endpoint().manager_attached());
}

TEST_CASE(worker_reattachment_reconciles_visible_publish_after_error) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    directory.failure = PublishFailure::after_replace;

    auto result = host::reattach_and_rotate_runtime_lease(
        directory, worker->transport(), binding, binding.profile_revision);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().binding().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), result.value().binding());
}

TEST_CASE(worker_reattachment_recovers_stage_when_lease_remained_old) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    directory.failure = PublishFailure::before_replace;

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, worker->transport(), binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto still_old = host::inspect_runtime_lease_binding(directory);
    REQUIRE(still_old.ok());
    REQUIRE_EQ(still_old.value(), binding);
    directory.failure = PublishFailure::none;

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory, worker->transport(), binding, binding.profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
}

TEST_CASE(worker_reattachment_recovers_commit_ack_loss_from_new_lease) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<CommitAckLossTransport>(
        worker->endpoint_owner());

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
}

TEST_CASE(worker_reattachment_recovers_when_commit_was_not_delivered) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<CommitDeliveryLossTransport>(
        worker->endpoint_owner());

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), binding);

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());
}

TEST_CASE(worker_reattachment_rejects_visible_lease_secret_conflict) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto next = host::next_reattachment_binding(
        binding, binding.profile_revision);
    REQUIRE(next.ok());
    auto wrong_bytes = secret_bytes();
    wrong_bytes[0] ^= std::byte{0x7f};
    auto wrong_secret = host::SessionSecret::from_bytes(wrong_bytes);
    REQUIRE(wrong_secret.ok());
    auto wrong_lease = host::encode_runtime_lease(
        next.value(), wrong_secret.value());
    REQUIRE(wrong_lease.ok());
    directory.replacement_on_publish = wrong_lease.move_value();
    directory.failure = PublishFailure::after_replace;

    auto conflicted = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        binding,
        binding.profile_revision);
    REQUIRE(!conflicted.ok());
    REQUIRE_EQ(worker->endpoint().current_binding(), binding);
    REQUIRE(!worker->endpoint().manager_attached());
}

TEST_CASE(worker_reattachment_final_state_loss_still_recovers_n_plus_one) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<FinalStateResponseLossTransport>(
        worker->endpoint_owner());

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
}

TEST_CASE(worker_reattachment_finalize_ack_loss_still_recovers_n_plus_one) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<FinalizeAckLossTransport>(
        worker->endpoint_owner());

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        recovered.value().disposition(),
        host::ServiceReattachmentDisposition::recovered_current);
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());
}

TEST_CASE(worker_reattachment_confirm_delivery_loss_recovers_n_plus_one) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<ConfirmLossTransport>(
        worker->endpoint_owner(),
        ConfirmLossTransport::Point::before_delivery);

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);

    auto recovered = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        recovered.value().disposition(),
        host::ServiceReattachmentDisposition::recovered_current);
    REQUIRE_EQ(recovered.value().binding().session_generation, 2U);
}

TEST_CASE(worker_reattachment_confirm_ack_loss_allows_only_exact_successor) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<ConfirmLossTransport>(
        worker->endpoint_owner(),
        ConfirmLossTransport::Point::after_delivery);

    auto failed = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!failed.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());

    // recovery=false is authenticated evidence that the worker received the
    // confirm, which itself proves the prior service validated finalize ACK.
    // A later service may therefore perform exactly one ordinary N+1 rotation.
    auto successor = host::reattach_and_rotate_runtime_lease(
        directory,
        worker->transport(),
        visible.value(),
        visible.value().profile_revision);
    REQUIRE(successor.ok());
    REQUIRE_EQ(
        successor.value().disposition(),
        host::ServiceReattachmentDisposition::rotated_successor);
    REQUIRE_EQ(successor.value().binding().session_generation, 3U);
    REQUIRE_EQ(worker->endpoint().current_binding(), successor.value().binding());
}

TEST_CASE(worker_supervisor_accepts_full_current_generation_crash_recovery) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto lossy = std::make_shared<CommitDeliveryLossTransport>(
        worker->endpoint_owner());
    auto interrupted = host::reattach_and_rotate_runtime_lease(
        directory, lossy, binding, binding.profile_revision);
    REQUIRE(!interrupted.ok());
    auto visible = host::inspect_runtime_lease_binding(directory);
    REQUIRE(visible.ok());
    REQUIRE_EQ(visible.value().session_generation, 2U);

    ProtocolConnector connector(*worker, directory);
    ProcessState process;
    ProcessQuery processes(process);
    Scheduler scheduler;
    tests::FakeServiceClock clock(100'000, 1'000);
    management::WorkerSupervisor supervisor(
        clock, processes, connector, scheduler);
    auto recovered = supervisor.reattach(
        management_identity(visible.value()),
        visible.value().profile_revision,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        recovered.value().identity.session_generation,
        visible.value().session_generation);
    REQUIRE_EQ(worker->endpoint().current_binding(), visible.value());
}

TEST_CASE(worker_reattachment_invalidates_n_session_before_n_plus_one_use) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto probing = std::make_shared<OldSessionProbeTransport>(
        worker->endpoint_owner());

    auto reattached = host::reattach_and_rotate_runtime_lease(
        directory,
        probing,
        binding,
        binding.profile_revision);
    REQUIRE(reattached.ok());
    REQUIRE(probing->old_session_rejected);
    REQUIRE_EQ(reattached.value().binding().session_generation, 2U);
}

TEST_CASE(service_reattachment_connection_exchanges_authenticated_requests) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    auto reattached = host::reattach_and_rotate_runtime_lease(
        directory, worker->transport(), binding, binding.profile_revision);
    REQUIRE(reattached.ok());
    worker->adopt_binding(reattached.value().binding());

    auto response = reattached.value().exchange(host::HostMessage{
        reattached.value().binding().profile_id,
        reattached.value().binding().image_id,
        host::kWorkerRuntimeSnapshotQuery,
        0,
        {}});
    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().command_id,
        host::kWorkerRuntimeSnapshotResponse);
    auto status = host::decode_worker_runtime_status(
        response.value().payload, reattached.value().binding());
    REQUIRE(status.ok());
    REQUIRE_EQ(status.value().binding, reattached.value().binding());

    const auto operation =
        uuid("77777777-7777-4777-8777-777777777777");
    response = reattached.value().exchange(host::HostMessage{
        reattached.value().binding().profile_id,
        operation,
        host::kWorkerCheckpointRequest,
        30'000,
        {}});
    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().operation_id, operation);
    REQUIRE_EQ(
        response.value().command_id,
        host::kWorkerCheckpointResponse);

    reattached.value().disconnect();
    REQUIRE(!worker->endpoint().manager_attached());
    REQUIRE(!reattached.value().exchange(host::HostMessage{
        reattached.value().binding().profile_id,
        operation,
        host::kWorkerRuntimeSnapshotQuery,
        0,
        {}}).ok());
}

TEST_CASE(worker_supervisor_rejects_tampered_authenticated_runtime_snapshot) {
    LeaseDirectory directory;
    const auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    ProtocolConnector connector(*worker, directory);
    connector.tamper_next_snapshot = true;
    ProcessState process;
    ProcessQuery processes(process);
    Scheduler scheduler;
    tests::FakeServiceClock clock(100'000, 1'000);
    management::WorkerSupervisor supervisor(
        clock, processes, connector, scheduler);

    auto attached = supervisor.reattach(
        management_identity(binding),
        binding.profile_revision,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(!attached.ok());
    REQUIRE_EQ(process.pins.load(), 0U);
    REQUIRE(!worker->endpoint().manager_attached());
}

TEST_CASE(worker_supervisor_protocol_reconnects_one_persistent_worker_100_times) {
    LeaseDirectory directory;
    auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    ProtocolConnector connector(*worker, directory);
    ProcessState process;
    ProcessQuery processes(process);
    Scheduler scheduler;
    tests::FakeServiceClock clock(100'000, 1'000);
    const auto stable_device = management_snapshot(worker->status()).device;
    const auto stable_operation = worker->status().active_operation_id;

    for (std::size_t cycle = 0; cycle < 100; ++cycle) {
        management::WorkerRuntimeSnapshot attached =
            management_snapshot(worker->status());
        {
            management::WorkerSupervisor supervisor(
                clock, processes, connector, scheduler);
            auto result = supervisor.reattach(
                management_identity(binding),
                binding.profile_revision,
                uuid("77777777-7777-4777-8777-777777777777"));
            REQUIRE(result.ok());
            attached = result.move_value();
            REQUIRE_EQ(attached.device, stable_device);
            REQUIRE_EQ(attached.dirty_epoch, 91U);
            REQUIRE(attached.dirty);
            REQUIRE_EQ(attached.active_operation_id, stable_operation);
            REQUIRE(worker->endpoint().manager_attached());
            REQUIRE_EQ(process.pins.load(), 1U);
        }
        REQUIRE(!worker->endpoint().manager_attached());
        REQUIRE_EQ(process.pins.load(), 0U);
        binding = host_binding(attached.identity);
        auto lease_binding = host::inspect_runtime_lease_binding(directory);
        REQUIRE(lease_binding.ok());
        REQUIRE_EQ(lease_binding.value(), binding);
    }
    REQUIRE_EQ(binding.session_generation, 101U);
    REQUIRE_EQ(scheduler.intents.size(), 1U);
}

TEST_CASE(worker_supervisor_second_service_is_denied_until_first_disconnects) {
    LeaseDirectory directory;
    auto binding = initial_binding();
    publish_initial_lease(directory, binding);
    auto worker = PersistentWorker::create(binding);
    ProtocolConnector connector(*worker, directory);
    ProcessState process;
    ProcessQuery processes(process);
    Scheduler scheduler;
    tests::FakeServiceClock clock(100'000, 1'000);

    management::WorkerRuntimeSnapshot first_snapshot =
        management_snapshot(worker->status());
    {
        management::WorkerSupervisor first(
            clock, processes, connector, scheduler);
        auto attached = first.reattach(
            management_identity(binding), binding.profile_revision,
            uuid("77777777-7777-4777-8777-777777777777"));
        REQUIRE(attached.ok());
        first_snapshot = attached.move_value();
        const auto current = host_binding(first_snapshot.identity);

        management::WorkerSupervisor second(
            clock, processes, connector, scheduler);
        auto duplicate = second.reattach(
            management_identity(current), current.profile_revision,
            uuid("88888888-8888-4888-8888-888888888888"));
        REQUIRE(!duplicate.ok());
        REQUIRE_EQ(process.pins.load(), 1U);
    }
    REQUIRE_EQ(process.pins.load(), 0U);
    REQUIRE(!worker->endpoint().manager_attached());

    const auto current = host_binding(first_snapshot.identity);
    management::WorkerSupervisor replacement(
        clock, processes, connector, scheduler);
    auto reattached = replacement.reattach(
        management_identity(current), current.profile_revision,
        uuid("88888888-8888-4888-8888-888888888888"));
    REQUIRE(reattached.ok());
}

} // namespace

namespace {

TEST_CASE(staged_startup_secret_binds_identity_after_child_creation) {
    auto transfer = host::prepare_staged_startup_secret_transfer();
    REQUIRE(transfer.ok());
    REQUIRE(transfer.value().inherited_read_handle() != nullptr);

    const auto expected_binding = initial_binding();
    auto expected_secret = secret();
    const auto expected_bytes = secret_bytes();
    REQUIRE(transfer.value().publish(
        expected_binding, expected_secret).ok());

    const host::StartupBindingConstraints constraints{
        expected_binding.profile_id,
        expected_binding.worker_pid,
        expected_binding.process_creation_time,
        expected_binding.product_major,
        expected_binding.product_minor,
        expected_binding.product_patch};
    auto material = host::consume_startup_material(
        transfer.value().release_read_handle(), constraints);
    REQUIRE(material.ok());
    REQUIRE(material.value().binding == expected_binding);
    REQUIRE(std::equal(
        material.value().secret.bytes().begin(),
        material.value().secret.bytes().end(),
        expected_bytes.begin(), expected_bytes.end()));
}

TEST_CASE(staged_startup_secret_is_single_publication) {
    auto transfer = host::prepare_staged_startup_secret_transfer();
    REQUIRE(transfer.ok());

    auto first_secret = secret();
    REQUIRE(transfer.value().publish(
        initial_binding(), first_secret).ok());
    auto second_secret = secret();
    const auto second = transfer.value().publish(
        initial_binding(), second_secret);
    REQUIRE(!second.ok());
}

TEST_CASE(staged_startup_secret_rejects_live_process_mismatch) {
    auto transfer = host::prepare_staged_startup_secret_transfer();
    REQUIRE(transfer.ok());
    const auto expected_binding = initial_binding();
    auto expected_secret = secret();
    REQUIRE(transfer.value().publish(
        expected_binding, expected_secret).ok());

    const host::StartupBindingConstraints constraints{
        expected_binding.profile_id,
        expected_binding.worker_pid + 1,
        expected_binding.process_creation_time,
        expected_binding.product_major,
        expected_binding.product_minor,
        expected_binding.product_patch};
    const auto material = host::consume_startup_material(
        transfer.value().release_read_handle(), constraints);
    REQUIRE(!material.ok());
}

TEST_CASE(worker_control_replies_round_trip_without_path_or_secret_fields) {
    const host::WorkerCheckpointReply checkpoint{true, 17};
    const auto checkpoint_bytes =
        host::encode_worker_checkpoint_reply(checkpoint);
    REQUIRE(checkpoint_bytes.ok());
    const auto decoded_checkpoint = host::decode_worker_checkpoint_reply(
        checkpoint_bytes.value());
    REQUIRE(decoded_checkpoint.ok());
    REQUIRE_EQ(decoded_checkpoint.value(), checkpoint);

    const host::WorkerControlledStopReply stopped{true, true, 18};
    const auto stopped_bytes =
        host::encode_worker_controlled_stop_reply(stopped);
    REQUIRE(stopped_bytes.ok());
    const auto decoded_stopped = host::decode_worker_controlled_stop_reply(
        stopped_bytes.value());
    REQUIRE(decoded_stopped.ok());
    REQUIRE_EQ(decoded_stopped.value(), stopped);

    const host::WorkerControlledStopReply checkpoint_only{false, true, 19};
    const auto checkpoint_only_bytes =
        host::encode_worker_controlled_stop_reply(checkpoint_only);
    REQUIRE(checkpoint_only_bytes.ok());
    const auto decoded_checkpoint_only =
        host::decode_worker_controlled_stop_reply(
            checkpoint_only_bytes.value());
    REQUIRE(decoded_checkpoint_only.ok());
    REQUIRE_EQ(decoded_checkpoint_only.value(), checkpoint_only);

    REQUIRE(!host::encode_worker_controlled_stop_reply(
        {true, false, 20}).ok());
}

TEST_CASE(worker_control_replies_reject_unknown_flags_and_trailing_bytes) {
    auto checkpoint = host::encode_worker_checkpoint_reply({true, 17});
    REQUIRE(checkpoint.ok());
    checkpoint.value()[0] = std::byte{0x80};
    REQUIRE(!host::decode_worker_checkpoint_reply(
        checkpoint.value()).ok());

    auto stopped = host::encode_worker_controlled_stop_reply(
        {true, true, 18});
    REQUIRE(stopped.ok());
    stopped.value().push_back(std::byte{});
    REQUIRE(!host::decode_worker_controlled_stop_reply(stopped.value()).ok());
}

} // namespace
