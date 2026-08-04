#include "test_framework.h"

#include "src/m2/m2_lifecycle_runner.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

enum class FailAt {
    none,
    capture,
    construct_provider,
    null_provider,
    start_provider,
    create_device,
    wait_for_disk,
    setup_before_mount,
    setup_after_mount,
    sample_after_write,
    delete_retrim,
    sample_after_retrim,
    performance,
    remove_mounts,
    remove_device,
    stop_provider,
    restore_service,
    compare_baseline,
    publish_report,
};

struct Probe {
    FailAt fail_at{FailAt::none};
    std::vector<std::string> events;
    bool all_deadlines_finite{true};
    std::size_t publish_calls{};
    std::stop_source* cancel_source{};
    std::string cancel_on_event;
    std::string advance_on_event;
    std::function<void()> advance_clock;
    bool invalid_report_template{};
    bool provider_destroyed{};
    bool provider_destroyed_at_compare{};

    void observe(std::string event, const myrd::OperationContext& context) {
        events.push_back(std::move(event));
        all_deadlines_finite = all_deadlines_finite &&
            context.deadline !=
                std::chrono::steady_clock::time_point::max();
        if (cancel_source != nullptr &&
            events.back() == cancel_on_event) {
            cancel_source->request_stop();
        }
        if (advance_clock && events.back() == advance_on_event) {
            advance_clock();
        }
    }
};

myrd::Error injected_error(std::string_view operation) {
    std::wstring wide(operation.begin(), operation.end());
    return {myrd::ErrorDomain::internal, 0xE019u, std::move(wide),
            L"injected lifecycle failure"};
}

template <typename T>
myrd::Result<T> injected_failure(std::string_view operation) {
    return myrd::Result<T>::failure(injected_error(operation));
}

myrd::Result<void> injected_void_failure(std::string_view operation) {
    return myrd::Result<void>::failure(injected_error(operation));
}

