#include "m2_local_fake.h"

#include "m2_lifecycle_runner.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace myrd {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kLocalFakeDiskSize = 64ull << 30;
constexpr aim::DeviceId kLocalFakeDevice{197121u};

Error local_fake_error(std::wstring operation, std::wstring message) {
    return {
        ErrorDomain::evidence,
        static_cast<std::uint32_t>(
            OperationErrorCode::internal_invariant),
        std::move(operation),
        std::move(message)};
}

template <typename T>
Result<T> context_failure(const Result<void>& checked) {
    return Result<T>::failure(checked.error());
}

Result<void> require_state(
    bool condition,
    std::wstring_view operation,
    std::wstring_view message) {
    if (condition) return Result<void>::success();
    return Result<void>::failure(local_fake_error(
        std::wstring(operation), std::wstring(message)));
}

struct LocalFakeState {
    bool baseline_captured{};
    bool provider_constructed{};
    bool provider_started{};
    bool write_completed{};
    bool retrim_completed{};
    bool performance_completed{};
    bool device_created{};
    bool disk_confirmed{};
    bool mounts_owned{};
    bool provider_stopped{};
    bool service_restored{};
};

M2InventorySnapshot baseline_inventory() {
    M2InventorySnapshot snapshot;
    snapshot.physical_disks = {"0", "1"};
    snapshot.volumes = {R"(\\?\Volume{system}\)"};
    snapshot.mount_points = {R"(C:\)"};
    snapshot.service_state = "running";
    snapshot.service_start_type = "automatic";
    snapshot.process_handle_count = 100;
    snapshot.process_thread_count = 10;
    snapshot.process_private_bytes = 64ull << 20;
    return snapshot;
}

