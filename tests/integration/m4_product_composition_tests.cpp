#include "test_framework.h"

#include "src/m3/m3_provider.h"
#include "src/m3/m3_lifecycle_runner.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/writer_lock.h"
#include "src/proxy/object_names.h"
#include "src/common/unique_handle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace {

TEST_CASE(product_defaults_are_shared_m3_values) {
    constexpr std::uint64_t kKiB = 1024ull;
    constexpr std::uint64_t kMiB = 1024ull * kKiB;
    constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr std::uint64_t kTiB = 1024ull * kGiB;
    const auto limits = myrd::m3::production_persistence_limits(8ull * kGiB);
    const auto store = myrd::m3::production_store_options(2ull * kGiB);
    REQUIRE_EQ(limits.max_virtual_bytes, 8ull * kGiB);
    REQUIRE_EQ(limits.max_journal_committed_bytes, 2ull * kGiB);
    REQUIRE_EQ(limits.max_journal_records,
               2ull * kGiB / myrd::persistence::kJournalAlignment - 1);
    REQUIRE_EQ(limits.max_index_cache_bytes, 64ull * kMiB);
    REQUIRE_EQ(limits.max_bat_cache_bytes, 16ull * kMiB);
    REQUIRE_EQ(limits.max_io_scratch_bytes, 8ull * kMiB);
    REQUIRE_EQ(limits.max_verify_batch_bytes, 8ull * kMiB);
    REQUIRE_EQ(store.hard_limit_bytes, 2ull * kGiB);
    REQUIRE_EQ(store.soft_limit_bytes, 1ull * kGiB);
    REQUIRE_EQ(store.eviction_low_watermark_bytes, 512ull * kMiB);
    REQUIRE(myrd::validate_dynamic_block_store_options(store).ok());
    const std::array matrix{
        std::array{64ull * kMiB, 64ull * kMiB, 32ull * kMiB, 16ull * kMiB},
        std::array{1ull * kTiB, 1ull * kTiB, 512ull * kGiB, 256ull * kGiB},
        std::array{65537ull * kKiB, 64ull * kMiB, 32ull * kMiB, 16ull * kMiB}};
    for (const auto& item : matrix) {
        const auto options = myrd::m3::production_store_options(item[0]);
        REQUIRE_EQ(options.hard_limit_bytes, item[1]);
        REQUIRE_EQ(options.soft_limit_bytes, item[2]);
        REQUIRE_EQ(options.eviction_low_watermark_bytes, item[3]);
        REQUIRE(options.soft_limit_bytes < options.hard_limit_bytes);
        REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());
    }
}

TEST_CASE(product_runtime_exposes_maintenance_stats) {
    myrd::m3::M3MaintenanceStats stats{};
    REQUIRE_EQ(stats.generation, 0ull);
    REQUIRE_EQ(stats.dirty_bytes, 0ull);
    REQUIRE_EQ(stats.journal_committed_bytes, 0ull);
}

struct TransportProbe final { myrd::IBlockDevice* device{}; };

class TestTransport final : public myrd::m3::IM3ProviderTransport {
public:
    TestTransport(myrd::IBlockDevice& device, std::shared_ptr<TransportProbe> probe)
        : probe_(std::move(probe)) { probe_->device = &device; }
    myrd::Result<void> start() override { return myrd::Result<void>::success(); }
    myrd::Result<void> stop() override { return myrd::Result<void>::success(); }
    myrd::proxy::SessionSnapshot snapshot() const override { return {}; }
    myrd::proxy::ProxyObjectNames names() const override { return {}; }
    std::shared_ptr<TransportProbe> probe_;
};