myrd::M2InventorySnapshot baseline_inventory() {
    myrd::M2InventorySnapshot snapshot;
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

myrd::M2DriverServiceState driver_state() {
    return {
        "loaded", "phdskmnt", "running", "automatic",
        R"(C:\Windows\System32\drivers\phdskmnt.sys)", "1.2.28.93",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
}

myrd::M2PerformanceProbeResult performance_sample(
    std::uint64_t sequential_rate,
    std::uint64_t random_rate) {
    myrd::M2PerformanceProbeResult sample;
    sample.operation = myrd::M2RawOperation::Write;
    sample.region_offset = 64ull << 20;
    sample.policy = {
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        "single-thread-normal-priority",
        "FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH"};
    const auto frequency = sequential_rate * random_rate;
    sample.sequential = {
        myrd::kM2SequentialRegionBytes,
        myrd::kM2SequentialOperationCount,
        myrd::kM2SequentialRegionBytes * random_rate,
        frequency,
        sequential_rate};
    sample.random = {
        myrd::kM2RandomIoBytes * myrd::kM2RandomOperationCount,
        myrd::kM2RandomOperationCount,
        myrd::kM2RandomOperationCount * sequential_rate,
        frequency,
        random_rate};
    return sample;
}

myrd::M2PerformanceEvidence passing_performance() {
    myrd::M2PerformanceEvidence evidence;
    evidence.fixed = performance_sample(1'000'000, 10'000);
    evidence.dynamic = performance_sample(700'000, 6'000);
    evidence.comparison.sequential_ratio_basis_points = 7'000;
    evidence.comparison.random_ratio_basis_points = 6'000;
    evidence.comparison.sequential_pass = true;
    evidence.comparison.random_pass = true;
    evidence.comparison.overall_pass = true;
    evidence.gate_status = myrd::M2PerformanceGateStatus::passed;
    return evidence;
}

myrd::M2Report report_template() {
    myrd::M2Report report;
    report.run.run_id = "run-0123456789abcdef";
    report.run.mode = myrd::M2RunMode::fake;
    report.build_toolchain = {
        "2.0.0", "225e332", "Debug", "MSVC", "19.44", "4.1",
        "myrd-m2-e2e.exe",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"};
    report.accepted_m1_baseline = {
        "m1-accepted.json",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        1, "GO", true};
    report.machine_os = {
        "test-machine", "Windows", "11 24H2", "x64", "26100",
        "Test CPU", 16, 32ull << 30,
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    report.geometry_options.disk_size_bytes = 1ull << 40;
    report.geometry_options.sector_size_bytes = 512;
    report.geometry_options.block_size_bytes = 64ull << 10;
    report.geometry_options.soft_limit_bytes = 512ull << 20;
    report.geometry_options.hard_limit_bytes = 1ull << 30;
    report.geometry_options.eviction_low_watermark_bytes = 384ull << 20;
    report.geometry_options.max_snapshot_batch_bytes = 64ull << 20;
    report.geometry_options.max_snapshot_batch_records = 1024;
    report.geometry_options.lock_stripe_count = 64;
    report.geometry_options.backing_kind = "fake";
    report.state.initial_state = myrd::M2InitialState::empty;
    report.state.initial_backing_generation = 0;
    report.state.current_backing_generation = 0;
    return report;
}

class FakeClock final : public myrd::IM2LifecycleClock {
public:
    explicit FakeClock(
        std::chrono::milliseconds step = 100ms)
        : step_(step) {}

    std::chrono::steady_clock::time_point steady_now() override {
        now_ += step_;
        return now_;
    }

    std::string utc_now() override {
        return "2026-08-04T01:02:03Z";
    }

    void advance(std::chrono::milliseconds amount) {
        now_ += amount;
    }

private:
    std::chrono::milliseconds step_;
    std::chrono::steady_clock::time_point now_{
        std::chrono::steady_clock::now()};
};

class FakeBaseline final : public myrd::IM2BaselineControl {
public:
    explicit FakeBaseline(Probe& probe) : probe_(probe) {}

    myrd::Result<myrd::M2BaselineSnapshot> capture(
        const myrd::OperationContext& context) override {
        probe_.observe("capture_baseline", context);
        if (probe_.fail_at == FailAt::capture) {
            return injected_failure<myrd::M2BaselineSnapshot>("capture");
        }
        return myrd::Result<myrd::M2BaselineSnapshot>::success(
            {driver_state(), baseline_inventory(),
             "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"});
    }

    myrd::Result<void> restore_service(
        const myrd::OperationContext& context,
        const myrd::M2BaselineSnapshot&) override {
        probe_.observe("restore_service", context);
        if (probe_.fail_at == FailAt::restore_service) {
            return injected_void_failure("restore_service");
        }
        return myrd::Result<void>::success();
    }

    myrd::Result<myrd::M2BaselineComparisonEvidence> compare(
        const myrd::OperationContext& context,
        const myrd::M2BaselineSnapshot& before) override {
        probe_.observe("compare_baseline", context);
        probe_.provider_destroyed_at_compare = probe_.provider_destroyed;
        if (probe_.fail_at == FailAt::compare_baseline) {
            return injected_failure<myrd::M2BaselineComparisonEvidence>(
                "compare_baseline");
        }
        myrd::M2FinalBaselineComparison comparison;
        comparison.comparison = myrd::M2BaselineComparison::match;
        comparison.exact_match = true;
        comparison.resources_within_tolerance = true;
        comparison.private_bytes_tolerance = 1ull << 20;
        comparison.before_fingerprint = before.fingerprint;
        comparison.after_fingerprint = before.fingerprint;
        comparison.before = before.inventory;
        comparison.after = before.inventory;
        if (return_mismatch) {
            comparison.comparison = myrd::M2BaselineComparison::mismatch;
            comparison.exact_match = false;
            comparison.resources_within_tolerance = false;
            comparison.after_fingerprint =
                "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
            comparison.after.process_handle_count += 2;
            comparison.after.process_thread_count += 1;
            comparison.after.process_private_bytes +=
                comparison.private_bytes_tolerance + 4096;
            comparison.diff.added.push_back("volume:stale-volume");
            comparison.diff.changed.push_back("service_state");
        }
        return myrd::Result<myrd::M2BaselineComparisonEvidence>::success(
            {before.driver_service, std::move(comparison)});
    }

    bool return_mismatch = false;

private:
    Probe& probe_;
};

class FakeProvider final : public myrd::IM2LifecycleProvider {
public:
    FakeProvider(Probe& probe, std::uint64_t logical_size)
        : probe_(probe), logical_size_(logical_size) {}

    ~FakeProvider() override { probe_.provider_destroyed = true; }

    myrd::Result<void> start(
        const myrd::OperationContext& context) override {
        probe_.observe("start_provider", context);
        if (probe_.fail_at == FailAt::start_provider) {
            return injected_void_failure("start_provider");
        }
        return myrd::Result<void>::success();
    }

    myrd::Result<void> stop(
        const myrd::OperationContext& context) override {
        probe_.observe("stop_provider", context);
        if (probe_.fail_at == FailAt::stop_provider) {
            return injected_void_failure("stop_provider");
        }
        return myrd::Result<void>::success();
    }

    myrd::Result<myrd::M2ProviderSample> sample(
        const myrd::OperationContext& context,
        myrd::M2StoreSamplePoint point) const override {
        std::string event = "sample:" + std::string(myrd::to_string(point));
        probe_.observe(std::move(event), context);
        if ((point == myrd::M2StoreSamplePoint::after_write &&
             probe_.fail_at == FailAt::sample_after_write) ||
            (point == myrd::M2StoreSamplePoint::after_retrim &&
             probe_.fail_at == FailAt::sample_after_retrim)) {
            return injected_failure<myrd::M2ProviderSample>("sample");
        }

        myrd::M2ProviderSample sample;
        sample.store_stats.logical_size_bytes = logical_size_;
        sample.store_stats.reserved_bytes = logical_size_;
        sample.store_stats.backing_generation = 0;
        sample.provider_process_id = 4242;
        switch (point) {
        case myrd::M2StoreSamplePoint::ready:
            sample.private_bytes = 64ull << 20;
            sample.store_stats.committed_bytes = 0;
            break;
        case myrd::M2StoreSamplePoint::after_write:
            sample.private_bytes = 164ull << 20;
            sample.store_stats.committed_bytes = 100ull << 20;
            break;
        case myrd::M2StoreSamplePoint::after_retrim:
            sample.private_bytes = 74ull << 20;
            sample.store_stats.committed_bytes = 10ull << 20;
            sample.store_stats.unmap_bytes_total = 100ull << 20;
            break;
        case myrd::M2StoreSamplePoint::final:
            sample.private_bytes = 64ull << 20;
            sample.store_stats.committed_bytes = 10ull << 20;
            sample.store_stats.unmap_bytes_total = 100ull << 20;
            break;
        }
        sample.working_set_bytes = sample.private_bytes / 2;
        sample.commit_bytes = sample.private_bytes;
        return myrd::Result<myrd::M2ProviderSample>::success(
            std::move(sample));
    }

    std::string session_id() const override { return "m2-session-0001"; }
    std::wstring native_object_path() const override {
        return L"Global\\myrd-m2-session-0001";
    }

private:
    Probe& probe_;
    std::uint64_t logical_size_;
};

class FakeProviderFactory final : public myrd::IM2LifecycleProviderFactory {
public:
    explicit FakeProviderFactory(Probe& probe) : probe_(probe) {}

    myrd::Result<std::unique_ptr<myrd::IM2LifecycleProvider>> create(
        const myrd::OperationContext& context,
        const myrd::M2GeometryOptions& geometry,
        const myrd::M2BackingState& backing) override {
        probe_.observe("construct_provider", context);
        REQUIRE_EQ(backing.initial_backing_generation, 0ull);
        REQUIRE_EQ(backing.current_backing_generation, 0ull);
        REQUIRE(!backing.durable_successor_installed);
        if (probe_.fail_at == FailAt::construct_provider) {
            return injected_failure<
                std::unique_ptr<myrd::IM2LifecycleProvider>>(
                    "construct_provider");
        }
        if (probe_.fail_at == FailAt::null_provider) {
            return myrd::Result<
                std::unique_ptr<myrd::IM2LifecycleProvider>>::success(
                    nullptr);
        }
        std::unique_ptr<myrd::IM2LifecycleProvider> provider =
            std::make_unique<FakeProvider>(probe_, geometry.disk_size_bytes);
        return myrd::Result<
            std::unique_ptr<myrd::IM2LifecycleProvider>>::success(
                std::move(provider));
    }

private:
    Probe& probe_;
};

myrd::M2DeviceIdentity complete_disk_identity(std::uint64_t disk_size) {
    myrd::M2DeviceIdentity identity;
    identity.provider_session_id = "m2-session-0001";
    identity.native_object_path = R"(Global\myrd-m2-session-0001)";
    identity.device_id_raw = "197121";
    identity.path_id = 1;
    identity.target_id = 2;
    identity.lun = 3;
    identity.create_flags = myrd::aim::kM0CreateFlags;
    identity.query_flags = myrd::aim::kM1QueryFlags;
    identity.confirmed_by_query_device = true;
    identity.physical_drive_number = 42;
    identity.physical_drive_path = R"(\\.\PhysicalDrive42)";
    identity.disk_length_bytes = disk_size;
    identity.bus_type = "SCSI";
    identity.vendor_id = "MYRD";
    identity.product_id = "DynamicDisk";
    identity.serial_number = "run-0123456789abcdef";
    identity.disk_unique_id = "m2-disk-0001";
    return identity;
}

class FakeDevices final : public myrd::IM2DeviceControl {
public:
    explicit FakeDevices(Probe& probe) : probe_(probe) {}

    std::uint32_t created_id = 197121u;

    myrd::Result<myrd::M2OwnedDevice> create(
        const myrd::OperationContext& context,
        std::wstring_view native_object_path,
        const myrd::M2GeometryOptions& geometry) override {
        probe_.observe("create_device", context);
        if (probe_.fail_at == FailAt::create_device) {
            return injected_failure<myrd::M2OwnedDevice>("create_device");
        }
        REQUIRE_EQ(
            native_object_path,
            std::wstring_view(L"Global\\myrd-m2-session-0001"));
        auto identity = complete_disk_identity(geometry.disk_size_bytes);
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
        identity.device_id_raw = std::to_string(created_id);
        const auto address = myrd::aim::decode_device_address({created_id});
        identity.path_id = address.path_id;
        identity.target_id = address.target_id;
        identity.lun = address.lun;
        return myrd::Result<myrd::M2OwnedDevice>::success(
            {{created_id}, std::move(identity)});
    }

    myrd::Result<myrd::M2DeviceIdentity> wait_for_disk(
        const myrd::OperationContext& context,
        const myrd::M2OwnedDevice&) override {
        probe_.observe("wait_for_disk", context);
        if (probe_.fail_at == FailAt::wait_for_disk) {
            return injected_failure<myrd::M2DeviceIdentity>("wait_for_disk");
        }
        auto identity = complete_disk_identity(1ull << 40);
        identity.device_id_raw = std::to_string(created_id);
        const auto address = myrd::aim::decode_device_address({created_id});
        identity.path_id = address.path_id;
        identity.target_id = address.target_id;
        identity.lun = address.lun;
        return myrd::Result<myrd::M2DeviceIdentity>::success(
            std::move(identity));
    }

    myrd::Result<void> remove(
        const myrd::OperationContext& context,
        myrd::aim::DeviceId id) override {
        probe_.observe("remove_device", context);
        REQUIRE_EQ(id.value, created_id);
        if (probe_.fail_at == FailAt::remove_device) {
            return injected_void_failure("remove_device");
        }
        return myrd::Result<void>::success();
    }

private:
    Probe& probe_;
};

class FakeWorkload final : public myrd::IM2VolumeWorkload {
public:
    explicit FakeWorkload(Probe& probe) : probe_(probe) {}

    myrd::Result<myrd::M2IoEvidence> setup_and_write(
        const myrd::OperationContext& context,
        myrd::M2DeviceIdentity& identity,
        const myrd::M2MountsAcquired& mounts_acquired) override {
        probe_.observe("setup_write", context);
        if (probe_.fail_at == FailAt::setup_before_mount) {
            return injected_failure<myrd::M2IoEvidence>("setup_before_mount");
        }
        identity.volume_guids = {R"(\\?\Volume{m2-test}\)"};
        identity.mount_points = {R"(C:\m2-test\)"};
        mounts_acquired();
        if (probe_.fail_at == FailAt::setup_after_mount) {
            return injected_failure<myrd::M2IoEvidence>("setup_after_mount");
        }
        const std::string hash(64, 'b');
        return myrd::Result<myrd::M2IoEvidence>::success(
            {hash, hash, 4ull << 30, 4ull << 30, 0});
    }

    myrd::Result<std::uint64_t> delete_and_retrim(
        const myrd::OperationContext& context,
        myrd::M2DeviceIdentity&) override {
        probe_.observe("delete_retrim", context);
        if (probe_.fail_at == FailAt::delete_retrim) {
            return injected_failure<std::uint64_t>("delete_retrim");
        }
        return myrd::Result<std::uint64_t>::success(1ull << 30);
    }

    myrd::Result<myrd::M2PerformanceEvidence> run_performance(
        const myrd::OperationContext& context,
        const myrd::M2DeviceIdentity&) override {
        probe_.observe("performance", context);
        if (probe_.fail_at == FailAt::performance) {
            return injected_failure<myrd::M2PerformanceEvidence>(
                "performance");
        }
        return myrd::Result<myrd::M2PerformanceEvidence>::success(
            passing_performance());
    }

    myrd::Result<void> remove_mounts(
        const myrd::OperationContext& context,
        const myrd::M2DeviceIdentity&) override {
        probe_.observe("remove_mounts", context);
        if (probe_.fail_at == FailAt::remove_mounts) {
            return injected_void_failure("remove_mounts");
        }
        return myrd::Result<void>::success();
    }

private:
    Probe& probe_;
};

class FakePublisher final : public myrd::IM2ReportPublisher {
public:
    explicit FakePublisher(Probe& probe) : probe_(probe) {}

    myrd::Result<void> publish(
        const myrd::OperationContext& context,
        const myrd::M2Report& report) override {
        ++probe_.publish_calls;
        probe_.observe("publish_report", context);
        published = report;
        if (probe_.fail_at == FailAt::publish_report) {
            return injected_void_failure("publish_report");
        }
        auto valid = myrd::validate_m2_report_json(
            myrd::serialize_m2_report(report));
        if (!valid) return valid;
        return myrd::Result<void>::success();
    }

    myrd::M2Report published;

private:
    Probe& probe_;
};

struct Fixture {
    explicit Fixture(
        FailAt fail_at = FailAt::none,
        std::chrono::milliseconds clock_step = 100ms)
        : probe{fail_at}, baseline(probe), providers(probe), devices(probe),
          workload(probe), publisher(probe), clock(clock_step) {}

    myrd::M2Report run(
        std::stop_token stop_token = {},
        std::chrono::milliseconds phase_timeout = 5s,
        std::optional<myrd::M2PhaseId> injected_failure_after =
            std::nullopt) {
        myrd::M2LifecycleConfig config;
        config.report = report_template();
        if (probe.invalid_report_template) {
            config.report.schema_version = 0;
        }
        config.phase_timeout = phase_timeout;
        config.stop_token = stop_token;
        config.injected_failure_after = injected_failure_after;
        return myrd::run_m2_lifecycle(
            baseline, providers, devices, workload, publisher, clock,
            std::move(config));
    }

    Probe probe;
    FakeBaseline baseline;
    FakeProviderFactory providers;
    FakeDevices devices;
    FakeWorkload workload;
    FakePublisher publisher;
    FakeClock clock;
};

const myrd::M2CleanupAction& cleanup(
    const myrd::M2Report& report,
    myrd::M2CleanupActionId id) {
    const auto it = std::find_if(
        report.cleanup_actions.begin(), report.cleanup_actions.end(),
        [id](const auto& action) { return action.id == id; });
    REQUIRE(it != report.cleanup_actions.end());
    return *it;
}

const myrd::M2Phase& phase(
    const myrd::M2Report& report,
    myrd::M2PhaseId id) {
    const auto it = std::find_if(
        report.phases.begin(), report.phases.end(),
        [id](const auto& value) { return value.id == id; });
    REQUIRE(it != report.phases.end());
    return *it;
}

} // namespace

TEST_CASE(m2_lifecycle_success_uses_exact_order_and_publishes_valid_go) {
    Fixture fixture;
    const auto report = fixture.run();

    const std::vector<std::string> expected = {
        "capture_baseline", "construct_provider", "start_provider",
        "sample:READY", "create_device", "wait_for_disk", "setup_write",
        "sample:AFTER-WRITE", "delete_retrim", "sample:AFTER-RETRIM",
        "performance", "remove_mounts", "remove_device", "stop_provider",
        "sample:FINAL", "restore_service", "compare_baseline",
        "publish_report"};
    REQUIRE_EQ(fixture.probe.events, expected);
    REQUIRE(fixture.probe.all_deadlines_finite);
    REQUIRE_EQ(report.verdict, myrd::M2Verdict::go);
    REQUIRE_EQ(report.failures.size(), 0u);
    REQUIRE_EQ(report.phases.size(), 16u);
    REQUIRE(std::all_of(
        report.phases.begin(), report.phases.end(),
        [](const auto& value) {
            return value.status == myrd::M2PhaseStatus::passed;
        }));
    REQUIRE_EQ(report.cleanup_actions.size(), 4u);
    REQUIRE(std::all_of(
        report.cleanup_actions.begin(), report.cleanup_actions.end(),
        [](const auto& action) {
            return action.attempted &&
                action.result == myrd::M2CleanupResult::succeeded;
        }));
    REQUIRE_EQ(report.store_stats_samples.size(), 4u);
    REQUIRE_EQ(report.os_memory_samples.size(), 4u);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_setup_failure_after_mount_rolls_back_every_owned_resource) {
    Fixture fixture(FailAt::setup_after_mount);
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::setup_write).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::remove_mounts).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::remove_device).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::stop_provider).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::restore_service).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(fixture.probe.events, (std::vector<std::string>{
        "capture_baseline", "construct_provider", "start_provider",
        "sample:READY", "create_device", "wait_for_disk", "setup_write",
        "remove_mounts", "remove_device", "stop_provider", "sample:FINAL",
        "restore_service", "compare_baseline", "publish_report"}));
}

