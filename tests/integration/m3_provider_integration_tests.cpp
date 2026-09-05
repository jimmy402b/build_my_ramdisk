#include "test_framework.h"

#include "src/m3/m3_provider.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/vhdx_reader.h"
#include "src/common/unique_handle.h"
#include "src/common/unique_view.h"
#include "src/proxy/imdproxy_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::m3;
using namespace myrd::persistence;

class TempBundle final {
public:
    TempBundle() {
        root = std::filesystem::temp_directory_path() /
            (L"myrd-m3-provider-" + std::to_wstring(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(10)};
}

Result<M3CompactionBoundaryDecision> accept_compaction_cut(
    const M3CompactionBoundaryEvidence& evidence) {
    REQUIRE(evidence.cut_dirty_epoch_known);
    return Result<M3CompactionBoundaryDecision>::success(
        {true, 303, 404, evidence});
}

PersistenceLimits limits() {
    constexpr std::uint64_t kMiB = 1024 * 1024;
    return {64 * kMiB, 64 * kMiB, 8192,
            64 * kIndexNodeBytes, 64,
            64 * kVhdxBatPageBytes, 64,
            2 * kMiB, 2 * kMiB};
}

DynamicBlockStoreOptions store_options() {
    return {2 * kM2BlockSize, 4 * kM2BlockSize, kM2BlockSize,
            2 * kM2BlockSize, 16, 64,
            4 * kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250}};
}

void create_bundle(const TempBundle& bundle) {
    GenerationZeroCreateRequest request{};
    request.bundle_root = bundle.root;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {8 * 1024 * 1024, 512, 4096};
    request.image_uuid.bytes[0] = std::byte{0x11};
    request.base_identity.file_identity.file_write_guid[0] = std::byte{0x21};
    request.base_identity.file_identity.data_write_guid[0] = std::byte{0x22};
    request.base_identity.virtual_disk_id[0] = std::byte{0x23};
    request.journal_identity.journal_uuid[0] = std::byte{0x31};
    request.journal_epoch = 1;
    request.operation_id = 3901;
    request.created_time_100ns = 1;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery = {limits()};
    auto created = create_generation_zero_image(OperationContext{}, request);
    REQUIRE(created.ok());
}

struct TransportProbe final {
    IBlockDevice* device{};
    std::vector<std::string> events;
    bool fail_stop{};
    bool throw_stop{};
};

class CapturingTransport final : public IM3ProviderTransport {
public:
    CapturingTransport(IBlockDevice& device,
                       std::shared_ptr<TransportProbe> probe)
        : probe_(std::move(probe)) {
        probe_->device = &device;
        probe_->events.emplace_back("create");
    }
    Result<void> start() override {
        probe_->events.emplace_back("start");
        return Result<void>::success();
    }
    Result<void> stop() override {
        probe_->events.emplace_back("stop");
        if (probe_->throw_stop) throw std::runtime_error("stop failure");
        if (probe_->fail_stop) {
            return Result<void>::failure({
                ErrorDomain::protocol, 0x3901,
                L"CapturingTransport::stop", L"injected stop failure"});
        }
        return Result<void>::success();
    }
    proxy::SessionSnapshot snapshot() const override { return {}; }
    proxy::ProxyObjectNames names() const override { return {}; }

private:
    std::shared_ptr<TransportProbe> probe_;
};

M3ProviderFactories factories(const std::shared_ptr<TransportProbe>& probe) {
    M3ProviderFactories result{};
    result.transport = [probe](IBlockDevice& device,
                               const proxy::IProxyNamespacePolicy&,
                               proxy::ShmProxySessionOptions) {
        std::unique_ptr<IM3ProviderTransport> transport =
            std::make_unique<CapturingTransport>(device, probe);
        return Result<std::unique_ptr<IM3ProviderTransport>>::success(
            std::move(transport));
    };
    return result;
}

M3ProviderConfig config(const TempBundle& bundle) {
    return {bundle.root, "disk.rdm", {limits()}, store_options(),
            std::chrono::milliseconds{5000}};
}

struct ShmClient final {
    UniqueHandle mapping;
    UniqueView view;
    UniqueHandle request;
    UniqueHandle response;