struct Bundle final {
    std::filesystem::path root;
    Bundle() : root(std::filesystem::temp_directory_path() /
                    (L"myrd-m4-r3-" + std::to_wstring(
                        std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(root);
        myrd::m3::M3CliConfig create{};
        create.command = myrd::m3::M3CliCommand::Create;
        create.bundle_root = root;
        create.manifest_name = "disk.rdm";
        create.geometry = {8ull * 1024 * 1024, 512, 4096};
        create.persistence_limits = {create.geometry->virtual_bytes,
            64ull * 1024 * 1024, 8192, 64ull * myrd::persistence::kIndexNodeBytes,
            64, 64ull * 4096, 64, 2ull * 1024 * 1024, 2ull * 1024 * 1024};
        create.timeout = std::chrono::seconds(5);
        REQUIRE_EQ(myrd::m3::run_m3_lifecycle(myrd::OperationContext{}, create).exit_code, 0);
    }
    ~Bundle() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

struct ProcessResult final {
    DWORD exit_code{};
    std::string stdout_text;
    std::string stderr_text;
};

std::string read_pipe(HANDLE pipe) {
    std::string result;
    std::array<char, 512> chunk{};
    DWORD read{};
    while (ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                    &read, nullptr) && read != 0) {
        result.append(chunk.data(), read);
    }
    return result;
}

ProcessResult run_product_compact(const std::filesystem::path& root) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdout_read_raw{}, stdout_write_raw{}, stderr_read_raw{}, stderr_write_raw{};
    REQUIRE(CreatePipe(&stdout_read_raw, &stdout_write_raw, &inheritable, 0));
    myrd::UniqueHandle stdout_read(stdout_read_raw), stdout_write(stdout_write_raw);
    REQUIRE(CreatePipe(&stderr_read_raw, &stderr_write_raw, &inheritable, 0));
    myrd::UniqueHandle stderr_read(stderr_read_raw), stderr_write(stderr_write_raw);
    REQUIRE(SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0));
    REQUIRE(SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0));

    std::wstring command = L"\"" MYRD_PRODUCT_EXECUTABLE
        L"\" compact --bundle \"" + root.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(MYRD_PRODUCT_EXECUTABLE, mutable_command.data(),
                           nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process));
    myrd::UniqueHandle child(process.hProcess), thread(process.hThread);
    stdout_write.reset();
    stderr_write.reset();
    REQUIRE_EQ(WaitForSingleObject(child.get(), 10'000), WAIT_OBJECT_0);
    ProcessResult result{};
    REQUIRE(GetExitCodeProcess(child.get(), &result.exit_code));
    result.stdout_text = read_pipe(stdout_read.get());
    result.stderr_text = read_pipe(stderr_read.get());
    return result;
}

TEST_CASE(product_provider_stats_are_current_and_checked) {
    Bundle bundle;
    myrd::m3::M3ProviderConfig config{};
    config.bundle_root = bundle.root;
    config.manifest_name = "disk.rdm";
    config.recovery = {{8ull * 1024 * 1024, 64ull * 1024 * 1024, 8192,
        64ull * myrd::persistence::kIndexNodeBytes, 64, 64ull * 4096, 64,
        2ull * 1024 * 1024, 2ull * 1024 * 1024}};
    config.store = {2ull * myrd::persistence::kM2BlockSize,
        4ull * myrd::persistence::kM2BlockSize,
        myrd::persistence::kM2BlockSize, 2ull * myrd::persistence::kM2BlockSize,
        16, 64, 4ull * myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250}};
    config.request_timeout = std::chrono::seconds(5);
    myrd::proxy::LocalNamespacePolicy policy;
    myrd::m3::M3ProviderFactories factories{};
    auto probe = std::make_shared<TransportProbe>();
    factories.transport = [probe](myrd::IBlockDevice& device, const myrd::proxy::IProxyNamespacePolicy&,
                             myrd::proxy::ShmProxySessionOptions) {
        return myrd::Result<std::unique_ptr<myrd::m3::IM3ProviderTransport>>::success(
            std::make_unique<TestTransport>(device, probe));
    };
    auto provider = myrd::m3::M3Provider::open_for_test(
        myrd::OperationContext{}, policy, config, std::move(factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(myrd::OperationContext{}).ok());
    REQUIRE(probe->device != nullptr);
    std::vector<std::byte> dirty(myrd::persistence::kM2BlockSize, std::byte{0x5a});
    REQUIRE(probe->device->write(myrd::OperationContext{}, 0, dirty).ok());
    const auto stats = provider.value()->maintenance_stats();
    REQUIRE(stats.ok());
    REQUIRE_EQ(stats.value().generation, 0ull);
    REQUIRE_EQ(stats.value().dirty_bytes, myrd::persistence::kM2BlockSize);
    REQUIRE_EQ(stats.value().journal_committed_bytes, 8192ull);
    REQUIRE(provider.value()->stop(myrd::OperationContext{}, {}).ok());
}

TEST_CASE(product_provider_preserves_journal_capacity_and_durable_generation) {
    Bundle bundle;
    myrd::m3::M3ProviderConfig config{};
    config.bundle_root = bundle.root;
    config.manifest_name = "disk.rdm";
    config.recovery = {{8ull * 1024 * 1024, 64ull * 1024 * 1024, 8192,
        64ull * myrd::persistence::kIndexNodeBytes, 64, 64ull * 4096, 64,
        2ull * 1024 * 1024, 2ull * 1024 * 1024}};
    config.store = {2ull * myrd::persistence::kM2BlockSize,
        4ull * myrd::persistence::kM2BlockSize,
        myrd::persistence::kM2BlockSize, 2ull * myrd::persistence::kM2BlockSize,
        16, 64, 4ull * myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250}};
    config.request_timeout = std::chrono::seconds(5);
    config.journal_committed_end_ceiling = 8192ull;
    myrd::proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    myrd::m3::M3ProviderFactories factories{};
    factories.transport = [probe](myrd::IBlockDevice& device,
                                  const myrd::proxy::IProxyNamespacePolicy&,
                                  myrd::proxy::ShmProxySessionOptions) {
        return myrd::Result<std::unique_ptr<myrd::m3::IM3ProviderTransport>>::success(
            std::make_unique<TestTransport>(device, probe));
    };
    auto provider = myrd::m3::M3Provider::open_for_test(
        myrd::OperationContext{}, policy, config, std::move(factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(myrd::OperationContext{}).ok());
    std::vector<std::byte> dirty(myrd::persistence::kM2BlockSize, std::byte{0x33});
    REQUIRE(probe->device->write(myrd::OperationContext{}, 0, dirty).ok());
    const auto before = provider.value()->maintenance_stats();
    REQUIRE(before.ok());
    const auto outcome = provider.value()->checkpoint(
        myrd::OperationContext{}, myrd::persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(outcome.error->code,
               static_cast<std::uint32_t>(myrd::persistence::PersistenceErrorCode::JournalCapacityExhausted));
    REQUIRE(outcome.error_context.has_value());
    REQUIRE_EQ(outcome.error_context->code,
               myrd::persistence::PersistenceErrorCode::JournalCapacityExhausted);
    REQUIRE_EQ(outcome.disposition, myrd::persistence::CommitDisposition::NotCommitted);
    REQUIRE_EQ(outcome.generation, before.value().generation + 1);
    const auto after = provider.value()->maintenance_stats();
    REQUIRE(after.ok());
    REQUIRE_EQ(after.value().generation, before.value().generation);
    REQUIRE_EQ(after.value().journal_committed_bytes,
               before.value().journal_committed_bytes);
    REQUIRE(provider.value()->stop(myrd::OperationContext{}, {}).ok());
}

TEST_CASE(product_offline_compaction_commits_successor_and_respects_writer_lock) {
    Bundle bundle;
    myrd::m3::M3ProviderConfig config{};
    config.bundle_root = bundle.root;
    config.manifest_name = "disk.rdm";
    config.recovery = {{8ull * 1024 * 1024, 64ull * 1024 * 1024, 8192,
        64ull * myrd::persistence::kIndexNodeBytes, 64, 64ull * 4096, 64,
        2ull * 1024 * 1024, 2ull * 1024 * 1024}};
    config.store = {2ull * myrd::persistence::kM2BlockSize,
        4ull * myrd::persistence::kM2BlockSize,
        myrd::persistence::kM2BlockSize, 2ull * myrd::persistence::kM2BlockSize,
        16, 64, 4ull * myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250}};
    config.request_timeout = std::chrono::seconds(5);
    myrd::proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    myrd::m3::M3ProviderFactories factories{};
    factories.transport = [probe](myrd::IBlockDevice& device,
                                  const myrd::proxy::IProxyNamespacePolicy&,
                                  myrd::proxy::ShmProxySessionOptions) {
        return myrd::Result<std::unique_ptr<myrd::m3::IM3ProviderTransport>>::success(
            std::make_unique<TestTransport>(device, probe));
    };
    auto provider = myrd::m3::M3Provider::open_for_test(
        myrd::OperationContext{}, policy, config, std::move(factories));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(myrd::OperationContext{}).ok());
    const std::vector<std::byte> expected(myrd::persistence::kM2BlockSize,
                                          std::byte{0x6c});
    REQUIRE(probe->device->write(myrd::OperationContext{}, 0, expected).ok());
    const auto checkpoint = provider.value()->checkpoint(
        myrd::OperationContext{}, myrd::persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(checkpoint.ok());
    REQUIRE_EQ(checkpoint.generation, 1ull);
    REQUIRE(provider.value()->stop(myrd::OperationContext{}, {}).ok());

    auto held = myrd::persistence::acquire_bundle_writer_lock(bundle.root, "disk.rdm");
    REQUIRE(held.ok());
    auto before_conflict = myrd::persistence::recover_complete_image(
        myrd::OperationContext{}, bundle.root, "disk.rdm", config.recovery);
    REQUIRE(before_conflict.ok());
    const auto conflict = myrd::m3::run_product_offline_compaction(
        myrd::OperationContext{}, bundle.root);
    REQUIRE_EQ(conflict.exit_code, 1);
    REQUIRE(conflict.error.has_value());
    REQUIRE_EQ(conflict.error->domain, myrd::ErrorDomain::persistence);
    REQUIRE_EQ(conflict.error->code,
               static_cast<std::uint32_t>(myrd::persistence::PersistenceErrorCode::ImageInUse));
    const auto process_conflict = run_product_compact(bundle.root);
    REQUIRE_EQ(process_conflict.exit_code, 1ul);
    REQUIRE_EQ(process_conflict.stdout_text, std::string{});
    REQUIRE_EQ(process_conflict.stderr_text,
        "Error: bundle is in use. Stop myrd run for this bundle, then retry: "
        "myrd compact --bundle \"" + bundle.root.string() + "\"\r\n");
    auto after_conflict = myrd::persistence::recover_complete_image(
        myrd::OperationContext{}, bundle.root, "disk.rdm", config.recovery);
    REQUIRE(after_conflict.ok());
    REQUIRE_EQ(after_conflict.image->checkpoint_generation,
               before_conflict.image->checkpoint_generation);
    REQUIRE_EQ(after_conflict.image->backing_identity.journal_committed_length,
               before_conflict.image->backing_identity.journal_committed_length);
    REQUIRE(held.value().close().ok());

    const auto compacted = myrd::m3::run_product_offline_compaction(
        myrd::OperationContext{}, bundle.root);
    REQUIRE_EQ(compacted.exit_code, 0);
    REQUIRE_EQ(compacted.disposition, myrd::persistence::CommitDisposition::Committed);
    REQUIRE(compacted.cleanup_succeeded);
    REQUIRE_EQ(compacted.generation, 2ull);
    auto recovered = myrd::persistence::recover_complete_image(
        myrd::OperationContext{}, bundle.root, "disk.rdm", config.recovery);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.image->checkpoint_generation, 2ull);
    auto store = myrd::persistence::open_recovered_dynamic_store(*recovered.image, config.store);
    REQUIRE(store.ok());
    std::vector<std::byte> actual(expected.size());
    REQUIRE(store.value()->read(myrd::OperationContext{}, 0, actual).ok());
    REQUIRE_EQ(actual, expected);
    store.value()->close();
}

TEST_CASE(product_offline_compaction_rejects_generation_zero) {
    Bundle bundle;
    const auto outcome = myrd::m3::run_product_offline_compaction(
        myrd::OperationContext{}, bundle.root);
    REQUIRE_EQ(outcome.exit_code, 1);
    REQUIRE(outcome.error.has_value());
}

}  // namespace