TEST_CASE(m2_lifecycle_start_failure_stops_constructed_provider_without_device_mutation) {
    Fixture fixture(FailAt::start_provider);
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::remove_mounts).result,
        myrd::M2CleanupResult::not_attempted);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::remove_device).result,
        myrd::M2CleanupResult::not_attempted);
    REQUIRE_EQ(
        cleanup(report, myrd::M2CleanupActionId::stop_provider).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(fixture.probe.events, (std::vector<std::string>{
        "capture_baseline", "construct_provider", "start_provider",
        "stop_provider", "sample:FINAL", "restore_service",
        "compare_baseline", "publish_report"}));
}

TEST_CASE(m2_lifecycle_cleanup_failure_continues_remaining_cleanup_and_forces_no_go) {
    for (const auto fail_at : {
             FailAt::remove_mounts,
             FailAt::remove_device,
             FailAt::stop_provider,
             FailAt::restore_service}) {
        Fixture fixture(fail_at);
        const auto report = fixture.run();

        REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
        REQUIRE(!report.failures.empty());
        REQUIRE_EQ(fixture.probe.events.back(), "publish_report");
        REQUIRE(std::find(
                    fixture.probe.events.begin(),
                    fixture.probe.events.end(),
                    "compare_baseline") != fixture.probe.events.end());
        REQUIRE_EQ(fixture.probe.publish_calls, 1u);
    }
}

