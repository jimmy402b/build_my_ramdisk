#include "src/common/sha256.h"
#include "src/m3/m3_lifecycle_runner.h"
#include "src/m3/m3_provider.h"
#include "src/persistence/file_io.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::m3;
using namespace myrd::persistence;

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kLogicalBytes = 100ULL * kGiB;
constexpr std::size_t kWriteBytes = 4096;
constexpr std::uint64_t kFormalSeedBase = 3221344269ULL;
constexpr std::uint64_t kFormalSeedStride = 104729ULL;
constexpr std::uint64_t kFormalSeedCount = 100;
constexpr std::uint64_t kFormalPayloadBlocks = 4096;
constexpr std::uint64_t kFormalAllocatedProfileSeed = kFormalSeedBase;
constexpr std::uint64_t kFormalAllocatedProfilePayloads =
    kGiB / kVhdxDataBlockBytes;

struct Coverage final {
    bool production_checkpoint{};
    bool reopen_verified{};
    bool compaction_verified{};
    bool concurrent_first_touch{};
    bool cow_overlap{};
    bool io_fault_recovered{};
    bool repeated_close{};
};

struct CleanupGauges final {
    std::uint64_t loading_blocks{};
    std::uint64_t active_snapshot_id{};
    std::uint64_t snapshot_cow_current_bytes{};
    std::uint64_t snapshot_cow_records{};
    std::uint64_t snapshot_blocked_writers{};

    bool empty() const noexcept {
        return loading_blocks == 0 && active_snapshot_id == 0 &&
            snapshot_cow_current_bytes == 0 && snapshot_cow_records == 0 &&
            snapshot_blocked_writers == 0;
    }
};

struct PerformanceMeasurements final {
    std::uint64_t duration_ms{};
    std::vector<std::uint64_t> latency_samples_us{};
    std::uint64_t checkpoint_duration_ms{};
    std::uint64_t compaction_duration_ms{};
};

struct AllocatedProfileMeasurement final {
    std::uint64_t allocated_bytes{};
    std::uint64_t startup_read_bytes{};
    std::uint64_t extra_committed_bytes{};
    std::uint64_t structural_read_bytes{};
    bool verified{};
    bool cleanup_succeeded{true};
};

struct SoakSeedReport final {
    std::uint64_t seed{};
    std::string profile;
    std::uint64_t operations{};
    std::string expected_sha256;
    std::string actual_sha256;
    bool complete_generations_only{};
    bool mixed_generation{};
    std::uint64_t startup_read_bytes{};
    std::uint64_t extra_committed_bytes{};
    AllocatedProfileMeasurement allocated_profile{};
    std::uint64_t checkpoint_payload_bytes{};
    std::uint64_t journal_growth_bytes{};
    std::uint64_t production_payload_bytes{};
    std::uint64_t production_journal_growth_bytes{};
    std::uint64_t mutation_pause_max_ms{};
    std::uint64_t writer_stall_max_ms{};
    std::uint64_t snapshot_records{};
    std::uint64_t snapshot_cow_peak_bytes{};
    std::uint64_t snapshot_writer_stalls{};
    Coverage coverage{};
    CleanupGauges cleanup{};
    PerformanceMeasurements performance{};
    bool cleanup_succeeded{};
};

enum class ModelKind : std::uint8_t { Data, Zero, Discard };

struct ModelBlock final {
    std::vector<std::byte> bytes{
        static_cast<std::size_t>(kM2BlockSize), std::byte{}};
    ModelKind kind{ModelKind::Zero};
    bool dirty{};
};

using Model = std::map<std::uint64_t, ModelBlock>;

struct JournalMeasurement final {
    std::uint64_t payload_bytes{};
    std::uint64_t growth_bytes{};
    bool cleanup_succeeded{true};
};

void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void require_result(const Result<void>& result, std::string_view message) {
    if (!result.ok()) fail(message);
}

void observe_cleanup_gauges(
    CleanupGauges& aggregate, const DynamicBlockStoreStats& stats) noexcept {
    aggregate.loading_blocks =
        (std::max)(aggregate.loading_blocks, stats.loading_blocks);
    aggregate.active_snapshot_id =
        (std::max)(aggregate.active_snapshot_id, stats.snapshot_active_id);
    aggregate.snapshot_cow_current_bytes = (std::max)(
        aggregate.snapshot_cow_current_bytes, stats.snapshot_cow_current_bytes);
    aggregate.snapshot_cow_records =
        (std::max)(aggregate.snapshot_cow_records, stats.snapshot_cow_records);
    aggregate.snapshot_blocked_writers = (std::max)(
        aggregate.snapshot_blocked_writers, stats.snapshot_blocked_writers);
}

std::string narrow_diagnostic(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 0 && character <= 0x7f
            ? static_cast<char>(character) : '?');
    }
    return result;
}

OperationContext bounded_context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::minutes(15)};
}

std::uint64_t process_read_bytes() {
    IO_COUNTERS counters{};
    require(GetProcessIoCounters(GetCurrentProcess(), &counters) != FALSE,
            "GetProcessIoCounters failed");
    return counters.ReadTransferCount;
}

std::uint64_t process_private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters)) != FALSE,
            "GetProcessMemoryInfo failed");
    return counters.PrivateUsage;
}

class BundleSandbox final {
public:
    explicit BundleSandbox(std::uint64_t seed) {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-soak-bundle-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(seed) + L"-" + std::to_wstring(nonce));
        std::error_code error;
        require(std::filesystem::create_directory(root_, error) && !error,
                "bundle sandbox creation failed");
    }

    ~BundleSandbox() noexcept {
        if (!cleaned_) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    bool cleanup() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        cleaned_ = !error && !std::filesystem::exists(root_, error);
        return cleaned_ && !error;
    }

private:
    std::filesystem::path root_{};
    bool cleaned_{};
};

class JournalSandbox final {
public:
    explicit JournalSandbox(std::uint64_t seed) {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-soak-journal-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(seed) + L"-" + std::to_wstring(nonce));
        std::error_code error;
        require(std::filesystem::create_directory(root_, error) && !error,
                "journal sandbox creation failed");
    }

    ~JournalSandbox() noexcept {
        if (!cleaned_) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    bool cleanup() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        cleaned_ = !error && !std::filesystem::exists(root_, error);
        return cleaned_ && !error;
    }