    static ShmClient open(const proxy::ProxyObjectNames& names) {
        ShmClient client;
        client.mapping = UniqueHandle(OpenFileMappingW(
            FILE_MAP_READ | FILE_MAP_WRITE, FALSE, names.mapping.c_str()));
        REQUIRE(client.mapping);
        client.view = UniqueView(MapViewOfFile(
            client.mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            proxy::kMappingSize));
        REQUIRE(client.view);
        client.request = UniqueHandle(OpenEventW(
            EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, names.request.c_str()));
        client.response = UniqueHandle(OpenEventW(
            SYNCHRONIZE, FALSE, names.response.c_str()));
        REQUIRE(client.request && client.response);
        return client;
    }

    std::span<std::byte> bytes() noexcept {
        return {static_cast<std::byte*>(view.get()), proxy::kMappingSize};
    }
};

template <class Request>
void send(ShmClient& client, const Request& request) {
    REQUIRE(proxy::write_wire(
        client.bytes().first(proxy::kHeaderSize), 0, request).ok());
    REQUIRE(SetEvent(client.request.get()) != FALSE);
    REQUIRE_EQ(WaitForSingleObject(client.response.get(), 5000),
               static_cast<DWORD>(WAIT_OBJECT_0));
}

} // namespace

TEST_CASE(m3_provider_recovered_lazy_store_checkpoints_and_reopens) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto first_probe = std::make_shared<TransportProbe>();
    auto first = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(first_probe));
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value()->generation(), 0ull);
    REQUIRE(first.value()->start(context()).ok());
    REQUIRE(first_probe->device != nullptr);

    const auto before_lazy = first.value()->storage_stats();
    std::vector<std::byte> lazy(kM2BlockSize);
    REQUIRE(first_probe->device->read(
        context(), 3 * kM2BlockSize, lazy).ok());
    const auto after_lazy = first.value()->storage_stats();
    REQUIRE_EQ(before_lazy.backing_reads_total, 0ull);
    REQUIRE_EQ(after_lazy.backing_reads_total, 1ull);

    std::vector<std::byte> data(kM2BlockSize, std::byte{0x6d});
    REQUIRE(first_probe->device->write(context(), 0, data).ok());
    const DataRange zero{kM2BlockSize, kM2BlockSize};
    REQUIRE(first_probe->device->zero(context(), {&zero, 1}).ok());
    const DataRange discard{2 * kM2BlockSize, kM2BlockSize};
    REQUIRE(first_probe->device->unmap(context(), {&discard, 1}).ok());

    const auto checkpoint = first.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(checkpoint.ok());
    REQUIRE_EQ(checkpoint.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(checkpoint.generation, 1ull);
    const auto stopped = first.value()->stop(context(), {});
    REQUIRE(stopped.ok());
    REQUIRE_EQ(stopped.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(first_probe->events,
               (std::vector<std::string>{"create", "start", "stop"}));

    auto second_probe = std::make_shared<TransportProbe>();
    auto second = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(second_probe));
    REQUIRE(second.ok());
    REQUIRE_EQ(second.value()->generation(), 1ull);
    REQUIRE(second.value()->start(context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(second_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, data);
    std::fill(actual.begin(), actual.end(), std::byte{0xff});
    REQUIRE(second_probe->device->read(
        context(), kM2BlockSize, actual).ok());
    REQUIRE(std::all_of(actual.begin(), actual.end(),
                        [](std::byte value) { return value == std::byte{}; }));
    REQUIRE(second.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_stop_checkpoints_only_after_transport_is_stopped) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x44});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    const auto stopped = provider.value()->stop(
        context(), {true, VolumeBarrierKind::RawBlockTestNoFilesystemCache});
    REQUIRE(stopped.ok());
    REQUIRE(stopped.checkpoint_completed);
    REQUIRE_EQ(stopped.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(stopped.generation, 1ull);
    REQUIRE_EQ(probe->events,
               (std::vector<std::string>{"create", "start", "stop"}));
}

TEST_CASE(m3_provider_repeated_checkpoints_advance_live_backing_generation) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());

    std::vector<std::byte> first_data(kM2BlockSize, std::byte{0x41});
    REQUIRE(probe->device->write(context(), 0, first_data).ok());
    const auto first = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.generation, 1ull);
    REQUIRE_EQ(provider.value()->storage_stats().backing_generation, 1ull);

    std::vector<std::byte> second_data(kM2BlockSize, std::byte{0x42});
    REQUIRE(probe->device->write(context(), 0, second_data).ok());
    const auto second = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(second.ok());
    REQUIRE_EQ(second.generation, 2ull);
    REQUIRE_EQ(provider.value()->storage_stats().backing_generation, 2ull);
    REQUIRE(provider.value()->stop(context(), {}).ok());

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 2ull);
    REQUIRE(reopened.value()->start(context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(reopened_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, second_data);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_transport_stop_failure_blocks_checkpoint_but_drains_store) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    probe->fail_stop = true;
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x77});
    REQUIRE(probe->device->write(context(), 0, data).ok());

    const auto stopped = provider.value()->stop(
        context(), {true, VolumeBarrierKind::RawBlockTestNoFilesystemCache});
    REQUIRE(!stopped.ok());
    REQUIRE(!stopped.transport_stopped);
    REQUIRE(!stopped.checkpoint_completed);
    REQUIRE_EQ(stopped.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(stopped.generation, 0ull);
    REQUIRE(stopped.store_closed);
    REQUIRE(stopped.session_closed);
    REQUIRE(stopped.writer_closed);
    REQUIRE(stopped.error.has_value());

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 0ull);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_serializes_checkpoint_and_stop_without_losing_commit) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();

    struct CheckpointGate final {
        std::mutex mutex;
        std::condition_variable changed;
        bool checkpoint_entered{};
        bool release_checkpoint{};
    };
    auto gate = std::make_shared<CheckpointGate>();
    auto test_factories = factories(probe);
    test_factories.before_checkpoint_prepare = [gate]() {
        std::unique_lock lock(gate->mutex);
        gate->checkpoint_entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [gate] {
            return gate->release_checkpoint;
        });
        return Result<void>::success();
    };

    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(test_factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x39});
    REQUIRE(probe->device->write(context(), 0, data).ok());

    auto checkpoint = std::async(std::launch::async, [&provider] {
        return provider.value()->checkpoint(
            context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(
            lock, std::chrono::seconds(5), [gate] {
                return gate->checkpoint_entered;
            }));
    }

    auto stop = std::async(std::launch::async, [&provider] {
        return provider.value()->stop(context(), {});
    });
    REQUIRE_EQ(stop.wait_for(std::chrono::milliseconds(100)),
               std::future_status::timeout);
    {
        std::scoped_lock lock(gate->mutex);
        gate->release_checkpoint = true;
    }
    gate->changed.notify_all();

    const auto checkpoint_result = checkpoint.get();
    REQUIRE(checkpoint_result.ok());
    REQUIRE_EQ(checkpoint_result.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(checkpoint_result.generation, 1ull);
    const auto stop_result = stop.get();
    REQUIRE(stop_result.ok());
    REQUIRE_EQ(stop_result.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(stop_result.generation, 1ull);
    REQUIRE_EQ(probe->events,
               (std::vector<std::string>{"create", "start", "stop"}));

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 1ull);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_normalizes_preparation_failures_with_full_context) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto configured = factories(probe);
    configured.before_checkpoint_prepare = [] {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(
                DynamicBlockStoreErrorCode::SnapshotBusy),
            L"snapshot", L"injected active snapshot", 170});
    };

    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(configured));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());

    const auto outcome = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(!outcome.ok());
    REQUIRE_EQ(outcome.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(outcome.generation, 1ull);
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.error_context.has_value());
    REQUIRE_EQ(outcome.error->domain, ErrorDomain::persistence);
    REQUIRE_EQ(outcome.error->code,
               static_cast<std::uint32_t>(PersistenceErrorCode::SnapshotBusy));
    REQUIRE(is_complete_error_context(*outcome.error_context));
    REQUIRE_EQ(outcome.error_context->operation,
               std::string{"checkpoint.prepare"});
    REQUIRE_EQ(outcome.error_context->native_code, 170u);
    REQUIRE_EQ(outcome.error_context->generation, outcome.generation);
    REQUIRE_EQ(outcome.error_context->disposition, outcome.disposition);

    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_stop_retries_pending_reconciliation_before_close) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto reconcile_attempts = std::make_shared<std::uint32_t>(0);
    auto test_factories = factories(probe);
    test_factories.before_checkpoint_reconcile = [reconcile_attempts]() {
        if ((*reconcile_attempts)++ == 0) {
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::VerificationFailed),
                L"provider_reconcile_test", L"injected first failure"});
        }
        return Result<void>::success();
    };

    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(test_factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x5c});
    REQUIRE(probe->device->write(context(), 0, data).ok());

    const auto first = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(!first.ok());
    REQUIRE_EQ(first.disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE_EQ(first.generation, 1ull);
    REQUIRE(first.error.has_value());
    REQUIRE(first.error_context.has_value());
    REQUIRE(is_complete_error_context(*first.error_context));
    REQUIRE_EQ(first.error_context->code,
               PersistenceErrorCode::VerificationFailed);
    REQUIRE_EQ(first.error_context->operation,
               std::string{"checkpoint.reconcile"});
    REQUIRE_EQ(first.error_context->generation, first.generation);
    REQUIRE_EQ(first.error_context->disposition, first.disposition);
    REQUIRE_EQ(provider.value()->storage_stats().backing_generation, 0ull);

    const auto stopped = provider.value()->stop(context(), {});
    REQUIRE(stopped.ok());
    REQUIRE_EQ(stopped.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(stopped.generation, 1ull);
    REQUIRE_EQ(*reconcile_attempts, 2u);

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 1ull);
    REQUIRE(reopened.value()->start(context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(reopened_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, data);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_transport_error_preserves_pending_durable_disposition) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto test_factories = factories(probe);
    test_factories.before_checkpoint_reconcile = [] {
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(
                PersistenceErrorCode::VerificationFailed),
            L"provider_reconcile_test", L"injected persistent failure"});
    };
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(test_factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x71});
    REQUIRE(probe->device->write(context(), 0, data).ok());

    const auto checkpoint = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(!checkpoint.ok());
    REQUIRE_EQ(checkpoint.disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    probe->fail_stop = true;

    const auto stopped = provider.value()->stop(context(), {});
    REQUIRE(!stopped.ok());
    REQUIRE(!stopped.transport_stopped);
    REQUIRE_EQ(stopped.disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE_EQ(stopped.generation, 1ull);
    REQUIRE(stopped.error.has_value());
    REQUIRE(stopped.error_context.has_value());
    REQUIRE(is_complete_error_context(*stopped.error_context));
    REQUIRE_EQ(stopped.error_context->operation,
               std::string{"stop.transport"});
    REQUIRE_EQ(stopped.error_context->generation, 1ull);
    REQUIRE_EQ(stopped.error_context->disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE_EQ(stopped.error->domain, ErrorDomain::protocol);
    REQUIRE_EQ(stopped.error->code, 0x3901u);
    REQUIRE(stopped.store_closed);
    REQUIRE(stopped.session_closed);
    REQUIRE(stopped.writer_closed);

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 1ull);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_transport_exception_is_first_error_and_cleanup_continues) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    probe->throw_stop = true;
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());

    const auto stopped = provider.value()->stop(context(), {true, std::nullopt});
    REQUIRE(!stopped.ok());
    REQUIRE(!stopped.transport_stopped);
    REQUIRE(!stopped.checkpoint_completed);
    REQUIRE(stopped.store_closed);
    REQUIRE(stopped.session_closed);
    REQUIRE(stopped.writer_closed);
    REQUIRE(stopped.error.has_value());
    REQUIRE(stopped.error_context.has_value());
    REQUIRE(is_complete_error_context(*stopped.error_context));
    REQUIRE_EQ(stopped.error_context->operation,
               std::string{"stop.transport"});
    REQUIRE_EQ(stopped.error_context->generation, 0ull);
    REQUIRE_EQ(stopped.error_context->disposition,
               CommitDisposition::NotCommitted);
    REQUIRE_EQ(stopped.error->code,
               static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat));
    REQUIRE_EQ(stopped.error->operation, L"M3Provider::stop");
}

