#include "tests/support/test_framework.h"

#include "src/host/host_core.h"
#include "src/m3/m3_provider.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/vhdx_reader.h"
#include "src/product/product_version.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::host;
using namespace myrd::m3;
using namespace myrd::persistence;

class TempBundle final {
public:
    TempBundle() {
        root = std::filesystem::temp_directory_path()
            / (L"myrd-m4-host-" + std::to_wstring(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
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

Result<HostCompactionBoundaryDecision> accept_compaction_cut(
    const HostCompactionBoundaryEvidence& evidence) {
    REQUIRE(evidence.cut_dirty_epoch_known);
    return Result<HostCompactionBoundaryDecision>::success(
        {true, 303, 404, evidence});
}

PersistenceLimits limits() {
    constexpr std::uint64_t kMiB = 1024 * 1024;
    return {64 * kMiB,
            64 * kMiB,
            8192,
            64 * kIndexNodeBytes,
            64,
            64 * kVhdxBatPageBytes,
            64,
            2 * kMiB,
            2 * kMiB};
}

DynamicBlockStoreOptions store_options() {
    return {2 * kM2BlockSize,
            4 * kM2BlockSize,
            kM2BlockSize,
            2 * kM2BlockSize,
            16,
            64,
            4 * kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250}};
}

M3ProviderConfig config(const TempBundle& bundle) {
    return {bundle.root,
            "disk.rdm",
            {limits()},
            store_options(),
            std::chrono::milliseconds{5000}};
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
    REQUIRE(create_generation_zero_image(OperationContext{}, request).ok());
}

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

WorkerBinding binding() {
    return {
        uuid("00112233-4455-4677-8899-aabbccddeeff"),
        uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
        42,
        100,
        7,
        3,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

HostCommand command(
    const WorkerBinding& target,
    std::string_view operation_id) {
    return {target, uuid(operation_id)};
}

struct TransportProbe final {
    IBlockDevice* device{};
    unsigned create_calls{};
    unsigned factory_calls{};
    unsigned start_calls{};
    unsigned stop_calls{};
    bool fail_start{};
    bool fail_stop{};
    bool fail_create{};
};

class FakeTransport final : public IM3ProviderTransport {
public:
    FakeTransport(IBlockDevice& device, std::shared_ptr<TransportProbe> probe)
        : probe_(std::move(probe)) {
        probe_->device = &device;
        ++probe_->create_calls;
    }

    Result<void> start() override {
        ++probe_->start_calls;
        if (probe_->fail_start) {
            return Result<void>::failure({
                ErrorDomain::protocol,
                0x4D20,
                L"FakeTransport::start",
                L"injected transport start failure"});
        }
        return Result<void>::success();
    }

    Result<void> stop() override {
        ++probe_->stop_calls;
        if (probe_->fail_stop) {
            return Result<void>::failure({
                ErrorDomain::protocol,
                0x4D21,
                L"FakeTransport::stop",
                L"injected transport stop failure",
                170});
        }
        return Result<void>::success();
    }

    proxy::SessionSnapshot snapshot() const override { return {}; }
    proxy::ProxyObjectNames names() const override { return {}; }

private:
    std::shared_ptr<TransportProbe> probe_;
};

M3ProviderFactories factories(const std::shared_ptr<TransportProbe>& probe) {
    M3ProviderFactories value{};
    value.transport = [probe](
        IBlockDevice& device,
        const proxy::IProxyNamespacePolicy&,
        proxy::ShmProxySessionOptions) {
        ++probe->factory_calls;
        if (probe->fail_create) {
            return Result<std::unique_ptr<IM3ProviderTransport>>::failure({
                ErrorDomain::protocol,
                0x4D22,
                L"FakeTransport::create",
                L"injected transport construction failure"});
        }
        std::unique_ptr<IM3ProviderTransport> transport =
            std::make_unique<FakeTransport>(device, probe);
        return Result<std::unique_ptr<IM3ProviderTransport>>::success(
            std::move(transport));
    };
    return value;
}

class AcceptedBundleFactory final : public IHostProviderFactory {
public:
    using ConfigureFactories =
        std::function<void(M3ProviderFactories&)>;

    AcceptedBundleFactory(
        M3ProviderConfig config_value,
        std::shared_ptr<TransportProbe> probe_value,
        ConfigureFactories configure = {})
        : config_(std::move(config_value)), probe_(std::move(probe_value)),
          configure_(std::move(configure)) {}

    Result<std::shared_ptr<IHostProvider>> open(
        const OperationContext& operation_context) override {
        ++open_calls;
        auto configured = factories(probe_);
        if (configure_) configure_(configured);
        auto opened = M3Provider::open_for_test(
            operation_context, policy_, config_, std::move(configured));
        if (!opened.ok()) {
            return Result<std::shared_ptr<IHostProvider>>::failure(
                opened.error());
        }
        return Result<std::shared_ptr<IHostProvider>>::success(
            adapt_m3_provider(opened.move_value()));
    }

    unsigned open_calls{};

private:
    proxy::LocalNamespacePolicy policy_;
    M3ProviderConfig config_;
    std::shared_ptr<TransportProbe> probe_;
    ConfigureFactories configure_;
};

} // namespace

TEST_CASE(host_m3_adapter_uses_real_bundle_without_creating_a_device) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto expected = binding();
    auto probe = std::make_shared<TransportProbe>();
    auto factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), probe);
    auto host = HostCore::create(expected, factory);
    REQUIRE(host.ok());
    REQUIRE_EQ(factory->open_calls, 0U);
    REQUIRE_EQ(probe->create_calls, 0U);

    // A second accepted provider can acquire the writer lock before start,
    // proving HostCore construction did not touch the bundle.
    proxy::LocalNamespacePolicy policy;
    auto independent_probe = std::make_shared<TransportProbe>();
    auto independent = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(independent_probe));
    REQUIRE(independent.ok());
    REQUIRE(independent.value()->stop(context(), {}).ok());