private:
    std::filesystem::path root_{};
    bool cleaned_{};
};

class DeterministicRandom final {
public:
    explicit DeterministicRandom(std::uint64_t state)
        : state_(state == 0 ? 1 : state) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

private:
    std::uint64_t state_{};
};

std::vector<std::byte> pattern(
    std::uint64_t seed, std::uint64_t operation, std::size_t size) {
    std::vector<std::byte> result(size);
    std::uint64_t state = seed ^ (operation + 1) * 0x9e3779b97f4a7c15ULL;
    for (auto& value : result) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::byte>(state & 0xffU);
    }
    return result;
}

std::vector<std::uint64_t> model_blocks(std::uint64_t total_blocks) {
    std::vector<std::uint64_t> result{
        0, 1, 2, 3, total_blocks / 7, total_blocks / 5,
        total_blocks / 3, total_blocks / 2, total_blocks - 1,
        total_blocks - 2, total_blocks / 11, total_blocks / 13,
        total_blocks / 17, total_blocks / 19, total_blocks / 23,
        total_blocks / 29, total_blocks / 31, total_blocks / 37,
        total_blocks / 41, total_blocks / 43};
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

PersistenceLimits persistence_limits() {
    return {
        kLogicalBytes,
        512ULL * kMiB,
        8192,
        128ULL * kIndexNodeBytes,
        128,
        128ULL * kVhdxBatPageBytes,
        128,
        4ULL * kMiB,
        4ULL * kMiB,
    };
}

DynamicBlockStoreOptions store_options() {
    return {
        32ULL * kMiB,
        64ULL * kMiB,
        16ULL * kMiB,
        kM2BlockSize,
        1,
        256,
        4ULL * kMiB,
        std::chrono::milliseconds{250},
    };
}

void create_bundle(const BundleSandbox& bundle, std::uint64_t seed) {
    GenerationZeroCreateRequest request{};
    request.bundle_root = bundle.root();
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {kLogicalBytes, 512, 4096};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto byte = static_cast<std::byte>(
            ((seed >> ((index % 8U) * 8U)) + index + 1U) & 0xffU);
        request.image_uuid.bytes[index] = byte;
        request.base_identity.file_identity.file_write_guid[index] =
            static_cast<std::byte>(0x20U + index);
        request.base_identity.file_identity.data_write_guid[index] =
            static_cast<std::byte>(0x40U + index);
        request.base_identity.virtual_disk_id[index] =
            static_cast<std::byte>(0x60U + index);
        request.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x80U + index);
    }
    request.journal_epoch = 1;
    request.operation_id = seed + 1;
    request.created_time_100ns = seed + 2;
    request.vhdx_limits = {65536, 4096, kVhdxDataBlockBytes};
    request.recovery = {persistence_limits()};
    auto created = create_generation_zero_image(bounded_context(), request);
    require(created.ok(), "100 GiB generation-zero image creation failed");
}

struct TransportProbe final { IBlockDevice* device{}; };

class CapturingTransport final : public IM3ProviderTransport {
public:
    CapturingTransport(IBlockDevice& device, std::shared_ptr<TransportProbe> probe)
        : probe_(std::move(probe)) {
        probe_->device = &device;
    }

    Result<void> start() override { return Result<void>::success(); }
    Result<void> stop() override { return Result<void>::success(); }
    proxy::SessionSnapshot snapshot() const override { return {}; }
    proxy::ProxyObjectNames names() const override { return {}; }

private:
    std::shared_ptr<TransportProbe> probe_;
};

M3ProviderFactories factories(
    const std::shared_ptr<TransportProbe>& probe,
    std::function<Result<void>(const JournalWriterPhaseEvent&)> phase_hook = {}) {
    M3ProviderFactories result{};
    result.transport = [probe](IBlockDevice& device,
                               const proxy::IProxyNamespacePolicy&,
                               proxy::ShmProxySessionOptions) {
        std::unique_ptr<IM3ProviderTransport> transport =
            std::make_unique<CapturingTransport>(device, probe);
        return Result<std::unique_ptr<IM3ProviderTransport>>::success(
            std::move(transport));
    };
#if defined(MYRD_ENABLE_TEST_SEAMS)
    result.journal_phase_hook = std::move(phase_hook);
#else
    (void)phase_hook;
#endif
    return result;
}

M3ProviderConfig provider_config(const BundleSandbox& bundle) {
    return {bundle.root(), "disk.rdm", {persistence_limits()},
            store_options(), std::chrono::minutes{15}};
}

std::unique_ptr<M3Provider> open_provider(
    const BundleSandbox& bundle,
    const std::shared_ptr<TransportProbe>& probe,
    M3ProviderFactories configured) {
    proxy::LocalNamespacePolicy policy;
    auto opened = M3Provider::open_for_test(
        bounded_context(), policy, provider_config(bundle),
        std::move(configured));
    require(opened.ok(), "M3Provider open failed");
    require(opened.value()->start(bounded_context()).ok(),
            "M3Provider start failed");
    require(probe->device != nullptr, "capturing transport missed block device");
    return opened.move_value();
}

bool stop_twice(M3Provider& provider) {
    const auto first = provider.stop(bounded_context(), {});
    require(first.ok(), "M3Provider first stop failed");
    const auto second = provider.stop(bounded_context(), {});
    require(second.ok(), "M3Provider repeated stop failed");
    return first.ok() && second.ok();
}

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
        .count());
}

std::uint64_t elapsed_us_nonzero(
    std::chrono::steady_clock::time_point started) {
    const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
        .count());
    return (std::max)(elapsed, 1ULL);
}

std::uint64_t elapsed_ms_nonzero(
    std::chrono::steady_clock::time_point started) {
    const auto elapsed_us = elapsed_us_nonzero(started);
    return (elapsed_us + 999ULL) / 1000ULL;
}

void update_peak_committed(
    const DynamicBlockStoreStats& initial,
    const DynamicBlockStoreStats& current,
    std::uint64_t& peak) {
    if (current.committed_bytes >= initial.committed_bytes) {
        peak = (std::max)(peak,
            current.committed_bytes - initial.committed_bytes);
    }
}