TEST_CASE(m3_provider_default_transport_preserves_m1_wire_contract) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    ShmClient client = ShmClient::open(provider.value()->names());

    send(client, proxy::kReqInfo);
    const auto info = proxy::read_wire<proxy::InfoResponse>(
        std::span<const std::byte>(client.bytes().data(), proxy::kHeaderSize), 0);
    REQUIRE(info.ok());
    REQUIRE_EQ(info.value().file_size, 8ull * 1024 * 1024);
    REQUIRE_EQ(info.value().req_alignment, 512u);

    constexpr std::uint64_t length = 512;
    std::fill_n(client.bytes().data() + proxy::kHeaderSize,
                static_cast<std::size_t>(length), std::byte{0x5a});
    send(client, proxy::WriteRequest{proxy::kReqWrite, 0, length});
    const auto written = proxy::read_wire<proxy::WriteResponse>(
        std::span<const std::byte>(client.bytes().data(), proxy::kHeaderSize), 0);
    REQUIRE(written.ok());
    REQUIRE_EQ(written.value().errorno, 0u);
    REQUIRE_EQ(written.value().length, length);
    std::fill_n(client.bytes().data() + proxy::kHeaderSize,
                static_cast<std::size_t>(length), std::byte{});
    send(client, proxy::ReadRequest{proxy::kReqRead, 0, length});
    const auto read = proxy::read_wire<proxy::ReadResponse>(
        std::span<const std::byte>(client.bytes().data(), proxy::kHeaderSize), 0);
    REQUIRE(read.ok());
    REQUIRE_EQ(read.value().errorno, 0u);
    REQUIRE_EQ(read.value().length, length);
    for (std::size_t index = 0; index < length; ++index) {
        REQUIRE_EQ(client.bytes()[proxy::kHeaderSize + index], std::byte{0x5a});
    }
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_exposes_test_only_checkpoint_durability_phases) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto configured = factories(probe);
    std::vector<std::string> observed;
    configured.before_checkpoint_prepare = [&] {
        observed.emplace_back("prepare.before");
        return Result<void>::success();
    };
    configured.journal_phase_hook = [&](const JournalWriterPhaseEvent& event) {
        if (event.phase == JournalWriterPhase::BeforeFlush) {
            observed.emplace_back("journal.flush.before");
        } else if (event.phase == JournalWriterPhase::AfterFlush) {
            observed.emplace_back("journal.flush.after");
        }
        return Result<void>::success();
    };
    configured.manifest_phase_hook = [&](const ManifestPublishTestPhase phase) {
        if (phase == ManifestPublishTestPhase::BeforeSlotWrite) {
            observed.emplace_back("manifest.slot_write.before");
        } else if (phase == ManifestPublishTestPhase::AfterSlotWrite) {
            observed.emplace_back("manifest.slot_write.after");
        } else if (phase == ManifestPublishTestPhase::BeforeFlush) {
            observed.emplace_back("manifest.flush.before");
        } else if (phase == ManifestPublishTestPhase::AfterFlush) {
            observed.emplace_back("manifest.flush.after");
        } else if (phase == ManifestPublishTestPhase::AfterReadback) {
            observed.emplace_back("manifest.readback.after");
        }
        return Result<void>::success();
    };
    configured.reconcile_phase_hook = [&observed](
        const CheckpointReconcileTestPhase phase) {
        if (phase == CheckpointReconcileTestPhase::BeforeRecoverView) {
            observed.emplace_back("reconcile.before_recover_view");
        } else if (phase ==
                   CheckpointReconcileTestPhase::BeforeReportSuccess) {
            observed.emplace_back("reconcile.before_report_success");
        }
        return Result<void>::success();
    };

    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(configured));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x4d});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    const auto checkpoint = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(checkpoint.ok());

    const std::vector<std::string> required{
        "prepare.before",
        "journal.flush.before",
        "journal.flush.after",
        "manifest.slot_write.before",
        "manifest.slot_write.after",
        "manifest.flush.before",
        "manifest.flush.after",
        "manifest.readback.after",
        "reconcile.before_recover_view",
        "reconcile.before_report_success",
    };
    std::size_t cursor = 0;
    for (const auto& item : observed) {
        if (cursor < required.size() && item == required[cursor]) ++cursor;
    }
    REQUIRE_EQ(cursor, required.size());
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_active_compaction_uses_read_only_boundary_and_keeps_io_live) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());

    std::vector<std::byte> first(kM2BlockSize, std::byte{0x61});
    REQUIRE(probe->device->write(context(), 0, first).ok());
    const auto checkpoint = provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(checkpoint.ok());
    REQUIRE_EQ(checkpoint.generation, 1ull);

    bool boundary_called{};
    std::vector<std::byte> second(kM2BlockSize, std::byte{0x62});
    const auto compacted = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7301},
        [&](const M3CompactionBoundaryEvidence& evidence)
            -> Result<M3CompactionBoundaryDecision> {
            boundary_called = true;
            REQUIRE_EQ(evidence.operation_id, 7301ull);
            REQUIRE_EQ(evidence.source_generation, 1ull);
            REQUIRE_EQ(evidence.candidate_generation, 2ull);
            REQUIRE(evidence.required_peak_free_bytes > 0);
            REQUIRE(evidence.available_free_bytes
                    >= evidence.required_peak_free_bytes);
            REQUIRE(!std::filesystem::exists(
                bundle.root / L"base-g0000-c2.vhdx"));
            REQUIRE(!std::filesystem::exists(
                bundle.root / L"journal-e0002-c2.rdj"));

            std::vector<std::byte> actual(kM2BlockSize);
            REQUIRE(probe->device->read(context(), 0, actual).ok());
            REQUIRE_EQ(actual, first);
            REQUIRE(probe->device->write(
                context(), kM2BlockSize, second).ok());
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE(boundary_called);
    REQUIRE(compacted.ok());
    REQUIRE_EQ(compacted.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(compacted.generation, 2ull);
    REQUIRE_EQ(compacted.metrics.source_generation, 1ull);
    REQUIRE_EQ(compacted.metrics.committed_generation, 2ull);
    REQUIRE(compacted.metrics.candidate_base_bytes > 0);
    REQUIRE(compacted.metrics.candidate_journal_bytes >= kJournalAlignment);
    REQUIRE_EQ(compacted.metrics.retained_candidate_count, 2ull);
    REQUIRE_EQ(compacted.metrics.deleted_candidate_count, 0ull);
    REQUIRE_EQ(compacted.metrics.required_peak_free_bytes,
               compacted.boundary.required_peak_free_bytes);
    REQUIRE_EQ(provider.value()->generation(), 2ull);
    REQUIRE_EQ(provider.value()->storage_stats().backing_generation, 2ull);

    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, first);
    REQUIRE(probe->device->read(
        context(), kM2BlockSize, actual).ok());
    REQUIRE_EQ(actual, second);
    REQUIRE(provider.value()->stop(context(), {}).ok());

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 2ull);
    REQUIRE(reopened.value()->start(context()).ok());
    REQUIRE(reopened_probe->device->read(
        context(), kM2BlockSize, actual).ok());
    REQUIRE_EQ(actual, second);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_denied_boundary_creates_no_artifacts) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x31});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());

    const auto denied = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7302},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {false, 0, 0, evidence});
        }, accept_compaction_cut);

    REQUIRE_EQ(denied.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"base-g0000-c2.vhdx"));
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"journal-e0002-c2.rdj"));
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_requires_durable_continue_revisions) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x35});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());

    const auto denied = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7306},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 0, 0, evidence});
        }, accept_compaction_cut);

    REQUIRE_EQ(denied.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"base-g0000-c2.vhdx"));
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"journal-e0002-c2.rdj"));
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_requires_exact_persisted_snapshot_cut) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x36});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());
    bool cut_called{};

    const auto denied = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7307},
        [](const M3CompactionBoundaryEvidence& evidence) {
            REQUIRE(!evidence.cut_dirty_epoch_known);
            REQUIRE_EQ(evidence.cut_dirty_epoch, 0U);
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        },
        [&](const M3CompactionBoundaryEvidence& evidence) {
            cut_called = true;
            REQUIRE(evidence.cut_dirty_epoch_known);
            return Result<M3CompactionBoundaryDecision>::success(
                {false, 303, 404, evidence});
        });

    REQUIRE(cut_called);
    REQUIRE_EQ(denied.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"base-g0000-c2.tmp"));
    REQUIRE(std::filesystem::exists(
        bundle.root / L"base-g0000-c2.vhdx"));
    REQUIRE(std::filesystem::exists(
        bundle.root / L"journal-e0002-c2.rdj"));

    const auto retried = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7310},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 505, 606, evidence});
        }, accept_compaction_cut);
    REQUIRE(retried.ok());
    REQUIRE_EQ(retried.generation, 2U);
    REQUIRE(retried.metrics.deleted_candidate_count >= 2U);
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_precommit_fault_preserves_old_generation) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto configured = factories(probe);
    bool inject_compaction_fault{};
    configured.manifest_phase_hook = [&](ManifestPublishTestPhase phase) {
        if (inject_compaction_fault
            && phase == ManifestPublishTestPhase::BeforeSlotWrite) {
            return Result<void>::failure({
                ErrorDomain::persistence, 0x39f1,
                L"compaction.test", L"injected precommit fault"});
        }
        return Result<void>::success();
    };
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(configured));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x32});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());
    inject_compaction_fault = true;

    const auto failed = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7303},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE_EQ(failed.disposition, CommitDisposition::NotCommitted);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    inject_compaction_fault = false;
    const auto resolved_source = provider.value()->reconcile_compaction(
        context(), failed.boundary);
    REQUIRE_EQ(
        resolved_source.disposition,
        CommitDisposition::NotCommitted);
    REQUIRE_EQ(resolved_source.generation, 1ull);
    REQUIRE(provider.value()->stop(context(), {}).ok());
    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 1ull);
    REQUIRE(reopened.value()->start(context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(reopened_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, payload);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_postcommit_reconcile_fault_reopens_candidate) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto configured = factories(probe);
    bool inject_compaction_fault{};
    configured.reconcile_phase_hook = [&] (
        CheckpointReconcileTestPhase phase) {
        if (inject_compaction_fault
            && phase == CheckpointReconcileTestPhase::BeforeRecoverView) {
            return Result<void>::failure({
                ErrorDomain::persistence, 0x39f2,
                L"compaction.test", L"injected reconciliation fault"});
        }
        return Result<void>::success();
    };
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), std::move(configured));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x33});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());
    inject_compaction_fault = true;

    const auto unresolved = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7304},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE_EQ(
        unresolved.disposition,
        CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    inject_compaction_fault = false;
    const auto resolved = provider.value()->reconcile_compaction(
        context(), unresolved.boundary);
    REQUIRE(resolved.ok());
    REQUIRE_EQ(resolved.generation, 2ull);
    REQUIRE_EQ(provider.value()->generation(), 2ull);

    const auto followup = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7311},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 707, 808, evidence});
        }, accept_compaction_cut);
    REQUIRE(followup.ok());
    REQUIRE_EQ(followup.generation, 3U);
    REQUIRE_EQ(followup.metrics.deleted_candidate_count, 2U);
    REQUIRE(!std::filesystem::exists(bundle.root / L"base-0000.vhdx"));
    REQUIRE(!std::filesystem::exists(bundle.root / L"journal-e0001.rdj"));
    REQUIRE(provider.value()->stop(context(), {}).ok());
    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(reopened_probe));
    REQUIRE(reopened.ok());
    REQUIRE_EQ(reopened.value()->generation(), 3ull);
    REQUIRE(reopened.value()->start(context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(reopened_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, payload);
    REQUIRE(reopened.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_compaction_low_space_returns_exact_preflight_proof) {
    TempBundle bundle;
    create_bundle(bundle);
    ULARGE_INTEGER available{};
    REQUIRE(GetDiskFreeSpaceExW(
        bundle.root.c_str(), &available, nullptr, nullptr) != FALSE);
    REQUIRE(available.QuadPart
        < (std::numeric_limits<std::uint64_t>::max)() - (128ULL << 20));
    auto constrained = config(bundle);
    constrained.recovery.limits.max_journal_committed_bytes =
        available.QuadPart + (128ULL << 20);

    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, constrained, factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> payload(kM2BlockSize, std::byte{0x34});
    REQUIRE(probe->device->write(context(), 0, payload).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());
    bool boundary_called{};

    const auto deferred = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7305},
        [&](const M3CompactionBoundaryEvidence& evidence) {
            boundary_called = true;
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE(!boundary_called);
    REQUIRE_EQ(deferred.disposition, CommitDisposition::NotCommitted);
    REQUIRE(deferred.error.has_value());
    REQUIRE(deferred.error_context.has_value());
    REQUIRE_EQ(
        deferred.error_context->code,
        PersistenceErrorCode::DiskFull);
    REQUIRE_EQ(deferred.boundary.operation_id, 7305ull);
    REQUIRE_EQ(deferred.boundary.source_generation, 1ull);
    REQUIRE_EQ(deferred.boundary.candidate_generation, 2ull);
    REQUIRE(deferred.boundary.required_peak_free_bytes
        > deferred.boundary.available_free_bytes);
    REQUIRE_EQ(
        deferred.metrics.required_peak_free_bytes,
        deferred.boundary.required_peak_free_bytes);
    REQUIRE_EQ(provider.value()->generation(), 1ull);
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"base-g0000-c2.vhdx"));
    REQUIRE(!std::filesystem::exists(
        bundle.root / L"journal-e0002-c2.rdj"));
    REQUIRE(provider.value()->stop(context(), {}).ok());
}