TEST_CASE(m2_lifecycle_every_main_failure_point_publishes_valid_no_go_and_exact_cleanup) {
    struct Case {
        FailAt fail_at;
        myrd::M2PhaseId failed_phase;
        bool mounts;
        bool device;
        bool provider;
        bool service;
    };
    const Case cases[] = {
        {FailAt::capture, myrd::M2PhaseId::capture_baseline,
         false, false, false, false},
        {FailAt::construct_provider, myrd::M2PhaseId::construct_provider,
         false, false, false, true},
        {FailAt::start_provider, myrd::M2PhaseId::start_provider,
         false, false, true, true},
        {FailAt::create_device, myrd::M2PhaseId::create_device,
         false, false, true, true},
        {FailAt::wait_for_disk, myrd::M2PhaseId::wait_for_disk,
         false, true, true, true},
        {FailAt::setup_before_mount, myrd::M2PhaseId::setup_write,
         false, true, true, true},
        {FailAt::setup_after_mount, myrd::M2PhaseId::setup_write,
         true, true, true, true},
        {FailAt::sample_after_write, myrd::M2PhaseId::sample_after_write,
         true, true, true, true},
        {FailAt::delete_retrim, myrd::M2PhaseId::delete_retrim,
         true, true, true, true},
        {FailAt::sample_after_retrim, myrd::M2PhaseId::sample_after_retrim,
         true, true, true, true},
        {FailAt::performance, myrd::M2PhaseId::performance,
         true, true, true, true},
        {FailAt::compare_baseline, myrd::M2PhaseId::compare_baseline,
         true, true, true, true},
    };

    for (const auto& item : cases) {
        Fixture fixture(item.fail_at);
        const auto report = fixture.run();

        REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
        REQUIRE_EQ(
            phase(report, item.failed_phase).status,
            myrd::M2PhaseStatus::failed);
        REQUIRE_EQ(
            cleanup(
                report,
                myrd::M2CleanupActionId::remove_mounts).attempted,
            item.mounts);
        REQUIRE_EQ(
            cleanup(
                report,
                myrd::M2CleanupActionId::remove_device).attempted,
            item.device);
        REQUIRE_EQ(
            cleanup(
                report,
                myrd::M2CleanupActionId::stop_provider).attempted,
            item.provider);
        REQUIRE_EQ(
            cleanup(
                report,
                myrd::M2CleanupActionId::restore_service).attempted,
            item.service);
        REQUIRE_EQ(
            phase(report, myrd::M2PhaseId::publish_report).status,
            myrd::M2PhaseStatus::passed);
        REQUIRE_EQ(fixture.probe.publish_calls, 1u);
        REQUIRE(myrd::validate_m2_report_json(
                    myrd::serialize_m2_report(report)).ok());
    }
}