void update_peak_process_committed(
    std::uint64_t baseline, std::uint64_t& peak) {
    const auto current = process_private_bytes();
    if (current >= baseline) {
        peak = (std::max)(peak, current - baseline);
    }
}

std::vector<std::byte> read_model_bytes(IBlockDevice& device, const Model& model) {
    std::vector<std::byte> result;
    result.reserve(model.size() * static_cast<std::size_t>(kM2BlockSize));
    for (const auto& [block, expected] : model) {
        std::vector<std::byte> actual(static_cast<std::size_t>(kM2BlockSize));
        require_result(device.read(
            bounded_context(), block * kM2BlockSize, actual),
            "production model readback failed");
        require(actual == expected.bytes,
                "production readback disagrees with complete-generation model");
        result.insert(result.end(), actual.begin(), actual.end());
    }
    return result;
}

std::vector<std::byte> model_bytes(const Model& model) {
    std::vector<std::byte> result;
    result.reserve(model.size() * static_cast<std::size_t>(kM2BlockSize));
    for (const auto& [block, value] : model) {
        (void)block;
        result.insert(result.end(), value.bytes.begin(), value.bytes.end());
    }
    return result;
}

std::uint64_t data_payload_bytes(const Model& model) {
    std::uint64_t result{};
    for (const auto& [block, value] : model) {
        (void)block;
        if (value.dirty && value.kind == ModelKind::Data) {
            result += kM2BlockSize;
        }
    }
    return result;
}

std::uint64_t dirty_record_count(const Model& model) {
    return static_cast<std::uint64_t>(std::count_if(
        model.begin(), model.end(),
        [](const auto& item) { return item.second.dirty; }));
}

void concurrent_first_touch(
    IBlockDevice& device,
    const std::vector<std::uint64_t>& blocks,
    Model& model,
    std::uint64_t seed,
    std::uint64_t& operations,
    std::uint64_t& mutation_pause_max_ms) {
    constexpr std::size_t kWorkers = 4;
    std::mutex error_mutex;
    std::string error_message;
    std::vector<std::vector<std::byte>> writes;
    writes.reserve(kWorkers);
    for (std::size_t index = 0; index < kWorkers; ++index) {
        writes.push_back(pattern(seed, 1000 + index,
                                 static_cast<std::size_t>(kM2BlockSize)));
    }
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&, index] {
            std::vector<std::byte> zero(static_cast<std::size_t>(kM2BlockSize));
            const auto offset = blocks[index] * kM2BlockSize;
            auto read = device.read(bounded_context(), offset, zero);
            if (!read.ok() || !std::all_of(
                    zero.begin(), zero.end(),
                    [](std::byte value) { return value == std::byte{}; })) {
                std::scoped_lock lock(error_mutex);
                error_message = "concurrent first-touch read failed";
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            auto write = device.write(bounded_context(), offset, writes[index]);
            const auto duration = elapsed_ms(started);
            {
                std::scoped_lock lock(error_mutex);
                mutation_pause_max_ms = (std::max)(
                    mutation_pause_max_ms, duration);
                if (!write.ok()) {
                    error_message = "concurrent first-touch write failed";
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(error_message.empty(), error_message);
    for (std::size_t index = 0; index < kWorkers; ++index) {
        auto& target = model.at(blocks[index]);
        target.bytes = std::move(writes[index]);
        target.kind = ModelKind::Data;
        target.dirty = true;
    }
    operations += 2 * kWorkers;
}

void random_workload(
    IBlockDevice& device,
    const std::vector<std::uint64_t>& blocks,
    Model& model,
    std::uint64_t seed,
    std::uint64_t limit,
    std::uint64_t& operations,
    std::uint64_t& mutation_pause_max_ms,
    std::vector<std::uint64_t>& latency_samples_us) {
    DeterministicRandom random(seed);
    for (std::uint64_t operation = 0; operation < limit; ++operation) {
        const auto block = blocks[random.next() % blocks.size()];
        const auto kind = random.next() % 4U;
        auto& expected = model.at(block);
        const auto started = std::chrono::steady_clock::now();
        if (kind == 0) {
            const auto data = pattern(seed, operation, kWriteBytes);
            const auto local_offset = static_cast<std::uint64_t>(
                (random.next() % (kM2BlockSize - kWriteBytes)) / 512U) * 512U;
            require_result(device.write(
                bounded_context(), block * kM2BlockSize + local_offset, data),
                "production random write failed");
            std::copy(data.begin(), data.end(),
                      expected.bytes.begin() + static_cast<std::ptrdiff_t>(
                          local_offset));
            expected.kind = ModelKind::Data;
            expected.dirty = true;
        } else if (kind == 1) {
            const auto local_offset = static_cast<std::uint64_t>(
                (random.next() % (kM2BlockSize - kWriteBytes)) / 512U) * 512U;
            std::vector<std::byte> actual(kWriteBytes);
            require_result(device.read(
                bounded_context(), block * kM2BlockSize + local_offset, actual),
                "production random read failed");
            require(std::equal(
                actual.begin(), actual.end(),
                expected.bytes.begin() + static_cast<std::ptrdiff_t>(
                    local_offset)), "random read disagrees with model");
        } else {
            const DataRange range{block * kM2BlockSize, kM2BlockSize};
            if (kind == 2) {
                require_result(device.zero(bounded_context(), {&range, 1}),
                               "production random zero failed");
                expected.kind = ModelKind::Zero;
            } else {
                require_result(device.unmap(bounded_context(), {&range, 1}),
                               "production random unmap failed");
                expected.kind = ModelKind::Discard;
            }
            std::fill(expected.bytes.begin(), expected.bytes.end(), std::byte{});
            expected.dirty = true;
        }
        latency_samples_us.push_back(elapsed_us_nonzero(started));
        mutation_pause_max_ms = (std::max)(
            mutation_pause_max_ms, elapsed_ms(started));
        ++operations;
    }
}

JournalHeader make_journal_header(std::uint64_t seed) {
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] = static_cast<std::byte>(
            ((seed >> ((index % 8U) * 8U)) + index + 1U) & 0xffU);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x80U + index);
    }
    header.epoch = 1;
    header.geometry = {kLogicalBytes, 512, 4096};
    header.base_checkpoint_generation = 0;
    header.predecessor_kind = JournalPredecessorKind::Genesis;
    const auto geometry_digest = compute_geometry_digest(header.geometry);
    require(geometry_digest.ok(), "journal geometry digest failed");
    header.geometry_digest = geometry_digest.value();
    VhdxFileIdentity base{};
    base.file_write_guid[0] = std::byte{0x31};
    base.data_write_guid[0] = std::byte{0x41};
    const auto base_digest = compute_base_identity_digest(base);
    require(base_digest.ok(), "journal base digest failed");
    header.base_identity_digest = base_digest.value();
    const auto predecessor = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest);
    require(predecessor.ok(), "journal predecessor digest failed");
    header.predecessor_digest = predecessor.value();
    header.feature_flags = 1;
    return header;
}