M2DriverServiceState driver_state() {
    return {
        "loaded",
        "phdskmnt",
        "running",
        "automatic",
        R"(C:\Windows\System32\drivers\phdskmnt.sys)",
        "1.2.28.93",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
}

M2PerformanceProbeResult performance_sample(
    std::uint64_t sequential_rate,
    std::uint64_t random_rate) {
    M2PerformanceProbeResult sample;
    sample.operation = M2RawOperation::Write;
    sample.region_offset = 64ull << 20;
    sample.policy = {
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        "single-thread-normal-priority",
        "FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH"};
    const auto frequency = sequential_rate * random_rate;
    sample.sequential = {
        kM2SequentialRegionBytes,
        kM2SequentialOperationCount,
        kM2SequentialRegionBytes * random_rate,
        frequency,
        sequential_rate};
    sample.random = {
        kM2RandomIoBytes * kM2RandomOperationCount,
        kM2RandomOperationCount,
        kM2RandomOperationCount * sequential_rate,
        frequency,
        random_rate};
    return sample;
}

M2PerformanceEvidence passing_performance() {
    M2PerformanceEvidence evidence;
    evidence.fixed = performance_sample(1'000'000, 10'000);
    evidence.dynamic = performance_sample(700'000, 6'000);
    evidence.comparison.sequential_ratio_basis_points = 7'000;
    evidence.comparison.random_ratio_basis_points = 6'000;
    evidence.comparison.sequential_pass = true;
    evidence.comparison.random_pass = true;
    evidence.comparison.overall_pass = true;
    evidence.gate_status = M2PerformanceGateStatus::passed;
    return evidence;
}

M2Report local_fake_report(std::string_view run_id) {
    M2Report report;
    report.run.run_id = std::string(run_id);
    report.run.mode = M2RunMode::fake;
    report.build_toolchain = {
        "2.0.0",
        "local-fake",
        "LocalFake",
        "MSVC",
        "19.44",
        "4.1",
        "myrd-m2-e2e.exe",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"};
    report.accepted_m1_baseline = {
        "local-fake-accepted-m1.json",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        1,
        "GO",
        true};
    report.machine_os = {
        "local-fake-machine",
        "Windows",
        "11 24H2",
        "x64",
        "26100",
        "Local Fake CPU",
        16,
        32ull << 30,
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    report.geometry_options.disk_size_bytes = kLocalFakeDiskSize;
    report.geometry_options.sector_size_bytes = 512;
    report.geometry_options.block_size_bytes = 64ull << 10;
    report.geometry_options.soft_limit_bytes = 512ull << 20;
    report.geometry_options.hard_limit_bytes = 1ull << 30;
    report.geometry_options.eviction_low_watermark_bytes = 384ull << 20;
    report.geometry_options.max_snapshot_batch_bytes = 64ull << 20;
    report.geometry_options.max_snapshot_batch_records = 1024;
    report.geometry_options.lock_stripe_count = 64;
    report.geometry_options.backing_kind = "fake";
    report.state.initial_state = M2InitialState::empty;
    report.state.initial_backing_generation = 0;
    report.state.current_backing_generation = 0;
    return report;
}

class LocalFakeClock final : public IM2LifecycleClock {
public:
    std::chrono::steady_clock::time_point steady_now() override {
        now_ += 10ms;
        return now_;
    }

    std::string utc_now() override {
        return "2026-08-04T01:02:03Z";
    }

private:
    std::chrono::steady_clock::time_point now_{
        std::chrono::steady_clock::now()};
};

class LocalFakeBaseline final : public IM2BaselineControl {
public:
    explicit LocalFakeBaseline(LocalFakeState& state) : state_(state) {}

    Result<M2BaselineSnapshot> capture(
        const OperationContext& context) override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<M2BaselineSnapshot>(checked);
        auto order = require_state(
            !state_.baseline_captured,
            L"local_fake.capture",
            L"baseline was captured more than once");
        if (!order) return context_failure<M2BaselineSnapshot>(order);
        state_.baseline_captured = true;
        return Result<M2BaselineSnapshot>::success({
            driver_state(),
            baseline_inventory(),
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"});
    }

    Result<void> restore_service(
        const OperationContext& context,
        const M2BaselineSnapshot&) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto order = require_state(
            state_.baseline_captured && !state_.device_created &&
                (!state_.provider_constructed || state_.provider_stopped),
            L"local_fake.restore_service",
            L"service restoration occurred before owned resources were cleaned");
        if (!order) return order;
        state_.service_restored = true;
        return Result<void>::success();
    }

    Result<M2BaselineComparisonEvidence> compare(
        const OperationContext& context,
        const M2BaselineSnapshot& before) override {
        auto checked = check_operation_context(context);
        if (!checked) {
            return context_failure<M2BaselineComparisonEvidence>(checked);
        }
        auto order = require_state(
            state_.service_restored,
            L"local_fake.compare",
            L"baseline comparison occurred before service restoration");
        if (!order) {
            return context_failure<M2BaselineComparisonEvidence>(order);
        }
        M2FinalBaselineComparison comparison;
        comparison.comparison = M2BaselineComparison::match;
        comparison.exact_match = true;
        comparison.resources_within_tolerance = true;
        comparison.private_bytes_tolerance = 1ull << 20;
        comparison.before_fingerprint = before.fingerprint;
        comparison.after_fingerprint = before.fingerprint;
        comparison.before = before.inventory;
        comparison.after = before.inventory;
        return Result<M2BaselineComparisonEvidence>::success(
            {before.driver_service, std::move(comparison)});
    }

private:
    LocalFakeState& state_;
};

class LocalFakeProvider final : public IM2LifecycleProvider {
public:
    LocalFakeProvider(LocalFakeState& state, std::uint64_t logical_size)
        : state_(state), logical_size_(logical_size) {}

    Result<void> start(const OperationContext& context) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto order = require_state(
            state_.provider_constructed && !state_.provider_started,
            L"local_fake.start_provider",
            L"provider start order was invalid");
        if (!order) return order;
        state_.provider_started = true;
        return Result<void>::success();
    }

    Result<void> stop(const OperationContext& context) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto order = require_state(
            state_.provider_constructed && !state_.device_created,
            L"local_fake.stop_provider",
            L"provider stop occurred before device removal");
        if (!order) return order;
        state_.provider_started = false;
        state_.provider_stopped = true;
        return Result<void>::success();
    }

    Result<M2ProviderSample> sample(
        const OperationContext& context,
        M2StoreSamplePoint point) const override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<M2ProviderSample>(checked);

        bool ordered = false;
        switch (point) {
        case M2StoreSamplePoint::ready:
            ordered = state_.provider_started && !state_.write_completed;
            break;
        case M2StoreSamplePoint::after_write:
            ordered = state_.provider_started && state_.write_completed &&
                !state_.retrim_completed;
            break;
        case M2StoreSamplePoint::after_retrim:
            ordered = state_.provider_started && state_.retrim_completed &&
                !state_.performance_completed;
            break;
        case M2StoreSamplePoint::final:
            ordered = state_.provider_stopped && !state_.provider_started;
            break;
        }
        auto order = require_state(
            ordered,
            L"local_fake.sample",
            L"provider sample point was requested out of order");
        if (!order) return context_failure<M2ProviderSample>(order);

        M2ProviderSample sample;
        sample.store_stats.logical_size_bytes = logical_size_;
        sample.store_stats.reserved_bytes = logical_size_;
        sample.store_stats.backing_generation = 0;
        sample.provider_process_id = 4242;
        switch (point) {
        case M2StoreSamplePoint::ready:
            sample.private_bytes = 64ull << 20;
            sample.store_stats.committed_bytes = 0;
            break;
        case M2StoreSamplePoint::after_write:
            sample.private_bytes = 164ull << 20;
            sample.store_stats.committed_bytes = 100ull << 20;
            break;
        case M2StoreSamplePoint::after_retrim:
            sample.private_bytes = 74ull << 20;
            sample.store_stats.committed_bytes = 10ull << 20;
            sample.store_stats.unmap_bytes_total = 100ull << 20;
            break;
        case M2StoreSamplePoint::final:
            sample.private_bytes = 64ull << 20;
            sample.store_stats.committed_bytes = 10ull << 20;
            sample.store_stats.unmap_bytes_total = 100ull << 20;
            break;
        }
        sample.working_set_bytes = sample.private_bytes / 2;
        sample.commit_bytes = sample.private_bytes;
        return Result<M2ProviderSample>::success(std::move(sample));
    }

    std::string session_id() const override {
        return "m2-local-fake-session";
    }

    std::wstring native_object_path() const override {
        return L"Global\\myrd-m2-local-fake-session";
    }

private:
    LocalFakeState& state_;
    std::uint64_t logical_size_;
};

class LocalFakeProviderFactory final : public IM2LifecycleProviderFactory {
public:
    explicit LocalFakeProviderFactory(LocalFakeState& state) : state_(state) {}

    Result<std::unique_ptr<IM2LifecycleProvider>> create(
        const OperationContext& context,
        const M2GeometryOptions& geometry,
        const M2BackingState& backing) override {
        auto checked = check_operation_context(context);
        if (!checked) {
            return context_failure<
                std::unique_ptr<IM2LifecycleProvider>>(checked);
        }
        auto order = require_state(
            state_.baseline_captured && !state_.provider_constructed &&
                geometry.disk_size_bytes == kLocalFakeDiskSize &&
                backing.initial_backing_generation == 0 &&
                backing.current_backing_generation == 0,
            L"local_fake.construct_provider",
            L"provider construction inputs or order were invalid");
        if (!order) {
            return context_failure<
                std::unique_ptr<IM2LifecycleProvider>>(order);
        }
        state_.provider_constructed = true;
        std::unique_ptr<IM2LifecycleProvider> provider =
            std::make_unique<LocalFakeProvider>(
                state_, geometry.disk_size_bytes);
        return Result<std::unique_ptr<IM2LifecycleProvider>>::success(
            std::move(provider));
    }

private:
    LocalFakeState& state_;
};

M2DeviceIdentity complete_disk_identity(std::string_view run_id) {
    M2DeviceIdentity identity;
    identity.provider_session_id = "m2-local-fake-session";
    identity.native_object_path =
        R"(Global\myrd-m2-local-fake-session)";
    identity.device_id_raw = "197121";
    identity.path_id = 1;
    identity.target_id = 2;
    identity.lun = 3;
    identity.create_flags = aim::kM0CreateFlags;
    identity.query_flags = aim::kM1QueryFlags;
    identity.confirmed_by_query_device = true;
    identity.physical_drive_number = 42;
    identity.physical_drive_path = R"(\\.\PhysicalDrive42)";
    identity.disk_length_bytes = kLocalFakeDiskSize;
    identity.bus_type = "SCSI";
    identity.vendor_id = "MYRD";
    identity.product_id = "DynamicDisk";
    identity.serial_number = std::string(run_id);
    identity.disk_unique_id = "m2-local-fake-disk";
    return identity;
}

class LocalFakeDevices final : public IM2DeviceControl {
public:
    LocalFakeDevices(LocalFakeState& state, std::string_view run_id)
        : state_(state), run_id_(run_id) {}

    Result<M2OwnedDevice> create(
        const OperationContext& context,
        std::wstring_view native_object_path,
        const M2GeometryOptions& geometry) override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<M2OwnedDevice>(checked);
        auto order = require_state(
            state_.provider_started && !state_.device_created &&
                geometry.disk_size_bytes == kLocalFakeDiskSize &&
                native_object_path ==
                    L"Global\\myrd-m2-local-fake-session",
            L"local_fake.create_device",
            L"device creation inputs or order were invalid");
        if (!order) return context_failure<M2OwnedDevice>(order);
        state_.device_created = true;
        auto identity = complete_disk_identity(run_id_);
        identity.query_flags = 0;
        identity.confirmed_by_query_device = false;
        identity.physical_drive_number = 0;
        identity.physical_drive_path.clear();
        identity.disk_length_bytes = 0;
        identity.bus_type.clear();
        identity.vendor_id.clear();
        identity.product_id.clear();
        identity.serial_number.clear();
        identity.disk_unique_id.clear();
        return Result<M2OwnedDevice>::success(
            {kLocalFakeDevice, std::move(identity)});
    }

    Result<M2DeviceIdentity> wait_for_disk(
        const OperationContext& context,
        const M2OwnedDevice& owned) override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<M2DeviceIdentity>(checked);
        auto order = require_state(
            state_.device_created && owned.id.value == kLocalFakeDevice.value,
            L"local_fake.wait_for_disk",
            L"disk confirmation occurred without the owned device");
        if (!order) return context_failure<M2DeviceIdentity>(order);
        state_.disk_confirmed = true;
        return Result<M2DeviceIdentity>::success(
            complete_disk_identity(run_id_));
    }

    Result<void> remove(
        const OperationContext& context,
        aim::DeviceId id) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto order = require_state(
            state_.device_created && !state_.mounts_owned &&
                id.value == kLocalFakeDevice.value,
            L"local_fake.remove_device",
            L"device removal occurred before mount cleanup");
        if (!order) return order;
        state_.device_created = false;
        return Result<void>::success();
    }