    const auto started = host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000101"),
        context());
    REQUIRE(started.ok());
    REQUIRE_EQ(started.generation, 0ULL);
    REQUIRE_EQ(started.geometry.virtual_bytes, 8ULL * 1024 * 1024);
    REQUIRE_EQ(factory->open_calls, 1U);
    REQUIRE_EQ(probe->create_calls, 1U);
    REQUIRE_EQ(probe->start_calls, 1U);
    REQUIRE(probe->device != nullptr);

    std::vector<std::byte> data(kM2BlockSize, std::byte{0x6D});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    const auto stats = host.value()->storage_stats(expected);
    REQUIRE(stats.ok());
    REQUIRE(stats.value().resident_dirty_blocks > 0);

    const auto checkpointed = host.value()->checkpoint(
        command(expected, "30000000-0000-4000-8000-000000000102"),
        context(),
        HostVolumeBarrierKind::raw_block_test_no_filesystem_cache);
    REQUIRE(checkpointed.ok());
    REQUIRE_EQ(checkpointed.generation, 1ULL);

    bool boundary_called{};
    std::vector<std::byte> post_checkpoint(
        kM2BlockSize, std::byte{0x7e});
    const auto compacted = host.value()->compact(
        command(expected, "30000000-0000-4000-8000-00000000010a"),
        context(),
        {HostVolumeBarrierKind::raw_block_test_no_filesystem_cache, 8301},
        [&](const HostCompactionBoundaryEvidence& evidence)
            -> Result<HostCompactionBoundaryDecision> {
            boundary_called = true;
            REQUIRE_EQ(evidence.native_operation_id, 8301ull);
            REQUIRE_EQ(evidence.source_generation, 1ull);
            REQUIRE_EQ(evidence.candidate_generation, 2ull);
            REQUIRE(probe->device->write(
                context(), kM2BlockSize, post_checkpoint).ok());
            return Result<HostCompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);
    REQUIRE(boundary_called);
    REQUIRE(compacted.ok());
    REQUIRE_EQ(compacted.generation, 2ull);
    REQUIRE_EQ(compacted.metrics.committed_generation, 2ull);

    const auto stopped = host.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000103"),
        context(),
        {});
    REQUIRE(stopped.ok());
    REQUIRE_EQ(stopped.generation, 2ULL);
    REQUIRE_EQ(probe->stop_calls, 1U);

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened_factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), reopened_probe);
    auto reopened = HostCore::create(expected, reopened_factory);
    REQUIRE(reopened.ok());
    REQUIRE(reopened.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000104"),
        context()).ok());
    std::vector<std::byte> actual(kM2BlockSize);
    REQUIRE(reopened_probe->device->read(context(), 0, actual).ok());
    REQUIRE_EQ(actual, data);
    REQUIRE(reopened_probe->device->read(
        context(), kM2BlockSize, actual).ok());
    REQUIRE_EQ(actual, post_checkpoint);
    REQUIRE(reopened.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000105"),
        context(),
        {}).ok());
}