std::uint64_t formal_seed_index(std::uint64_t seed) {
    if (seed < kFormalSeedBase ||
        (seed - kFormalSeedBase) % kFormalSeedStride != 0) {
        fail("Formal seed is outside the frozen set");
    }
    const auto index = (seed - kFormalSeedBase) / kFormalSeedStride;
    require(index < kFormalSeedCount, "Formal seed index is out of range");
    return index;
}

class AllocatedProfilePayloadSource final : public IVhdxPayloadSource {
public:
    Result<void> read_payload(
        const OperationContext&,
        const std::uint64_t payload_index,
        const std::span<std::byte> destination) const override {
        if (payload_index >= kFormalAllocatedProfilePayloads ||
            destination.size() != kVhdxDataBlockBytes) {
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::InvalidFormat),
                L"m3_soak_allocated_profile",
                L"allocated profile payload request is out of range"});
        }
        const auto value = static_cast<std::byte>(
            static_cast<std::uint8_t>(payload_index % 251ULL + 1ULL));
        std::fill(destination.begin(), destination.end(), value);
        return Result<void>::success();
    }
};

AllocatedProfileMeasurement verify_formal_allocated_profile(
    const std::uint64_t seed) {
    require(seed == kFormalAllocatedProfileSeed,
            "allocated profile ran outside its frozen Formal seed");
    BundleSandbox sandbox(seed ^ 0xA110CA7EULL);
    const auto relative_name = std::string{"allocated-profile.vhdx"};
    const auto path = sandbox.root() / L"allocated-profile.vhdx";
    const auto context = bounded_context();

    VhdxWriterIdentity identity{};
    for (std::size_t index = 0; index < 16; ++index) {
        identity.file_identity.file_write_guid[index] =
            static_cast<std::byte>(0x11U + index);
        identity.file_identity.data_write_guid[index] =
            static_cast<std::byte>(0x31U + index);
        identity.virtual_disk_id[index] =
            static_cast<std::byte>(0x51U + index);
    }
    std::vector<std::uint64_t> candidates(
        static_cast<std::size_t>(kFormalAllocatedProfilePayloads));
    std::iota(candidates.begin(), candidates.end(), std::uint64_t{0});
    AllocatedProfilePayloadSource source;
    auto plan = plan_sparse_vhdx(
        context, {kLogicalBytes, 512}, identity, candidates, source,
        {kFormalAllocatedProfilePayloads,
         kFormalAllocatedProfilePayloads, kVhdxDataBlockBytes});
    require(plan.ok(), "allocated profile VHDX planning failed");
    require(plan.value().virtual_size() == kLogicalBytes &&
            plan.value().payloads().size() ==
                kFormalAllocatedProfilePayloads,
            "allocated profile VHDX plan is not 100 GiB/1 GiB");

    auto created = open_windows_persistence_file(
        path, FileOpenMode::CreateNewReadWrite);
    require(created.ok(), "allocated profile VHDX create failed");
    auto written = write_sparse_vhdx_file(
        context, plan.value(), source, created.move_value());
    require(written.ok(), "allocated profile VHDX write failed");
    const auto expected_file_length = written.value().layout().file_length();
    auto created_file = written.value().release_file();
    require_result(created_file->close(),
                   "allocated profile VHDX close failed");
    created_file.reset();

    AllocatedProfileMeasurement measurement{};
    measurement.allocated_bytes =
        kFormalAllocatedProfilePayloads * kVhdxDataBlockBytes;
    {
        const auto read_before = process_read_bytes();
        const auto private_before = process_private_bytes();
        auto capability = open_bundle_file_for_backing_view(
            sandbox.root(), relative_name);
        require(capability.ok(),
                "allocated profile backing-view open failed");
        auto opened = capability.move_value();
        auto delete_guard = opened.take_delete_guard();
        auto validated = open_validated_persistence_file(opened.take_file());
        require(validated.ok(),
                "allocated profile validated-file open failed");
        auto owned_file = validated.move_value();
        VhdxParseEvidence evidence{};
        const auto max_payloads =
            kLogicalBytes / kVhdxDataBlockBytes + 1ULL;
        auto reader_result = VhdxReader::open(
            context, owned_file,
            {{4ULL * kMiB, 128}, max_payloads, max_payloads},
            {identity.file_identity, expected_file_length}, &evidence);
        require(reader_result.ok(),
                "allocated profile production VHDX reader open failed");
        const auto read_after = process_read_bytes();
        const auto private_after = process_private_bytes();
        measurement.startup_read_bytes = read_after >= read_before
            ? read_after - read_before : 0;
        measurement.extra_committed_bytes = private_after >= private_before
            ? private_after - private_before : 0;
        measurement.structural_read_bytes = evidence.structural_read_bytes;

        auto reader = reader_result.move_value();
        require(reader->layout().virtual_size() == kLogicalBytes &&
                reader->stats().payload_read_calls == 0,
                "allocated profile eagerly read payload data at startup");
        require(measurement.startup_read_bytes <= 64ULL * kMiB &&
                measurement.extra_committed_bytes <= 128ULL * kMiB &&
                evidence.stage == VhdxParseStage::Complete &&
                evidence.structural_read_bytes > 0,
                "allocated profile startup resource limit failed");

        std::array<std::byte, kM2BlockSize> block{};
        const auto loaded = reader->read_block(context, 0, block);
        require(loaded.ok() && loaded.value().kind == BlockLoadKind::Data &&
                std::all_of(block.begin(), block.end(), [](const std::byte value) {
                    return value == std::byte{1};
                }) && reader->stats().payload_read_calls == 1,
                "allocated profile first-touch payload verification failed");
        measurement.verified = true;
    }
    measurement.cleanup_succeeded = sandbox.cleanup();
    require(measurement.cleanup_succeeded,
            "allocated profile sandbox cleanup failed");
    return measurement;
}