TEST_CASE(m2_lifecycle_every_injected_main_phase_uses_owned_resource_cleanup) {
    struct Case {
        myrd::M2PhaseId phase;
        bool mounts;
        bool device;
        bool provider;
        bool service;
    };
    const Case cases[] = {
        {myrd::M2PhaseId::capture_baseline, false, false, false, true},
        {myrd::M2PhaseId::construct_provider, false, false, true, true},
        {myrd::M2PhaseId::start_provider, false, false, true, true},
        {myrd::M2PhaseId::create_device, false, true, true, true},
        {myrd::M2PhaseId::wait_for_disk, false, true, true, true},
        {myrd::M2PhaseId::setup_write, true, true, true, true},
        {myrd::M2PhaseId::sample_after_write, true, true, true, true},
        {myrd::M2PhaseId::delete_retrim, true, true, true, true},
        {myrd::M2PhaseId::sample_after_retrim, true, true, true, true},
        {myrd::M2PhaseId::performance, true, true, true, true},
    };

    for (const auto& item : cases) {
        Fixture fixture;
        const auto report = fixture.run({}, 5s, item.phase);

        REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
        REQUIRE_EQ(
            phase(report, item.phase).status,
            myrd::M2PhaseStatus::failed);
        REQUIRE(
            phase(report, item.phase).error.find(
                "injected acceptance failure") != std::string::npos);
        REQUIRE_EQ(
            cleanup(report, myrd::M2CleanupActionId::remove_mounts)
                .attempted,
            item.mounts);
        REQUIRE_EQ(
            cleanup(report, myrd::M2CleanupActionId::remove_device)
                .attempted,
            item.device);
        REQUIRE_EQ(
            cleanup(report, myrd::M2CleanupActionId::stop_provider)
                .attempted,
            item.provider);
        REQUIRE_EQ(
            cleanup(report, myrd::M2CleanupActionId::restore_service)
                .attempted,
            item.service);
        REQUIRE(myrd::validate_m2_report_json(
                    myrd::serialize_m2_report(report)).ok());
    }
}