private:
    LocalFakeState& state_;
    std::string run_id_;
};

class LocalFakeWorkload final : public IM2VolumeWorkload {
public:
    explicit LocalFakeWorkload(LocalFakeState& state) : state_(state) {}

    Result<M2IoEvidence> setup_and_write(
        const OperationContext& context,
        M2DeviceIdentity& identity,
        const M2MountsAcquired& mounts_acquired) override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<M2IoEvidence>(checked);
        auto order = require_state(
            state_.disk_confirmed && !state_.mounts_owned,
            L"local_fake.setup_write",
            L"workload setup occurred before disk confirmation");
        if (!order) return context_failure<M2IoEvidence>(order);
        identity.volume_guids = {R"(\\?\Volume{m2-local-fake}\)"};
        identity.mount_points = {R"(C:\m2-local-fake\)"};
        state_.mounts_owned = true;
        mounts_acquired();
        state_.write_completed = true;
        const std::string hash(64, 'b');
        return Result<M2IoEvidence>::success(
            {hash, hash, 4ull << 30, 4ull << 30, 0});
    }

    Result<std::uint64_t> delete_and_retrim(
        const OperationContext& context,
        M2DeviceIdentity&) override {
        auto checked = check_operation_context(context);
        if (!checked) return context_failure<std::uint64_t>(checked);
        auto order = require_state(
            state_.write_completed && !state_.retrim_completed,
            L"local_fake.delete_retrim",
            L"delete/retrim occurred before the write workload");
        if (!order) return context_failure<std::uint64_t>(order);
        state_.retrim_completed = true;
        return Result<std::uint64_t>::success(1ull << 30);
    }

    Result<M2PerformanceEvidence> run_performance(
        const OperationContext& context,
        const M2DeviceIdentity&) override {
        auto checked = check_operation_context(context);
        if (!checked) {
            return context_failure<M2PerformanceEvidence>(checked);
        }
        auto order = require_state(
            state_.retrim_completed && !state_.performance_completed,
            L"local_fake.performance",
            L"performance probe occurred before delete/retrim");
        if (!order) {
            return context_failure<M2PerformanceEvidence>(order);
        }
        state_.performance_completed = true;
        return Result<M2PerformanceEvidence>::success(
            passing_performance());
    }

    Result<void> remove_mounts(
        const OperationContext& context,
        const M2DeviceIdentity&) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto order = require_state(
            state_.mounts_owned,
            L"local_fake.remove_mounts",
            L"mount cleanup occurred without owned mounts");
        if (!order) return order;
        state_.mounts_owned = false;
        return Result<void>::success();
    }