JournalMeasurement write_formal_resource_checkpoint(std::uint64_t seed) {
    JournalSandbox sandbox(seed);
    const auto journal_path = sandbox.root() / L"checkpoint.rdj";
    auto opened = open_windows_persistence_file(
        journal_path, FileOpenMode::CreateNewReadWrite);
    require(opened.ok(), "native resource journal create failed");
    auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
    const auto header = make_journal_header(seed);
    const auto header_bytes = encode_journal_header(header);
    require(header_bytes.ok(), "resource journal header encoding failed");
    require_result(file->write_exact(
        bounded_context(), 0, header_bytes.value()),
        "resource journal header write failed");

    JournalCheckpointRequest request{};
    request.file = file;
    request.file_identity.volume_serial_number = 1;
    request.file_identity.file_id[0] = std::byte{0x51};
    request.file_identity.attributes = 1;
    request.file_identity.hard_link_count = 1;
    request.header = header;
    request.previous_generation = 0;
    request.previous_digest = header.predecessor_digest;
    request.committed_length = kJournalAlignment;
    request.operation_id = seed + 1;

    JournalWriterOptions options{};
    options.index_builder = {
        {128 * kIndexNodeBytes, 128}, 8192,
        8192 * static_cast<std::uint64_t>(kIndexNodeBytes)};
    options.max_checkpoint_records = 8192;
    options.max_checkpoint_bytes =
        (kFormalPayloadBlocks * kM2BlockSize * 120ULL / 100ULL /
         kJournalAlignment) * kJournalAlignment;
    auto writer = JournalWriter::create(options);
    require(writer.ok(), "resource journal writer creation failed");

    const auto prepared = writer.value()->prepare_streaming(
        bounded_context(), request,
        [seed](const OperationContext&,
               const JournalCheckpointBatchConsumer& consume) -> Result<void> {
            for (std::uint64_t block = 0;
                 block < kFormalPayloadBlocks; ++block) {
                const auto payload = pattern(
                    seed, block, static_cast<std::size_t>(kM2BlockSize));
                const JournalCheckpointBlockInput input{
                    JournalRecordType::Data, block, block + 1, 0, 1, payload};
                const auto consumed = consume({&input, 1});
                if (!consumed.ok()) return consumed;
            }
            return Result<void>::success();
        });
    if (!prepared.ok()) {
        fail("single 256 MiB journal checkpoint failed: code=" +
             std::to_string(prepared.error().code) + ", native=" +
             std::to_string(prepared.error().native_code) + ", operation=" +
             narrow_diagnostic(prepared.error().operation) + ", message=" +
             narrow_diagnostic(prepared.error().message));
    }
    require(prepared.value().data_count == kFormalPayloadBlocks &&
            prepared.value().generation == 1,
            "single 256 MiB checkpoint counters are invalid");
    const auto payload_bytes = kFormalPayloadBlocks * kM2BlockSize;
    const auto growth_bytes =
        prepared.value().committed_length - kJournalAlignment;
    require(growth_bytes > payload_bytes &&
            growth_bytes <= payload_bytes * 120ULL / 100ULL,
            "single 256 MiB journal write amplification exceeded 20 percent");
    require_result(file->close(), "resource journal close failed");
    file.reset();
    writer.value().reset();
    const bool cleaned = sandbox.cleanup();
    require(cleaned, "resource journal cleanup failed");
    return {payload_bytes, growth_bytes, cleaned};
}

bool is_compaction_seed(std::uint64_t seed, std::string_view profile) {
    return profile == "Formal" ? formal_seed_index(seed) == 0 : seed == 42;
}

bool is_io_fault_seed(std::uint64_t seed, std::string_view profile) {
    return profile == "Formal" ? formal_seed_index(seed) == 1 : seed == 9001;
}

M3CliConfig compaction_config(const BundleSandbox& bundle) {
    M3CliConfig config{};
    config.command = M3CliCommand::Compact;
    config.bundle_root = bundle.root();
    config.manifest_name = "disk.rdm";
    config.persistence_limits = persistence_limits();
    config.store_options = store_options();
    config.timeout = std::chrono::minutes{15};
    config.barrier = VolumeBarrierKind::RawBlockTestNoFilesystemCache;
    return config;
}