TEST_CASE(m2_lifecycle_fast_fault_samples_keep_strict_elapsed_order) {
    Fixture fixture(FailAt::none, 0ms);
    const auto report = fixture.run(
        {}, 5s, myrd::M2PhaseId::start_provider);

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(fixture.probe.publish_calls, 1u);
    REQUIRE_EQ(report.os_memory_samples.size(), 2u);
    REQUIRE(
        report.os_memory_samples[1].elapsed_ms >
        report.os_memory_samples[0].elapsed_ms);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_publish_failure_returns_no_go_without_claiming_publication) {
    Fixture fixture(FailAt::publish_report);
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::publish_report).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(fixture.probe.publish_calls, 1u);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_pre_cancelled_run_has_no_mutation_and_still_publishes_no_go) {
    Fixture fixture;
    std::stop_source source;
    source.request_stop();
    const auto report = fixture.run(source.get_token());

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(fixture.probe.events, (std::vector<std::string>{
        "publish_report"}));
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::capture_baseline).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE(std::all_of(
        report.cleanup_actions.begin(), report.cleanup_actions.end(),
        [](const auto& action) { return !action.attempted; }));
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_cancellation_after_mount_runs_uncancelled_exact_cleanup) {
    Fixture fixture;
    std::stop_source source;
    fixture.probe.cancel_source = &source;
    fixture.probe.cancel_on_event = "setup_write";
    const auto report = fixture.run(source.get_token());

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::setup_write).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE(
        phase(report, myrd::M2PhaseId::setup_write).error.find(
            "cancelled") != std::string::npos);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::remove_mounts).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::remove_device).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::stop_provider).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::restore_service).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_detects_success_returned_after_phase_deadline) {
    Fixture fixture(FailAt::none, 3s);
    const auto report = fixture.run({}, 5s);

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::capture_baseline).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE(
        phase(report, myrd::M2PhaseId::capture_baseline).error.find(
            "deadline") != std::string::npos);
    REQUIRE_EQ(fixture.probe.events, (std::vector<std::string>{
        "capture_baseline", "publish_report"}));
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_late_provider_acquisition_is_still_owned_and_stopped) {
    Fixture fixture;
    fixture.probe.advance_on_event = "construct_provider";
    fixture.probe.advance_clock = [&fixture]() {
        fixture.clock.advance(10s);
    };
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::construct_provider).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::stop_provider).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE(std::find(
                fixture.probe.events.begin(),
                fixture.probe.events.end(),
                "stop_provider") != fixture.probe.events.end());
}