private:
    LocalFakeState& state_;
};

class FileReportPublisher final : public IM2ReportPublisher {
public:
    explicit FileReportPublisher(std::filesystem::path final_path)
        : final_path_(std::move(final_path)) {}

    Result<void> publish(
        const OperationContext& context,
        const M2Report& report) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        return publish_m2_report(report, final_path_);
    }

private:
    std::filesystem::path final_path_;
};

} // namespace

Result<M2LocalFakeRun> run_m2_local_fake(
    const std::filesystem::path& evidence_directory,
    std::string_view run_id) {
    auto report_path =
        make_unique_m2_report_path(evidence_directory, run_id);
    if (!report_path) {
        return Result<M2LocalFakeRun>::failure(report_path.error());
    }

    LocalFakeState state;
    LocalFakeBaseline baseline(state);
    LocalFakeProviderFactory providers(state);
    LocalFakeDevices devices(state, run_id);
    LocalFakeWorkload workload(state);
    FileReportPublisher publisher(report_path.value());
    LocalFakeClock clock;

    M2LifecycleConfig config;
    config.report = local_fake_report(run_id);
    config.phase_timeout = 30s;
    auto report = run_m2_lifecycle(
        baseline,
        providers,
        devices,
        workload,
        publisher,
        clock,
        std::move(config));

    if (report.verdict != M2Verdict::go) {
        return Result<M2LocalFakeRun>::failure(local_fake_error(
            L"run_m2_local_fake",
            L"local fake lifecycle did not produce a GO verdict"));
    }
    auto readback = validate_published_m2_report(report_path.value());
    if (!readback) {
        return Result<M2LocalFakeRun>::failure(readback.error());
    }
    return Result<M2LocalFakeRun>::success(
        {std::move(report), report_path.move_value()});
}

} // namespace myrd