SoakSeedReport run_soak_seed(
    std::uint64_t seed, std::string_view profile) {
    const auto run_started = std::chrono::steady_clock::now();
    const auto random_operation_limit = profile == "Formal" ? 2048ULL : 512ULL;
    const bool compaction_scenario = is_compaction_seed(seed, profile);
    const bool io_fault_scenario = is_io_fault_seed(seed, profile);
    BundleSandbox bundle(seed);
    create_bundle(bundle, seed);

    const auto blocks = model_blocks(kLogicalBytes / kM2BlockSize);
    Model model;
    for (const auto block : blocks) model.emplace(block, ModelBlock{});

    auto probe = std::make_shared<TransportProbe>();
    std::atomic_bool inject_io{io_fault_scenario};
    std::mutex cow_mutex;
    std::condition_variable cow_cv;
    bool allow_cow{};
    bool cow_finished{};
    bool cow_triggered{};
    bool cow_write_ok{};
    std::uint64_t cow_write_ms{};
    CleanupGauges cleanup_gauges{};
    std::vector<std::uint64_t> latency_samples_us;
    latency_samples_us.reserve(static_cast<std::size_t>(
        profile == "Formal" ? 2048ULL : 512ULL));
    std::uint64_t checkpoint_duration_ms{};
    std::uint64_t compaction_duration_ms{};
    const auto cow_block = blocks.back();
    const auto cow_data = pattern(
        seed, 0xC0FFEEULL, static_cast<std::size_t>(kM2BlockSize));

    auto hook = [&](const JournalWriterPhaseEvent& event) -> Result<void> {
        if (event.phase != JournalWriterPhase::BeforeBlockWrite ||
            event.ordinal != 0) {
            return Result<void>::success();
        }
        if (inject_io.exchange(false)) {
            return Result<void>::failure({
                ErrorDomain::persistence, static_cast<std::uint32_t>(
                    PersistenceErrorCode::IoError),
                L"m3_soak", L"injected journal I/O failure",
                ERROR_WRITE_FAULT});
        }
        std::unique_lock lock(cow_mutex);
        if (cow_triggered) return Result<void>::success();
        cow_triggered = true;
        allow_cow = true;
        cow_cv.notify_all();
        cow_cv.wait(lock, [&] { return cow_finished; });
        if (!cow_write_ok) {
            return Result<void>::failure({
                ErrorDomain::persistence, static_cast<std::uint32_t>(
                    PersistenceErrorCode::IoError),
                L"m3_soak", L"concurrent COW mutation failed"});
        }
        return Result<void>::success();
    };

    const auto startup_read_before = process_read_bytes();
    const auto private_bytes_before = process_private_bytes();
    auto provider = open_provider(
        bundle, probe, factories(probe, hook));
    const auto startup_read_after = process_read_bytes();
    const auto initial_stats = provider->storage_stats();
    auto startup_read_bytes = startup_read_after >= startup_read_before
        ? startup_read_after - startup_read_before : 0;
    std::uint64_t peak_committed{};
    std::uint64_t mutation_pause_max_ms{};
    std::uint64_t operations{};

    const auto before_first_touch = provider->storage_stats();
    concurrent_first_touch(
        *probe->device,
        std::vector<std::uint64_t>(blocks.begin(), blocks.begin() + 4),
        model, seed, operations, mutation_pause_max_ms);
    const auto after_first_touch = provider->storage_stats();
    const bool concurrent_first_touch_verified =
        after_first_touch.backing_reads_total >=
            before_first_touch.backing_reads_total + 4;
    require(concurrent_first_touch_verified,
            "concurrent first-touch did not perform lazy backing reads");
    update_peak_process_committed(private_bytes_before, peak_committed);
    random_workload(*probe->device, blocks, model, seed,
                    random_operation_limit, operations,
                    mutation_pause_max_ms, latency_samples_us);

    const auto low_data = pattern(
        seed, 0xA11CEULL, static_cast<std::size_t>(kM2BlockSize));
    require_result(probe->device->write(
        bounded_context(), blocks.front() * kM2BlockSize, low_data),
        "low checkpoint sentinel write failed");
    model.at(blocks.front()).bytes = low_data;
    model.at(blocks.front()).kind = ModelKind::Data;
    model.at(blocks.front()).dirty = true;
    const auto high_data = pattern(
        seed, 0xBEEFULL, static_cast<std::size_t>(kM2BlockSize));
    require_result(probe->device->write(
        bounded_context(), cow_block * kM2BlockSize, high_data),
        "high checkpoint sentinel write failed");
    model.at(cow_block).bytes = high_data;
    model.at(cow_block).kind = ModelKind::Data;
    model.at(cow_block).dirty = true;
    operations += 2;
    update_peak_committed(initial_stats, provider->storage_stats(),
                          peak_committed);
    update_peak_process_committed(private_bytes_before, peak_committed);

    const Model first_cut = model;
    const auto production_payload_bytes = data_payload_bytes(first_cut);
    const auto snapshot_records = dirty_record_count(first_cut);
    const auto journal_path = bundle.root() / L"journal-e0001.rdj";
    const auto journal_before = std::filesystem::file_size(journal_path);

    bool io_fault_observed = false;
    if (io_fault_scenario) {
        const auto failed_checkpoint = provider->checkpoint(
            bounded_context(),
            VolumeBarrierKind::RawBlockTestNoFilesystemCache);
        require(!failed_checkpoint.ok() &&
                failed_checkpoint.disposition ==
                    CommitDisposition::NotCommitted &&
                failed_checkpoint.error_context.has_value() &&
                failed_checkpoint.error_context->code ==
                    PersistenceErrorCode::IoError,
                "injected product I/O failure did not fail closed");
        io_fault_observed = true;
    }

    std::thread cow_writer([&] {
        std::unique_lock lock(cow_mutex);
        cow_cv.wait(lock, [&] { return allow_cow; });
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        const auto wrote = probe->device->write(
            bounded_context(), cow_block * kM2BlockSize, cow_data);
        const auto duration = elapsed_ms(started);
        lock.lock();
        cow_write_ms = duration;
        cow_write_ok = wrote.ok();
        cow_finished = true;
        lock.unlock();
        cow_cv.notify_all();
    });
    const auto first_checkpoint_started = std::chrono::steady_clock::now();
    const auto first_checkpoint = provider->checkpoint(
        bounded_context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    checkpoint_duration_ms = (std::max)(
        checkpoint_duration_ms, elapsed_ms_nonzero(first_checkpoint_started));
    if (!cow_triggered) {
        std::scoped_lock lock(cow_mutex);
        allow_cow = true;
        cow_cv.notify_all();
    }
    cow_writer.join();
    const bool first_generation_complete =
        first_checkpoint.ok() && first_checkpoint.generation == 1;
    require(first_generation_complete,
            "production checkpoint did not commit generation one");
    require(cow_triggered && cow_write_ok,
            "production checkpoint did not overlap a COW mutation");
    model.at(cow_block).bytes = cow_data;
    model.at(cow_block).kind = ModelKind::Data;
    model.at(cow_block).dirty = true;
    mutation_pause_max_ms = (std::max)(mutation_pause_max_ms, cow_write_ms);
    ++operations;
    const auto after_first_checkpoint = provider->storage_stats();
    require(after_first_checkpoint.snapshot_cow_peak_bytes >= kM2BlockSize,
            "production checkpoint did not observe a COW pre-image");
    update_peak_committed(initial_stats, after_first_checkpoint,
                          peak_committed);
    update_peak_process_committed(private_bytes_before, peak_committed);
    observe_cleanup_gauges(cleanup_gauges, after_first_checkpoint);
    (void)read_model_bytes(*probe->device, model);
    bool repeated_close_verified = stop_twice(*provider);
    provider.reset();

    const auto journal_after = std::filesystem::file_size(journal_path);
    require(journal_after > journal_before,
            "production checkpoint did not grow the journal");
    const auto production_journal_growth = journal_after - journal_before;

    auto reopened_probe = std::make_shared<TransportProbe>();
    auto reopened = open_provider(
        bundle, reopened_probe, factories(reopened_probe));
    require(reopened->generation() == 1,
            "reopen did not select generation one");
    (void)read_model_bytes(*reopened_probe->device, first_cut);
    observe_cleanup_gauges(cleanup_gauges, reopened->storage_stats());
    bool reopen_verified = reopened->generation() == 1;
    repeated_close_verified =
        stop_twice(*reopened) && repeated_close_verified;
    reopened.reset();

    model = first_cut;
    auto second_probe = std::make_shared<TransportProbe>();
    auto second = open_provider(bundle, second_probe, factories(second_probe));
    const auto final_data = pattern(
        seed, 0xF1A1ULL, static_cast<std::size_t>(kM2BlockSize));
    require_result(second_probe->device->write(
        bounded_context(), cow_block * kM2BlockSize, final_data),
        "final generation write failed");
    model.at(cow_block).bytes = final_data;
    model.at(cow_block).kind = ModelKind::Data;
    model.at(cow_block).dirty = true;
    ++operations;
    const auto second_checkpoint_started = std::chrono::steady_clock::now();
    const auto second_checkpoint = second->checkpoint(
        bounded_context(), VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    checkpoint_duration_ms = (std::max)(
        checkpoint_duration_ms, elapsed_ms_nonzero(second_checkpoint_started));
    const bool second_generation_complete =
        second_checkpoint.ok() && second_checkpoint.generation == 2;
    require(second_generation_complete,
            "production checkpoint did not commit generation two");
    observe_cleanup_gauges(cleanup_gauges, second->storage_stats());
    repeated_close_verified =
        stop_twice(*second) && repeated_close_verified;
    second.reset();

    std::uint64_t expected_generation = 2;
    bool compaction_verified = false;
    if (compaction_scenario) {
        const auto compaction_started = std::chrono::steady_clock::now();
        const auto compacted = run_m3_lifecycle(
            bounded_context(), compaction_config(bundle));
        compaction_duration_ms = elapsed_ms_nonzero(compaction_started);
        require(compacted.exit_code == 0 &&
                compacted.disposition == CommitDisposition::Committed &&
                compacted.generation == 3,
                "production compaction failed");
        expected_generation = 3;
        compaction_verified = true;
        ++operations;
    }

    auto final_probe = std::make_shared<TransportProbe>();
    auto final_provider = open_provider(
        bundle, final_probe, factories(final_probe));
    require(final_provider->generation() == expected_generation,
            "final reopen selected an incomplete generation");
    const auto actual_bytes = read_model_bytes(*final_probe->device, model);
    observe_cleanup_gauges(cleanup_gauges, final_provider->storage_stats());
    const auto expected_bytes = model_bytes(model);
    const auto expected_hash = sha256_hex(expected_bytes);
    const auto actual_hash = sha256_hex(actual_bytes);
    require(expected_hash.ok() && actual_hash.ok() &&
            expected_hash.value() == actual_hash.value(),
            "final complete-generation hash mismatch");
    const bool final_generation_complete =
        final_provider->generation() == expected_generation;
    repeated_close_verified =
        stop_twice(*final_provider) && repeated_close_verified;
    final_provider.reset();

    JournalMeasurement resource_checkpoint{};
    AllocatedProfileMeasurement allocated_profile{};
    if (profile == "Formal" && formal_seed_index(seed) == 0) {
        resource_checkpoint = write_formal_resource_checkpoint(seed);
        allocated_profile = verify_formal_allocated_profile(seed);
        startup_read_bytes = (std::max)(
            startup_read_bytes, allocated_profile.startup_read_bytes);
        peak_committed = (std::max)(
            peak_committed, allocated_profile.extra_committed_bytes);
    }
    const auto reported_payload = resource_checkpoint.payload_bytes != 0
        ? resource_checkpoint.payload_bytes : production_payload_bytes;
    const auto reported_growth = resource_checkpoint.growth_bytes != 0
        ? resource_checkpoint.growth_bytes : production_journal_growth;
    const bool cleaned = bundle.cleanup() &&
        resource_checkpoint.cleanup_succeeded &&
        allocated_profile.cleanup_succeeded;
    require(cleanup_gauges.empty(),
            "production soak left active store work after a stable boundary");
    require(cleaned, "production soak bundle cleanup failed");
    require(!latency_samples_us.empty(),
            "production soak collected no latency observations");

    return {
        seed,
        std::string(profile),
        operations,
        expected_hash.value(),
        actual_hash.value(),
        first_generation_complete && second_generation_complete &&
            final_generation_complete,
        expected_hash.value() != actual_hash.value(),
        startup_read_bytes,
        peak_committed,
        allocated_profile,
        reported_payload,
        reported_growth,
        production_payload_bytes,
        production_journal_growth,
        mutation_pause_max_ms,
        cow_write_ms,
        snapshot_records,
        after_first_checkpoint.snapshot_cow_peak_bytes,
        after_first_checkpoint.snapshot_writer_stalls_total,
        Coverage{first_generation_complete && second_generation_complete,
                 reopen_verified && final_generation_complete,
                 compaction_verified, concurrent_first_touch_verified,
                 cow_triggered &&
                     after_first_checkpoint.snapshot_cow_peak_bytes >=
                         kM2BlockSize,
                 io_fault_scenario && io_fault_observed &&
                     first_generation_complete,
                 repeated_close_verified},
        cleanup_gauges,
        PerformanceMeasurements{
            elapsed_ms_nonzero(run_started), std::move(latency_samples_us),
            checkpoint_duration_ms, compaction_duration_ms},
        cleaned,
    };
}

std::uint64_t parse_seed(const char* text) {
    if (text == nullptr || *text == '\0') {
        fail("MYRD_M3_SOAK_SEED is required");
    }
    std::uint64_t value{};
    const auto end = text + std::char_traits<char>::length(text);
    const auto parsed = std::from_chars(text, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        fail("MYRD_M3_SOAK_SEED must be unsigned decimal");
    }
    return value;
}

std::string environment_value(const char* name) {
    char* value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value, value_size == 0 ? 0 : value_size - 1);
    std::free(value);
    return result;
}

void print_summary(const SoakSeedReport& report) {
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    std::cout << "{\"schema\":\"myrd.m3.soak-seed.v2\",\"seed\":"
              << report.seed << ",\"profile\":\"" << report.profile
              << "\",\"operation_count\":" << report.operations
              << ",\"expected_sha256\":\"" << report.expected_sha256
              << "\",\"actual_sha256\":\"" << report.actual_sha256
              << "\",\"complete_generations_only\":"
              << boolean(report.complete_generations_only)
              << ",\"mixed_generation\":" << boolean(report.mixed_generation)
              << ",\"resource\":{\"logical_bytes\":" << kLogicalBytes
              << ",\"startup_read_bytes\":" << report.startup_read_bytes
              << ",\"extra_committed_bytes\":"
              << report.extra_committed_bytes
              << ",\"allocated_profile_bytes\":"
              << report.allocated_profile.allocated_bytes
              << ",\"allocated_profile_startup_read_bytes\":"
              << report.allocated_profile.startup_read_bytes
              << ",\"allocated_profile_extra_committed_bytes\":"
              << report.allocated_profile.extra_committed_bytes
              << ",\"allocated_profile_structural_read_bytes\":"
              << report.allocated_profile.structural_read_bytes
              << ",\"allocated_profile_verified\":"
              << boolean(report.allocated_profile.verified) << "}"
              << ",\"checkpoint\":{\"payload_bytes\":"
              << report.checkpoint_payload_bytes
              << ",\"journal_growth_bytes\":"
              << report.journal_growth_bytes
              << ",\"production_payload_bytes\":"
              << report.production_payload_bytes
              << ",\"production_journal_growth_bytes\":"
              << report.production_journal_growth_bytes << "}"
              << ",\"latency\":{\"mutation_pause_max_ms\":"
              << report.mutation_pause_max_ms
              << ",\"writer_stall_max_ms\":"
              << report.writer_stall_max_ms << "}"
              << ",\"snapshot\":{\"records\":"
              << report.snapshot_records << ",\"cow_peak_bytes\":"
              << report.snapshot_cow_peak_bytes << ",\"writer_stalls\":"
              << report.snapshot_writer_stalls << "}"
              << ",\"coverage\":{\"production_checkpoint\":"
              << boolean(report.coverage.production_checkpoint)
              << ",\"reopen_verified\":"
              << boolean(report.coverage.reopen_verified)
              << ",\"compaction_verified\":"
              << boolean(report.coverage.compaction_verified)
              << ",\"concurrent_first_touch\":"
              << boolean(report.coverage.concurrent_first_touch)
              << ",\"cow_overlap\":"
              << boolean(report.coverage.cow_overlap)
              << ",\"io_fault_recovered\":"
              << boolean(report.coverage.io_fault_recovered)
              << ",\"repeated_close\":"
              << boolean(report.coverage.repeated_close) << "}"
              << ",\"performance\":{\"duration_ms\":"
              << report.performance.duration_ms
              << ",\"checkpoint_duration_ms\":"
              << report.performance.checkpoint_duration_ms
              << ",\"compaction_duration_ms\":"
              << report.performance.compaction_duration_ms
              << ",\"latency_samples_us\":[";
    for (std::size_t index = 0;
         index < report.performance.latency_samples_us.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << report.performance.latency_samples_us[index];
    }
    std::cout << "]}"
              << ",\"cleanup\":{\"loading_blocks\":"
              << report.cleanup.loading_blocks
              << ",\"active_snapshot_id\":"
              << report.cleanup.active_snapshot_id
              << ",\"snapshot_cow_current_bytes\":"
              << report.cleanup.snapshot_cow_current_bytes
              << ",\"snapshot_cow_records\":"
              << report.cleanup.snapshot_cow_records
              << ",\"snapshot_blocked_writers\":"
              << report.cleanup.snapshot_blocked_writers << "}"
              << ",\"cleanup_succeeded\":"
              << boolean(report.cleanup_succeeded) << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--self-test")) {
        std::cerr << "M3_SOAK_ERROR unsupported command line\n";
        return 1;
    }
    try {
        const auto test_mode = environment_value("MYRD_M3_SOAK_TEST_MODE");
        if (test_mode == "nonzero") {
            std::cerr << "M3_SOAK_TEST_MODE nonzero\n";
            return 7;
        }
        if (test_mode == "malformed") {
            std::cout << "not-json\n";
            return 0;
        }
        if (test_mode == "multiple") {
            std::cout << "{\"schema\":\"first\"}\n"
                         "{\"schema\":\"second\"}\n";
            return 0;
        }
        if (test_mode == "timeout") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
        if (argc == 2) {
            const auto first = run_soak_seed(12648430, "Quick");
            const auto second = run_soak_seed(12648430, "Quick");
            require(first.operations > 0, "self-test ran no operations");
            require(first.expected_sha256 == first.actual_sha256,
                    "self-test model hash mismatch");
            require(first.coverage.production_checkpoint &&
                    first.coverage.reopen_verified &&
                    first.coverage.concurrent_first_touch &&
                    first.coverage.cow_overlap &&
                    first.coverage.repeated_close,
                    "self-test missed production coverage");
            require(first.operations == second.operations &&
                    first.expected_sha256 == second.expected_sha256 &&
                    first.actual_sha256 == second.actual_sha256,
                    "same seed was not deterministic");
            print_summary(first);
            return 0;
        }
        const auto profile = environment_value("MYRD_M3_SOAK_PROFILE");
        if (profile != "Quick" && profile != "Formal") {
            fail("MYRD_M3_SOAK_PROFILE must be Quick or Formal");
        }
        const auto seed_text = environment_value("MYRD_M3_SOAK_SEED");
        print_summary(run_soak_seed(parse_seed(seed_text.c_str()), profile));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "M3_SOAK_ERROR " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "M3_SOAK_ERROR unknown failure\n";
        return 1;
    }
}