TEST_CASE(host_m3_adapter_preserves_not_committed_compaction_evidence) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto expected = binding();
    auto probe = std::make_shared<TransportProbe>();
    bool inject_compaction_fault{};
    auto factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), probe,
        [&](M3ProviderFactories& configured) {
            configured.manifest_phase_hook =
                [&](ManifestPublishTestPhase phase) {
                    if (inject_compaction_fault
                        && phase == ManifestPublishTestPhase::BeforeSlotWrite) {
                        return Result<void>::failure({
                            ErrorDomain::persistence,
                            0x4df1,
                            L"host.compaction.test",
                            L"injected precommit fault"});
                    }
                    return Result<void>::success();
                };
        });
    auto host = HostCore::create(expected, factory);
    REQUIRE(host.ok());
    REQUIRE(host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000114"),
        context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x54});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    REQUIRE(host.value()->checkpoint(
        command(expected, "30000000-0000-4000-8000-000000000115"),
        context(),
        HostVolumeBarrierKind::raw_block_test_no_filesystem_cache).ok());
    inject_compaction_fault = true;

    const auto compacted = host.value()->compact(
        command(expected, "30000000-0000-4000-8000-000000000116"),
        context(),
        {HostVolumeBarrierKind::raw_block_test_no_filesystem_cache, 8302},
        [](const HostCompactionBoundaryEvidence& evidence) {
            return Result<HostCompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE(!compacted.ok());
    REQUIRE_EQ(compacted.disposition, HostCommitDisposition::not_committed);
    REQUIRE_EQ(compacted.boundary.native_operation_id, 8302ull);
    REQUIRE_EQ(compacted.boundary.source_generation, 1ull);
    REQUIRE_EQ(compacted.generation, 2ull);
    REQUIRE(compacted.error.has_value());
    REQUIRE(compacted.error_context.has_value());
    REQUIRE_EQ(
        compacted.error_context->disposition,
        HostCommitDisposition::not_committed);
    inject_compaction_fault = false;
    const auto resolved_source = host.value()->reconcile_compaction(
        command(expected, "30000000-0000-4000-8000-000000000122"),
        context(), compacted.boundary);
    REQUIRE_EQ(
        resolved_source.disposition,
        HostCommitDisposition::not_committed);
    REQUIRE_EQ(resolved_source.generation, 1ull);
    REQUIRE(host.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000117"),
        context(), {}).ok());
}

TEST_CASE(host_m3_adapter_preserves_low_space_preflight_without_callback) {
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
    const auto expected = binding();
    auto probe = std::make_shared<TransportProbe>();
    auto factory = std::make_shared<AcceptedBundleFactory>(
        constrained, probe);
    auto host = HostCore::create(expected, factory);
    REQUIRE(host.ok());
    REQUIRE(host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000118"),
        context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x55});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    REQUIRE(host.value()->checkpoint(
        command(expected, "30000000-0000-4000-8000-000000000119"),
        context(),
        HostVolumeBarrierKind::raw_block_test_no_filesystem_cache).ok());
    bool boundary_called{};

    const auto deferred = host.value()->compact(
        command(expected, "30000000-0000-4000-8000-000000000120"),
        context(),
        {HostVolumeBarrierKind::raw_block_test_no_filesystem_cache, 8303},
        [&](const HostCompactionBoundaryEvidence& evidence) {
            boundary_called = true;
            return Result<HostCompactionBoundaryDecision>::success(
                {true, 101, 202, evidence});
        }, accept_compaction_cut);

    REQUIRE(!boundary_called);
    REQUIRE(!deferred.ok());
    REQUIRE_EQ(deferred.disposition, HostCommitDisposition::not_committed);
    REQUIRE_EQ(deferred.boundary.native_operation_id, 8303ull);
    REQUIRE(deferred.boundary.required_peak_free_bytes
        > deferred.boundary.available_free_bytes);
    REQUIRE(deferred.error.has_value());
    REQUIRE(deferred.error_context.has_value());
    REQUIRE_EQ(deferred.error_context->code, 13U);
    REQUIRE(host.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000121"),
        context(), {}).ok());
}