TEST_CASE(m3_provider_second_compaction_deletes_only_unreferenced_retired_pair) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> first(kM2BlockSize, std::byte{0x37});
    REQUIRE(probe->device->write(context(), 0, first).ok());
    REQUIRE(provider.value()->checkpoint(
        context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache).ok());

    const auto first_compaction = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7308},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);
    REQUIRE(first_compaction.ok());
    REQUIRE_EQ(first_compaction.generation, 2U);
    REQUIRE_EQ(first_compaction.metrics.retained_candidate_count, 2U);
    REQUIRE_EQ(first_compaction.metrics.deleted_candidate_count, 0U);

    std::vector<std::byte> second(kM2BlockSize, std::byte{0x38});
    REQUIRE(probe->device->write(context(), kM2BlockSize, second).ok());
    const auto second_compaction = provider.value()->compact(
        context(),
        M3CompactionRequest{
            VolumeBarrierKind::RawBlockTestNoFilesystemCache, 7309},
        [](const M3CompactionBoundaryEvidence& evidence) {
            return Result<M3CompactionBoundaryDecision>::success(
                {true, 303, 404, evidence});
        }, accept_compaction_cut);

    REQUIRE(second_compaction.ok());
    REQUIRE_EQ(second_compaction.generation, 3U);
    REQUIRE_EQ(second_compaction.metrics.deleted_candidate_count, 2U);
    REQUIRE_EQ(second_compaction.metrics.retained_candidate_count, 2U);
    REQUIRE(!std::filesystem::exists(bundle.root / L"base-0000.vhdx"));
    REQUIRE(!std::filesystem::exists(bundle.root / L"journal-e0001.rdj"));
    REQUIRE(std::filesystem::exists(bundle.root / L"base-g0000-c2.vhdx"));
    REQUIRE(std::filesystem::exists(bundle.root / L"journal-e0002-c2.rdj"));
    REQUIRE(provider.value()->stop(context(), {}).ok());
}