TEST_CASE(m2_lifecycle_late_device_acquisition_is_still_owned_and_removed) {
    Fixture fixture;
    fixture.probe.advance_on_event = "create_device";
    fixture.probe.advance_clock = [&fixture]() {
        fixture.clock.advance(10s);
    };
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::create_device).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::remove_device).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE(std::find(
                fixture.probe.events.begin(),
                fixture.probe.events.end(),
                "remove_device") != fixture.probe.events.end());
}

TEST_CASE(m2_lifecycle_accepts_and_cleans_up_valid_zero_scsi_address) {
    Fixture fixture;
    fixture.devices.created_id = 0;
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::go);
    REQUIRE_EQ(report.device_identity.device_id_raw, std::string("0"));
    REQUIRE_EQ(report.device_identity.path_id, 0u);
    REQUIRE_EQ(report.device_identity.target_id, 0u);
    REQUIRE_EQ(report.device_identity.lun, 0u);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::remove_device).result,
        myrd::M2CleanupResult::succeeded);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_successful_publication_is_the_final_report_state) {
    Fixture fixture;
    fixture.probe.advance_on_event = "publish_report";
    fixture.probe.advance_clock = [&fixture]() {
        fixture.clock.advance(10s);
    };
    const auto report = fixture.run();

    REQUIRE_EQ(fixture.probe.publish_calls, 1u);
    REQUIRE_EQ(report.verdict, myrd::M2Verdict::go);
    REQUIRE_EQ(report.verdict, fixture.publisher.published.verdict);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::publish_report).status,
        phase(
            fixture.publisher.published,
            myrd::M2PhaseId::publish_report).status);
    REQUIRE_EQ(
        myrd::serialize_m2_report(report),
        myrd::serialize_m2_report(fixture.publisher.published));
}