TEST_CASE(host_m3_adapter_late_open_failure_releases_bundle_lock) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto expected = binding();
    auto probe = std::make_shared<TransportProbe>();
    probe->fail_create = true;
    auto factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), probe);
    auto host = HostCore::create(expected, factory);
    REQUIRE(host.ok());

    const auto started = host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000106"),
        context());
    REQUIRE(!started.ok());
    REQUIRE(!started.provider_opened);
    REQUIRE(started.error.has_value());
    REQUIRE(!started.cleanup.has_value());
    REQUIRE_EQ(probe->factory_calls, 1U);
    REQUIRE_EQ(probe->create_calls, 0U);
    REQUIRE(!host.value()->provider_state(expected).ok());

    auto retry_probe = std::make_shared<TransportProbe>();
    auto retry_factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), retry_probe);
    auto retry = HostCore::create(expected, retry_factory);
    REQUIRE(retry.ok());
    REQUIRE(retry.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000112"),
        context()).ok());
    REQUIRE(retry.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000113"),
        context(),
        {}).ok());
}

TEST_CASE(host_m3_adapter_start_failure_cleans_up_and_releases_bundle_lock) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto expected = binding();
    auto failing_probe = std::make_shared<TransportProbe>();
    failing_probe->fail_start = true;
    auto failing_factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), failing_probe);
    auto host = HostCore::create(expected, failing_factory);
    REQUIRE(host.ok());

    const auto started = host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000107"),
        context());
    REQUIRE(!started.ok());
    REQUIRE(started.provider_opened);
    REQUIRE(!started.provider_started);
    REQUIRE(started.error.has_value());
    REQUIRE(started.cleanup.has_value());
    REQUIRE(started.cleanup->ok());
    REQUIRE_EQ(failing_probe->start_calls, 1U);
    REQUIRE_EQ(failing_probe->stop_calls, 1U);
    REQUIRE(!host.value()->provider_state(expected).ok());

    auto retry_probe = std::make_shared<TransportProbe>();
    auto retry_factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), retry_probe);
    auto retry = HostCore::create(expected, retry_factory);
    REQUIRE(retry.ok());
    REQUIRE(retry.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000108"),
        context()).ok());
    REQUIRE(retry.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000109"),
        context(),
        {}).ok());
}

TEST_CASE(host_m3_adapter_preserves_real_stop_failure_and_cleanup_evidence) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto expected = binding();
    auto probe = std::make_shared<TransportProbe>();
    probe->fail_stop = true;
    auto factory = std::make_shared<AcceptedBundleFactory>(
        config(bundle), probe);
    auto host = HostCore::create(expected, factory);
    REQUIRE(host.ok());
    REQUIRE(host.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000110"),
        context()).ok());

    const auto stopped = host.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000111"),
        context(),
        {});
    REQUIRE(!stopped.ok());
    REQUIRE(!stopped.transport_stopped);
    REQUIRE(stopped.store_closed);
    REQUIRE(stopped.session_closed);
    REQUIRE(stopped.writer_closed);
    REQUIRE(stopped.error.has_value());
    REQUIRE(stopped.error_context.has_value());
    REQUIRE_EQ(stopped.error->domain, ErrorDomain::protocol);
    REQUIRE_EQ(stopped.error->code, 0x4D21U);
    REQUIRE_EQ(stopped.error->operation, L"FakeTransport::stop");
    REQUIRE_EQ(stopped.error_context->operation, "stop.transport");
    REQUIRE_EQ(
        stopped.error_context->code,
        static_cast<std::uint32_t>(PersistenceErrorCode::IoError));
    REQUIRE_EQ(stopped.error_context->native_code, 170U);
}