TEST_CASE(m2_lifecycle_null_provider_is_rejected_without_dereference) {
    Fixture fixture(FailAt::null_provider);
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::construct_provider).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(
        cleanup(
            report,
            myrd::M2CleanupActionId::stop_provider).result,
        myrd::M2CleanupResult::not_attempted);
    REQUIRE_EQ(fixture.probe.events, (std::vector<std::string>{
        "capture_baseline", "construct_provider", "restore_service",
        "compare_baseline", "publish_report"}));
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_lifecycle_never_hands_an_invalid_report_to_the_publisher) {
    Fixture fixture;
    fixture.probe.invalid_report_template = true;
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    REQUIRE_EQ(
        phase(report, myrd::M2PhaseId::publish_report).status,
        myrd::M2PhaseStatus::failed);
    REQUIRE_EQ(fixture.probe.publish_calls, 0u);
}

TEST_CASE(m2_lifecycle_baseline_mismatch_reports_structural_and_resource_details) {
    Fixture fixture;
    fixture.baseline.return_mismatch = true;
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::no_go);
    const auto& error = phase(
        report, myrd::M2PhaseId::compare_baseline).error;
    REQUIRE(error.find("volume:stale-volume") != std::string::npos);
    REQUIRE(error.find("service_state") != std::string::npos);
    REQUIRE(error.find("handles=") != std::string::npos);
    REQUIRE(error.find("threads=") != std::string::npos);
    REQUIRE(error.find("private_bytes=") != std::string::npos);
}

TEST_CASE(m2_lifecycle_destroys_stopped_provider_before_baseline_comparison) {
    Fixture fixture;
    const auto report = fixture.run();

    REQUIRE_EQ(report.verdict, myrd::M2Verdict::go);
    REQUIRE(fixture.probe.provider_destroyed_at_compare);
}

int main() {
    for (const auto& test : myrd::test::registry()) {
        test.func();
        std::cout << "PASS " << test.name << '\n';
    }
    return 0;
}
