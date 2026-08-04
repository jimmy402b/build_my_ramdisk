#include "test_framework.h"

#include "src/m2/m2_report.h"
#include "src/driver/aim_protocol.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

myrd::M2PerformanceProbeResult performance_sample(
    std::uint64_t sequential_rate,
    std::uint64_t random_rate) {
    myrd::M2PerformanceProbeResult sample;
    sample.operation = myrd::M2RawOperation::Write;
    sample.region_offset = 64ULL << 20U;
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

myrd::M2StoreStatsSample store_sample(
    myrd::M2StoreSamplePoint point,
    std::uint64_t committed_bytes,
    std::uint64_t unmap_bytes) {
    myrd::M2StoreStatsSample sample;
    sample.point = point;
    sample.captured_utc = "2026-08-04T01:02:30Z";
    sample.stats.logical_size_bytes = 1ULL << 40U;
    sample.stats.reserved_bytes = 1ULL << 40U;
    sample.stats.committed_bytes = committed_bytes;
    sample.stats.unmap_bytes_total = unmap_bytes;
    sample.stats.backing_generation = 0;
    return sample;
}

myrd::M2OsMemorySample memory_sample(
    myrd::M2StoreSamplePoint point,
    myrd::M2PhaseId phase_id,
    std::string captured_utc,
    std::uint64_t elapsed_ms,
    std::uint64_t private_bytes,
    std::uint64_t commit_bytes) {
    myrd::M2OsMemorySample sample;
    sample.point = point;
    sample.phase_id = phase_id;
    sample.captured_utc = std::move(captured_utc);
    sample.provider_process_id = 4242;
    sample.elapsed_ms = elapsed_ms;
    sample.working_set_bytes = private_bytes / 2;
    sample.private_bytes = private_bytes;
    sample.commit_bytes = commit_bytes;
    return sample;
}

std::vector<myrd::M2Phase> passed_phases() {
    const myrd::M2PhaseId ids[] = {
        myrd::M2PhaseId::capture_baseline,
        myrd::M2PhaseId::construct_provider,
        myrd::M2PhaseId::start_provider,
        myrd::M2PhaseId::create_device,
        myrd::M2PhaseId::wait_for_disk,
        myrd::M2PhaseId::setup_write,
        myrd::M2PhaseId::sample_after_write,
        myrd::M2PhaseId::delete_retrim,
        myrd::M2PhaseId::sample_after_retrim,
        myrd::M2PhaseId::performance,
        myrd::M2PhaseId::remove_mounts,
        myrd::M2PhaseId::remove_device,
        myrd::M2PhaseId::stop_provider,
        myrd::M2PhaseId::restore_service,
        myrd::M2PhaseId::compare_baseline,
        myrd::M2PhaseId::publish_report};
    std::vector<myrd::M2Phase> phases;
    for (const auto id : ids) {
        phases.push_back({id, "2026-08-04T01:02:04Z",
                          "2026-08-04T01:02:05Z",
                          myrd::M2PhaseStatus::passed, "", 1000});
    }
    return phases;
}

std::vector<myrd::M2CleanupAction> succeeded_cleanup_actions() {
    return {
        {myrd::M2CleanupActionId::remove_mounts, true,
         myrd::M2CleanupResult::succeeded, ""},
        {myrd::M2CleanupActionId::remove_device, true,
         myrd::M2CleanupResult::succeeded, ""},
        {myrd::M2CleanupActionId::stop_provider, true,
         myrd::M2CleanupResult::succeeded, ""},
        {myrd::M2CleanupActionId::restore_service, true,
         myrd::M2CleanupResult::succeeded, ""}};
}

myrd::M2InventorySnapshot inventory_snapshot() {
    myrd::M2InventorySnapshot snapshot;
    snapshot.physical_disks = {"0", "1"};
    snapshot.volumes = {R"(\\?\Volume{system}\)"};
    snapshot.mount_points = {R"(C:\)"};
    snapshot.service_state = "running";
    snapshot.service_start_type = "automatic";
    snapshot.process_handle_count = 100;
    snapshot.process_thread_count = 10;
    snapshot.process_private_bytes = 64ULL << 20U;
    return snapshot;
}

myrd::M2Report complete_report() {
    myrd::M2Report report;
    report.run = {"run-0123456789abcdef", myrd::M2RunMode::fake,
                  "2026-08-04T01:02:03Z", "2026-08-04T01:03:04Z"};
    report.build_toolchain.build_version = "2.0.0";
    report.build_toolchain.git_commit = "5ee5a00";
    report.build_toolchain.configuration = "Debug";
    report.build_toolchain.compiler = "MSVC";
    report.build_toolchain.compiler_version = "19.44";
    report.build_toolchain.cmake_version = "4.1";
    report.build_toolchain.executable_path = "myrd-m2-e2e.exe";
    report.build_toolchain.executable_sha256 =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    report.build_toolchain.build_fingerprint =
        report.build_toolchain.executable_sha256;
    report.accepted_m1_baseline = {
        "m1-accepted.json",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        1, "GO", true};
    report.machine_os.machine_name = "test-machine";
    report.machine_os.os_name = "Windows";
    report.machine_os.os_version = "11 24H2";
    report.machine_os.architecture = "x64";
    report.machine_os.windows_build = "26100";
    report.machine_os.cpu_model = "Test CPU";
    report.machine_os.logical_processor_count = 16;
    report.machine_os.physical_memory_bytes = 32ULL << 30U;
    report.machine_os.machine_fingerprint =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    report.state_before.driver_state = "loaded";
    report.state_before.service_name = "deviosvc";
    report.state_before.service_state = "running";
    report.state_before.service_start_type = "automatic";
    report.state_before.driver_file_path = R"(C:\Windows\System32\drivers\phdskmnt.sys)";
    report.state_before.driver_file_version = "1.2.28.93";
    report.state_before.driver_sha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    report.state_after = report.state_before;
    report.geometry_options.disk_size_bytes = 1ULL << 40U;
    report.geometry_options.sector_size_bytes = 512;
    report.geometry_options.block_size_bytes = 64ULL << 10U;
    report.geometry_options.soft_limit_bytes = 512ULL << 20U;
    report.geometry_options.hard_limit_bytes = 1ULL << 30U;
    report.geometry_options.eviction_low_watermark_bytes = 384ULL << 20U;
    report.geometry_options.max_snapshot_batch_bytes = 64ULL << 20U;
    report.geometry_options.max_snapshot_batch_records = 1024;
    report.geometry_options.lock_stripe_count = 64;
    report.geometry_options.test_policy_selected = false;
    report.geometry_options.backing_kind = "fake";
    report.state.initial_state = myrd::M2InitialState::empty;
    report.state.initial_backing_generation = 0;
    report.state.current_backing_generation = 0;
    report.state.durable_successor_installed = false;
    report.state.durable_successor_generation_present = false;
    report.state.durable_successor_generation = 0;
    report.phases = passed_phases();
    report.io = {
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 (std::numeric_limits<std::uint64_t>::max)(), 1024, 512};
    report.store_stats_samples = {
        store_sample(myrd::M2StoreSamplePoint::ready, 0, 0),
        store_sample(myrd::M2StoreSamplePoint::after_write, 1ULL << 30U, 0),
        store_sample(myrd::M2StoreSamplePoint::after_retrim, 0, 1ULL << 30U),
        store_sample(myrd::M2StoreSamplePoint::final, 0, 1ULL << 30U)};
    report.store_stats_samples[0].captured_utc = "2026-08-04T01:02:30Z";
    report.store_stats_samples[1].captured_utc = "2026-08-04T01:02:31Z";
    report.store_stats_samples[2].captured_utc = "2026-08-04T01:02:40Z";
    report.store_stats_samples[3].captured_utc = "2026-08-04T01:02:41Z";
    report.os_memory_samples = {
        memory_sample(
            myrd::M2StoreSamplePoint::ready,
            myrd::M2PhaseId::start_provider,
            "2026-08-04T01:02:30Z", 0,
            64ULL << 20U, 64ULL << 20U),
        memory_sample(
            myrd::M2StoreSamplePoint::after_write,
            myrd::M2PhaseId::sample_after_write,
            "2026-08-04T01:02:31Z", 1'000,
            164ULL << 20U, 164ULL << 20U),
        memory_sample(
            myrd::M2StoreSamplePoint::after_retrim,
            myrd::M2PhaseId::sample_after_retrim,
            "2026-08-04T01:02:40Z", 10'000,
            74ULL << 20U, 74ULL << 20U),
        memory_sample(
            myrd::M2StoreSamplePoint::final,
            myrd::M2PhaseId::stop_provider,
            "2026-08-04T01:02:41Z", 11'000,
            64ULL << 20U, 64ULL << 20U)};
    report.performance.fixed = performance_sample(1'000'000, 10'000);
    report.performance.dynamic = performance_sample(700'000, 6'000);
    report.performance.comparison.sequential_ratio_basis_points = 7'000;
    report.performance.comparison.random_ratio_basis_points = 6'000;
    report.performance.comparison.sequential_pass = true;
    report.performance.comparison.random_pass = true;
    report.performance.comparison.overall_pass = true;
    report.performance.gate_status = myrd::M2PerformanceGateStatus::passed;
    report.device_identity.provider_session_id = "m2-session-0001";
    report.device_identity.native_object_path =
        R"(Global\myrd-m2-session-0001)";
    report.device_identity.device_id_raw = "197121";
    report.device_identity.path_id = 1;
    report.device_identity.target_id = 2;
    report.device_identity.lun = 3;
    report.device_identity.create_flags = myrd::aim::kM0CreateFlags;
    report.device_identity.query_flags = myrd::aim::kM1QueryFlags;
    report.device_identity.confirmed_by_query_device = true;
    report.device_identity.physical_drive_number = 42;
    report.device_identity.physical_drive_path = R"(\\.\PhysicalDrive42)";
    report.device_identity.disk_length_bytes = 1ULL << 40U;
    report.device_identity.bus_type = "SCSI";
    report.device_identity.vendor_id = "MYRD";
    report.device_identity.product_id = "DynamicDisk";
    report.device_identity.serial_number = "run-0123456789abcdef";
    report.device_identity.disk_unique_id = "m2-disk-0001";
    report.device_identity.volume_guids = {R"(\\?\Volume{m2-test}\)"};
    report.device_identity.mount_points = {R"(C:\m2-test\)"};
    report.cleanup_actions = succeeded_cleanup_actions();
    report.final_baseline_comparison.comparison =
        myrd::M2BaselineComparison::match;
    report.final_baseline_comparison.exact_match = true;
    report.final_baseline_comparison.resources_within_tolerance = true;
    report.final_baseline_comparison.private_bytes_tolerance = 1ULL << 20U;
    report.final_baseline_comparison.before_fingerprint =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    report.final_baseline_comparison.after_fingerprint =
        report.final_baseline_comparison.before_fingerprint;
    report.final_baseline_comparison.before = inventory_snapshot();
    report.final_baseline_comparison.after = inventory_snapshot();
    report.verdict = myrd::M2Verdict::go;
    return report;
}

void mark_performance_failed(myrd::M2Report& report) {
    report.performance.dynamic = performance_sample(500'000, 5'000);
    report.performance.comparison.sequential_ratio_basis_points = 5'000;
    report.performance.comparison.random_ratio_basis_points = 5'000;
    report.performance.comparison.sequential_pass = false;
    report.performance.comparison.random_pass = false;
    report.performance.comparison.overall_pass = false;
    report.performance.gate_status = myrd::M2PerformanceGateStatus::failed;
}

std::string replace_once(
    std::string input, std::string_view from, std::string_view to) {
    const auto position = input.find(from);
    REQUIRE(position != std::string::npos);
    input.replace(position, from.size(), to);
    return input;
}

std::string replace_last(
    std::string input, std::string_view from, std::string_view to) {
    const auto position = input.rfind(from);
    REQUIRE(position != std::string::npos);
    input.replace(position, from.size(), to);
    return input;
}

std::string omit_nth_key(
    std::string input, std::string_view key, std::size_t occurrence) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t position = 0;
    for (std::size_t index = 0; index <= occurrence; ++index) {
        position = input.find(needle, position);
        REQUIRE(position != std::string::npos);
        if (index != occurrence) {
            position += needle.size();
        }
    }
    input.replace(position + 1, key.size(), "omitted_" + std::string(key));
    return input;
}

void require_invalid(const std::string& json) {
    REQUIRE(myrd::validate_m2_report_json(json).has_error());
}

std::filesystem::path unique_test_directory() {
    const auto suffix = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("myrd-m2-report-test-" + suffix);
}

} // namespace

TEST_CASE(m2_report_enum_strings_are_stable_and_unknown_values_are_rejected) {
    REQUIRE_EQ(myrd::to_string(myrd::M2RunMode::fake), "FAKE");
    REQUIRE_EQ(myrd::to_string(myrd::M2RunMode::real), "REAL");
    REQUIRE_EQ(myrd::parse_m2_run_mode("REAL"), myrd::M2RunMode::real);
    REQUIRE_EQ(myrd::to_string(myrd::M2InitialState::empty), "EMPTY");
    REQUIRE_EQ(myrd::to_string(myrd::M2InitialState::seeded), "SEEDED");
    REQUIRE_EQ(myrd::to_string(myrd::M2StoreSamplePoint::ready), "READY");
    REQUIRE_EQ(myrd::to_string(myrd::M2StoreSamplePoint::after_write),
               "AFTER-WRITE");
    REQUIRE_EQ(myrd::to_string(myrd::M2StoreSamplePoint::after_retrim),
               "AFTER-RETRIM");
    REQUIRE_EQ(myrd::to_string(myrd::M2StoreSamplePoint::final), "FINAL");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseId::capture_baseline),
               "CAPTURE-BASELINE");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseId::publish_report),
               "PUBLISH-REPORT");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupActionId::remove_device),
               "REMOVE-DEVICE");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::not_run), "NOT-RUN");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::passed), "PASS");
    REQUIRE_EQ(myrd::to_string(myrd::M2PhaseStatus::failed), "FAIL");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::not_attempted),
               "NOT-ATTEMPTED");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::succeeded), "SUCCEEDED");
    REQUIRE_EQ(myrd::to_string(myrd::M2CleanupResult::failed), "FAILED");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::not_evaluated),
               "NOT-EVALUATED");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::passed), "PASS");
    REQUIRE_EQ(myrd::to_string(myrd::M2PerformanceGateStatus::failed), "FAIL");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::not_compared),
               "NOT-COMPARED");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::match), "MATCH");
    REQUIRE_EQ(myrd::to_string(myrd::M2BaselineComparison::mismatch),
               "MISMATCH");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::incomplete), "INCOMPLETE");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::go), "GO");
    REQUIRE_EQ(myrd::to_string(myrd::M2Verdict::no_go), "NO-GO");

    REQUIRE(!myrd::parse_m2_run_mode("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_initial_state("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_store_sample_point("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_phase_id("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_cleanup_action_id("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_phase_status("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_cleanup_result("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_performance_gate_status("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_baseline_comparison("UNKNOWN"));
    REQUIRE(!myrd::parse_m2_verdict("UNKNOWN"));
}

TEST_CASE(m2_report_serializes_and_schema_names_every_required_field) {
    const auto json = myrd::serialize_m2_report(complete_report());
    auto no_go_report = complete_report();
    mark_performance_failed(no_go_report);
    no_go_report.failures.push_back({"performance", 23, "threshold missed"});
    no_go_report.verdict = myrd::M2Verdict::no_go;
    const auto serialized_examples =
        json + myrd::serialize_m2_report(no_go_report);
    const auto schema = myrd::m2_report_json_schema();
    REQUIRE(!schema.empty());
    REQUIRE(schema.front() == '{');
    REQUIRE(schema.find("\"$schema\"") != std::string::npos);
    REQUIRE(schema.find("\"required\"") != std::string::npos);

    const std::vector<std::string_view> required_keys = {
        "schema_version", "run", "run_id", "mode", "started_utc",
        "ended_utc", "build_toolchain", "build_version", "git_commit",
        "configuration", "compiler", "compiler_version", "cmake_version",
        "executable_path", "executable_sha256", "build_fingerprint",
        "accepted_m1_baseline", "report_path", "report_sha256",
        "m1_schema_version", "m1_verdict", "accepted", "machine_os",
        "machine_name", "os_name", "os_version", "architecture",
        "windows_build", "cpu_model", "logical_processor_count",
        "physical_memory_bytes", "machine_fingerprint",
        "state_before", "state_after", "driver_state", "service_name",
        "service_state", "service_start_type", "driver_file_path",
        "driver_file_version", "driver_sha256", "geometry_options",
        "disk_size_bytes",
        "sector_size_bytes", "block_size_bytes", "soft_limit_bytes",
        "hard_limit_bytes", "eviction_low_watermark_bytes",
        "max_snapshot_batch_bytes", "max_snapshot_batch_records",
        "lock_stripe_count", "test_policy_selected", "backing_kind",
        "state", "initial_state", "initial_backing_generation",
        "current_backing_generation", "durable_successor_installed",
        "durable_successor_generation_present",
        "durable_successor_generation", "durability_scope_statement",
        "phases", "phase_id", "phase_started_utc", "phase_ended_utc",
        "phase_status", "phase_error", "phase_elapsed_ms", "io",
        "write_sha256", "read_sha256", "bytes_written", "bytes_read",
        "bytes_deleted", "store_stats_samples", "sample_point",
        "logical_size_bytes", "reserved_bytes", "committed_bytes",
        "resident_clean_blocks", "resident_dirty_blocks",
        "zero_clean_blocks", "zero_dirty_blocks", "loading_blocks",
        "evicted_blocks_total", "backing_reads_total",
        "backing_read_bytes_total", "uncached_reads_total",
        "unmap_bytes_total", "zero_bytes_total", "hard_limit_rejections",
        "commit_failures", "decommit_failures",
        "synchronous_pressure_passes_total",
        "synchronous_pressure_waits_total",
        "synchronous_pressure_no_progress_total",
        "synchronous_pressure_cancellations_total",
        "synchronous_pressure_deadlines_total",
        "commit_reservation_rollbacks_total", "snapshot_records_total",
        "snapshot_version_conflicts", "backing_generation",
        "backing_view_installs_total", "backing_generation_conflicts",
        "stale_backing_load_retries", "snapshot_active_id",
        "snapshot_active_state", "snapshot_cow_current_bytes",
        "snapshot_cow_peak_bytes", "snapshot_cow_records",
        "snapshot_blocked_writers", "snapshot_writer_stalls_total",
        "snapshot_auto_aborts_total", "snapshot_consistent_batches_total",
        "snapshot_consistent_records_total", "os_memory_samples", "captured_utc",
        "memory_sample_point", "memory_phase_id", "provider_process_id",
        "elapsed_ms", "working_set_bytes", "private_bytes", "commit_bytes",
        "performance", "fixed", "dynamic", "methodology_id", "operation",
        "queue_depth", "region_offset", "region_bytes",
        "sequential_io_bytes", "random_io_bytes", "random_seed",
        "sequential_warmup_operations", "random_warmup_operations",
        "policy", "machine_fingerprint", "build_fingerprint",
        "process_policy", "raw_io_flags", "sequential", "random",
        "operations", "bytes",
        "elapsed_ticks", "clock_frequency_hz", "bytes_per_second",
        "operations_per_second", "comparison",
        "sequential_ratio_basis_points", "random_ratio_basis_points",
        "sequential_threshold_basis_points", "random_threshold_basis_points",
        "sequential_pass", "random_pass", "overall_pass", "gate_status",
        "device_identity", "provider_session_id", "native_object_path",
        "device_id_raw", "path_id", "target_id", "lun", "create_flags",
        "query_flags", "confirmed_by_query_device", "physical_drive_number",
        "physical_drive_path", "disk_length_bytes", "bus_type", "vendor_id",
        "product_id", "serial_number", "disk_unique_id", "volume_guids",
        "mount_points", "cleanup_actions",
        "cleanup_action_id", "cleanup_attempted", "cleanup_result",
        "cleanup_error", "final_baseline_comparison", "comparison",
        "exact_match", "resources_within_tolerance",
        "private_bytes_tolerance", "before_fingerprint", "after_fingerprint",
        "before", "after", "diff", "physical_disks", "volumes",
        "m2_device_ids", "service_start_type", "process_handle_count",
        "process_thread_count", "process_private_bytes", "added", "removed",
        "changed", "failures",
        "failure_phase", "failure_code", "failure_message", "verdict"};

    for (const auto key : required_keys) {
        const std::string quoted = "\"" + std::string(key) + "\"";
        REQUIRE(serialized_examples.find(quoted) != std::string::npos);
        REQUIRE(schema.find(quoted) != std::string::npos);
    }
    REQUIRE(myrd::validate_m2_report_json(json).ok());
    REQUIRE(json.find("\"snapshot_active_state\": \"Inactive\"") !=
            std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(no_go_report)).ok());
}

TEST_CASE(m2_report_preserves_uint64_max_as_an_unquoted_decimal_integer) {
    const auto json = myrd::serialize_m2_report(complete_report());
    REQUIRE(json.find("\"bytes_written\": 18446744073709551615") !=
            std::string::npos);
    REQUIRE(json.find("\"18446744073709551615\"") == std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(json).ok());
}

TEST_CASE(m2_report_rejects_incoherent_or_unknown_snapshot_state) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json,
        "\"snapshot_active_state\": \"Inactive\"",
        "\"snapshot_active_state\": \"Active\""));
    require_invalid(replace_once(
        json,
        "\"snapshot_active_id\": 0",
        "\"snapshot_active_id\": 7"));
    require_invalid(replace_once(
        json,
        "\"snapshot_active_state\": \"Inactive\"",
        "\"snapshot_active_state\": \"Unknown\""));
}

TEST_CASE(m2_report_validator_identifies_the_first_invalid_section) {
    auto json = myrd::serialize_m2_report(complete_report());
    json = replace_once(
        std::move(json),
        "\"block_size_bytes\": 65536",
        "\"block_size_bytes\": 32768");

    const auto validation = myrd::validate_m2_report_json(json);

    REQUIRE(validation.has_error());
    REQUIRE(
        validation.error().message.find(L"geometry_options") !=
        std::wstring::npos);
}

TEST_CASE(m2_report_validator_rejects_each_missing_required_member) {
    const auto json = myrd::serialize_m2_report(complete_report());
    const std::vector<std::string_view> once = {
        "schema_version", "run", "run_id", "mode", "started_utc",
        "ended_utc", "build_toolchain", "build_version", "git_commit",
        "configuration", "compiler", "compiler_version", "cmake_version",
        "executable_path", "executable_sha256", "build_fingerprint",
        "accepted_m1_baseline", "report_path", "report_sha256",
        "m1_schema_version", "m1_verdict", "accepted", "machine_os",
        "machine_name", "os_name", "os_version", "architecture",
        "windows_build", "cpu_model", "logical_processor_count",
        "physical_memory_bytes", "machine_fingerprint",
        "state_before", "state_after", "geometry_options", "disk_size_bytes",
        "sector_size_bytes", "block_size_bytes", "soft_limit_bytes",
        "hard_limit_bytes", "eviction_low_watermark_bytes",
        "max_snapshot_batch_bytes", "max_snapshot_batch_records",
        "lock_stripe_count", "test_policy_selected", "backing_kind", "state",
        "initial_state", "initial_backing_generation",
        "current_backing_generation", "durable_successor_installed",
        "durable_successor_generation_present",
        "durable_successor_generation", "durability_scope_statement", "phases",
        "phase_id", "phase_started_utc", "phase_ended_utc", "phase_status",
        "phase_error", "phase_elapsed_ms", "io", "write_sha256",
        "read_sha256", "bytes_written", "bytes_read", "bytes_deleted",
        "store_stats_samples", "sample_point", "logical_size_bytes",
        "reserved_bytes", "committed_bytes", "resident_clean_blocks",
        "resident_dirty_blocks", "zero_clean_blocks", "zero_dirty_blocks",
        "loading_blocks", "evicted_blocks_total", "backing_reads_total",
        "backing_read_bytes_total", "uncached_reads_total",
        "unmap_bytes_total", "zero_bytes_total", "hard_limit_rejections",
        "commit_failures", "decommit_failures",
        "synchronous_pressure_passes_total",
        "synchronous_pressure_waits_total",
        "synchronous_pressure_no_progress_total",
        "synchronous_pressure_cancellations_total",
        "synchronous_pressure_deadlines_total",
        "commit_reservation_rollbacks_total", "snapshot_records_total",
        "snapshot_version_conflicts", "backing_generation",
        "backing_view_installs_total", "backing_generation_conflicts",
        "stale_backing_load_retries", "snapshot_active_id",
        "snapshot_active_state", "snapshot_cow_current_bytes",
        "snapshot_cow_peak_bytes", "snapshot_cow_records",
        "snapshot_blocked_writers", "snapshot_writer_stalls_total",
        "snapshot_auto_aborts_total", "snapshot_consistent_batches_total",
        "snapshot_consistent_records_total", "os_memory_samples",
        "captured_utc", "memory_sample_point", "memory_phase_id",
        "provider_process_id", "elapsed_ms", "working_set_bytes",
        "private_bytes", "commit_bytes",
        "performance", "methodology_id", "operation", "queue_depth",
        "region_offset", "region_bytes", "sequential_io_bytes",
        "random_io_bytes", "random_seed", "sequential_warmup_operations",
        "random_warmup_operations", "policy", "machine_fingerprint",
        "build_fingerprint", "process_policy", "raw_io_flags",
        "sequential", "random",
        "elapsed_ticks", "clock_frequency_hz", "bytes_per_second",
        "operations_per_second", "comparison",
        "sequential_ratio_basis_points", "random_ratio_basis_points",
        "sequential_threshold_basis_points", "random_threshold_basis_points",
        "sequential_pass", "random_pass", "overall_pass", "gate_status",
        "device_identity", "provider_session_id", "native_object_path",
        "device_id_raw", "path_id", "target_id", "lun", "create_flags",
        "query_flags", "confirmed_by_query_device", "physical_drive_number",
        "physical_drive_path", "disk_length_bytes", "bus_type", "vendor_id",
        "product_id", "serial_number", "disk_unique_id", "volume_guids",
        "mount_points", "cleanup_actions",
        "cleanup_action_id", "cleanup_attempted", "cleanup_result", "cleanup_error",
        "final_baseline_comparison", "comparison", "exact_match",
        "resources_within_tolerance", "private_bytes_tolerance",
        "before_fingerprint", "after_fingerprint", "before", "after", "diff",
        "physical_disks", "volumes", "m2_device_ids", "service_start_type",
        "process_handle_count", "process_thread_count", "process_private_bytes",
        "added", "removed", "changed", "failures", "verdict"};
    for (const auto key : once) {
        require_invalid(omit_nth_key(json, key, 0));
    }

    for (const auto key : {
             "driver_state", "service_name", "service_state",
             "service_start_type", "driver_file_path", "driver_file_version",
             "driver_sha256"}) {
        require_invalid(omit_nth_key(json, key, 0));
        require_invalid(omit_nth_key(json, key, 1));
    }
    for (const auto key : {"fixed", "dynamic", "operations", "bytes"}) {
        require_invalid(omit_nth_key(json, key, 0));
        if (key == std::string_view("operations") ||
            key == std::string_view("bytes")) {
            require_invalid(omit_nth_key(json, key, 1));
        }
    }

    auto no_go = complete_report();
    mark_performance_failed(no_go);
    no_go.failures.push_back({"performance", 23, "threshold missed"});
    no_go.verdict = myrd::M2Verdict::no_go;
    const auto no_go_json = myrd::serialize_m2_report(no_go);
    for (const auto key : {
             "failure_phase", "failure_code", "failure_message"}) {
        require_invalid(omit_nth_key(no_go_json, key, 0));
    }
}

TEST_CASE(m2_report_validator_rejects_malformed_duplicate_and_trailing_json) {
    require_invalid("{");
    require_invalid(myrd::serialize_m2_report(complete_report()) + " garbage");
    auto duplicate = myrd::serialize_m2_report(complete_report());
    duplicate = replace_once(duplicate, "\"schema_version\": 1",
                             "\"schema_version\": 1, \"schema_version\": 1");
    require_invalid(duplicate);
}

TEST_CASE(m2_report_validator_rejects_unknown_enums_and_bad_integer_spellings) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_last(
        json, "\"verdict\": \"GO\"", "\"verdict\": \"MAYBE\""));
    require_invalid(replace_once(
        json, "\"phase_status\": \"PASS\"",
        "\"phase_status\": \"UNKNOWN\""));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024", "\"bytes_read\": -1"));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024", "\"bytes_read\": \"1024\""));
    require_invalid(replace_once(
        json, "\"bytes_read\": 1024",
        "\"bytes_read\": 18446744073709551616"));
    require_invalid(replace_once(
        json, "\"schema_version\": 1", "\"schema_version\": 2"));
    require_invalid(replace_once(
        json, "\"schema_version\": 1",
        "\"schema_version\": 4294967296"));
}

TEST_CASE(m2_report_validator_rejects_every_inconsistent_go_verdict) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json, "\"phase_status\": \"PASS\"",
        "\"phase_status\": \"NOT-RUN\""));

    auto with_failure = complete_report();
    with_failure.failures.push_back({"fake-acceptance", 5, "failed"});
    require_invalid(myrd::serialize_m2_report(with_failure));

    require_invalid(replace_once(
        json, "\"cleanup_result\": \"SUCCEEDED\"",
        "\"cleanup_result\": \"FAILED\""));
    require_invalid(replace_once(
        json, "\"comparison\": \"MATCH\"",
        "\"comparison\": \"MISMATCH\""));
    require_invalid(replace_once(
        json, "\"gate_status\": \"PASS\"",
        "\"gate_status\": \"FAIL\""));
}

TEST_CASE(m2_report_validator_rejects_go_when_match_fingerprints_differ) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json,
        "\"after_fingerprint\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"",
        "\"after_fingerprint\": \"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\""));
}

TEST_CASE(m2_process_resource_policy_allows_release_but_rejects_growth) {
    const auto before = inventory_snapshot();
    auto after = before;
    after.process_handle_count -= 1;
    after.process_thread_count -= 2;
    after.process_private_bytes += 1ULL << 20U;
    REQUIRE(myrd::m2_process_resources_within_tolerance(
        before, after, 1ULL << 20U));

    after = before;
    after.process_handle_count += 1;
    REQUIRE(!myrd::m2_process_resources_within_tolerance(
        before, after, 1ULL << 20U));

    after = before;
    after.process_thread_count += 1;
    REQUIRE(!myrd::m2_process_resources_within_tolerance(
        before, after, 1ULL << 20U));

    after = before;
    after.process_private_bytes += (1ULL << 20U) + 1;
    REQUIRE(!myrd::m2_process_resources_within_tolerance(
        before, after, 1ULL << 20U));
}

TEST_CASE(m2_report_validator_accepts_resource_only_baseline_no_go) {
    auto report = complete_report();
    auto& comparison = report.final_baseline_comparison;
    comparison.comparison = myrd::M2BaselineComparison::mismatch;
    comparison.resources_within_tolerance = false;
    comparison.after.process_thread_count += 1;
    report.failures.push_back({
        "COMPARE-BASELINE", 4110,
        "final process resource baseline grew"});
    report.verdict = myrd::M2Verdict::no_go;

    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_cross_checks_performance_gate_and_ratio) {
    const auto json = myrd::serialize_m2_report(complete_report());
    require_invalid(replace_once(
        json, "\"sequential_ratio_basis_points\": 7000",
        "\"sequential_ratio_basis_points\": 6999"));
    require_invalid(replace_once(
        json, "\"random_threshold_basis_points\": 6000",
        "\"random_threshold_basis_points\": 6001"));
    require_invalid(replace_once(
        json,
        "\"machine_fingerprint\": \"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\"",
        "\"machine_fingerprint\": \"0000000000000000000000000000000000000000000000000000000000000000\""));
    require_invalid(replace_once(
        json, "\"methodology_id\": \"m2-raw-qd1-v1\"",
        "\"methodology_id\": \"noncanonical\""));
    require_invalid(replace_once(
        json, "\"overall_pass\": true", "\"overall_pass\": false"));

    auto different_flags = complete_report();
    different_flags.performance.dynamic.policy.raw_io_flags =
        "FILE_FLAG_NO_BUFFERING";
    require_invalid(myrd::serialize_m2_report(different_flags));

    auto failed_above_threshold = complete_report();
    failed_above_threshold.performance.gate_status =
        myrd::M2PerformanceGateStatus::failed;
    failed_above_threshold.failures.push_back(
        {"performance", 23, "threshold missed"});
    failed_above_threshold.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(failed_above_threshold));
}

TEST_CASE(m2_report_validator_cross_checks_store_samples_and_backing_state) {
    auto report = complete_report();
    std::swap(report.store_stats_samples[0], report.store_stats_samples[1]);
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.store_stats_samples.pop_back();
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.store_stats_samples[2].stats.backing_generation = 1;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.store_stats_samples[1].stats.committed_bytes =
        report.geometry_options.hard_limit_bytes + 1;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.state.current_backing_generation = 1;
    for (auto& sample : report.store_stats_samples) {
        sample.stats.backing_generation = 1;
    }
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.state.durable_successor_installed = true;
    report.state.durable_successor_generation_present = true;
    report.state.durable_successor_generation = 1;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_requires_exact_phase_and_cleanup_order) {
    auto report = complete_report();
    std::swap(report.phases[0], report.phases[1]);
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases[1].id = report.phases[0].id;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases.pop_back();
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    std::swap(report.cleanup_actions[0], report.cleanup_actions[1]);
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.cleanup_actions.pop_back();
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases[0].error = "success cannot carry an error";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.cleanup_actions[0].error = "success cannot carry an error";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases[0].status = myrd::M2PhaseStatus::not_run;
    report.phases[0].started_utc.clear();
    report.phases[0].ended_utc.clear();
    report.phases[0].elapsed_ms = 0;
    report.phases[0].error = "not-run cannot carry an error";
    report.failures.push_back({"test", 1, "failure"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.cleanup_actions[0] = {
        myrd::M2CleanupActionId::remove_mounts, false,
        myrd::M2CleanupResult::not_attempted,
        "not-attempted cannot carry an error"};
    report.failures.push_back({"test", 1, "failure"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_cross_checks_exact_device_identity) {
    auto report = complete_report();
    report.device_identity.device_id_raw = "197122";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.path_id = 2;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.query_flags ^= 1;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.confirmed_by_query_device = false;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.physical_drive_number = 0;
    report.device_identity.physical_drive_path = R"(\\.\PhysicalDrive0)";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.disk_length_bytes += 512;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.provider_session_id.clear();
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.volume_guids.clear();
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_accepts_valid_zero_scsi_address) {
    auto report = complete_report();
    report.device_identity.device_id_raw = "0";
    report.device_identity.path_id = 0;
    report.device_identity.target_id = 0;
    report.device_identity.lun = 0;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_accepts_only_coherent_staged_failure_identity) {
    auto report = complete_report();
    report.device_identity.query_flags = 0;
    report.device_identity.confirmed_by_query_device = false;
    report.device_identity.physical_drive_number = 0;
    report.device_identity.physical_drive_path.clear();
    report.device_identity.disk_length_bytes = 0;
    report.device_identity.bus_type.clear();
    report.device_identity.vendor_id.clear();
    report.device_identity.product_id.clear();
    report.device_identity.serial_number.clear();
    report.device_identity.disk_unique_id.clear();
    report.device_identity.volume_guids.clear();
    report.device_identity.mount_points.clear();
    report.failures.push_back(
        {"WAIT-FOR-DISK", 1, "disk discovery failed after CREATE"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());

    report = complete_report();
    report.device_identity.disk_unique_id.clear();
    report.device_identity.volume_guids.clear();
    report.device_identity.mount_points.clear();
    report.failures.push_back(
        {"WAIT-FOR-DISK", 1,
         "confirmed raw disk before GPT identity exists"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());

    report = complete_report();
    report.device_identity.volume_guids.clear();
    report.device_identity.mount_points.clear();
    report.failures.push_back(
        {"SETUP-WRITE", 1, "filesystem setup failed before mount"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());

    report = complete_report();
    report.device_identity.query_flags = 0;
    report.device_identity.confirmed_by_query_device = false;
    report.device_identity.physical_drive_number = 42;
    report.device_identity.physical_drive_path =
        R"(\\.\PhysicalDrive42)";
    report.failures.push_back(
        {"WAIT-FOR-DISK", 1, "mixed identity stage"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.device_identity.volume_guids.clear();
    report.failures.push_back(
        {"SETUP-WRITE", 1, "only one mount identity array present"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_cross_checks_structured_final_inventory) {
    auto report = complete_report();
    report.final_baseline_comparison.after.physical_disks.push_back("99");
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.final_baseline_comparison.exact_match = false;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.final_baseline_comparison.diff.added.push_back("PhysicalDrive99");
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.final_baseline_comparison.after.process_handle_count++;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.final_baseline_comparison.before.physical_disks = {"1", "0"};
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.final_baseline_comparison.comparison =
        myrd::M2BaselineComparison::mismatch;
    report.final_baseline_comparison.exact_match = false;
    report.final_baseline_comparison.after_fingerprint =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    report.final_baseline_comparison.diff.changed.push_back("service_state");
    report.failures.push_back({"compare-baseline", 55, "inventory differs"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_cross_checks_memory_sample_semantics) {
    auto report = complete_report();
    report.os_memory_samples.pop_back();
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    std::swap(report.os_memory_samples[0], report.os_memory_samples[1]);
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[1].phase_id = myrd::M2PhaseId::setup_write;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[2].provider_process_id++;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[2].captured_utc = "2026-08-04T01:02:39Z";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[2].elapsed_ms = 11'001;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[2].private_bytes = (74ULL << 20U) + 1;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.os_memory_samples[2].commit_bytes = (74ULL << 20U) + 1;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_identifies_the_memory_gate_that_failed) {
    auto report = complete_report();
    report.os_memory_samples[2].private_bytes = (74ULL << 20U) + 1;

    const auto validation = myrd::validate_m2_report_json(
        myrd::serialize_m2_report(report));

    REQUIRE(validation.has_error());
    REQUIRE(
        validation.error().message.find(
            L"os_memory_samples.retrim_private_bytes") !=
        std::wstring::npos);
}

TEST_CASE(m2_report_memory_gate_scales_with_observed_store_commit) {
    auto report = complete_report();
    report.geometry_options.soft_limit_bytes = 6ULL << 30U;
    report.geometry_options.hard_limit_bytes = 8ULL << 30U;
    report.geometry_options.eviction_low_watermark_bytes = 4ULL << 30U;
    report.store_stats_samples[1].stats.committed_bytes = 4ULL << 30U;
    report.store_stats_samples[2].stats.committed_bytes = 2ULL << 30U;
    report.store_stats_samples[3].stats.committed_bytes = 0;

    constexpr std::uint64_t ready = 64ULL << 20U;
    report.os_memory_samples[1].private_bytes = ready + (4ULL << 30U);
    report.os_memory_samples[1].commit_bytes = ready + (4ULL << 30U);
    report.os_memory_samples[2].private_bytes = ready + (2ULL << 30U);
    report.os_memory_samples[2].commit_bytes = ready + (2ULL << 30U);

    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_requires_matching_ordered_partial_sample_sets) {
    auto report = complete_report();
    report.store_stats_samples = {
        report.store_stats_samples[0], report.store_stats_samples[3]};
    report.os_memory_samples = {
        report.os_memory_samples[0], report.os_memory_samples[3]};
    report.failures.push_back(
        {"SAMPLE-AFTER-WRITE", 41, "sampling stopped early"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());

    report = complete_report();
    report.store_stats_samples = {
        report.store_stats_samples[0], report.store_stats_samples[3]};
    report.os_memory_samples = {
        report.os_memory_samples[0], report.os_memory_samples[2]};
    report.failures.push_back(
        {"SAMPLE-AFTER-WRITE", 42, "sample sets disagree"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    std::swap(report.store_stats_samples[0], report.store_stats_samples[3]);
    std::swap(report.os_memory_samples[0], report.os_memory_samples[3]);
    report.store_stats_samples.resize(2);
    report.os_memory_samples.resize(2);
    report.store_stats_samples[0].captured_utc = "2026-08-04T01:02:30Z";
    report.store_stats_samples[1].captured_utc = "2026-08-04T01:02:41Z";
    report.os_memory_samples[0].captured_utc = "2026-08-04T01:02:30Z";
    report.os_memory_samples[1].captured_utc = "2026-08-04T01:02:41Z";
    report.os_memory_samples[0].elapsed_ms = 0;
    report.os_memory_samples[1].elapsed_ms = 1'000;
    report.failures.push_back(
        {"SAMPLE-AFTER-WRITE", 43, "partial samples out of order"});
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_rejects_untrusted_environment_and_hash_evidence) {
    auto report = complete_report();
    report.build_toolchain.executable_sha256 = "short";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.accepted_m1_baseline.report_sha256 = "short";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.machine_os.machine_fingerprint = "short";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.state_after.service_state = "stopped";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.io.read_sha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.io.write_sha256 = "not-a-hash";
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_requires_ordered_rfc3339_utc_timestamps) {
    auto report = complete_report();
    report.run.started_utc = "yesterday";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.run.ended_utc = "2026-08-04T01:02:02Z";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases[0].started_utc = "2026-02-30T01:02:04Z";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.phases[0].ended_utc = "2026-08-04T01:02:03Z";
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    report.store_stats_samples[0].captured_utc = "2026-08-04 01:02:30";
    report.os_memory_samples[0].captured_utc =
        report.store_stats_samples[0].captured_utc;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_validator_enforces_real_geometry_and_zero_backing) {
    auto report = complete_report();
    report.run.mode = myrd::M2RunMode::real;
    report.geometry_options.disk_size_bytes = 64ULL << 30U;
    report.geometry_options.backing_kind = "zero";
    report.device_identity.disk_length_bytes = 64ULL << 30U;
    for (auto& sample : report.store_stats_samples) {
        sample.stats.logical_size_bytes = 64ULL << 30U;
        sample.stats.reserved_bytes = 64ULL << 30U;
    }
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());

    report.geometry_options.backing_kind = "fake";
    require_invalid(myrd::serialize_m2_report(report));

    report.geometry_options.backing_kind = "zero";
    report.geometry_options.disk_size_bytes = 32ULL << 30U;
    report.device_identity.disk_length_bytes = 32ULL << 30U;
    for (auto& sample : report.store_stats_samples) {
        sample.stats.logical_size_bytes = 32ULL << 30U;
        sample.stats.reserved_bytes = 32ULL << 30U;
    }
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_failure_forces_no_go_even_if_other_work_is_incomplete) {
    auto report = complete_report();
    report.phases.front().status = myrd::M2PhaseStatus::not_run;
    report.phases.front().started_utc.clear();
    report.phases.front().ended_utc.clear();
    report.phases.front().elapsed_ms = 0;
    report.cleanup_actions.front() = {
        myrd::M2CleanupActionId::remove_mounts, true,
        myrd::M2CleanupResult::failed,
        "cleanup failed"};
    report.failures.push_back({"cleanup", 31, "cleanup failed"});
    report.verdict = myrd::M2Verdict::no_go;
    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_accepts_consistent_incomplete_and_no_go_reports) {
    auto incomplete = complete_report();
    incomplete.phases.front().status = myrd::M2PhaseStatus::not_run;
    incomplete.phases.front().started_utc.clear();
    incomplete.phases.front().ended_utc.clear();
    incomplete.phases.front().elapsed_ms = 0;
    incomplete.cleanup_actions.front() = {
        myrd::M2CleanupActionId::remove_mounts, false,
        myrd::M2CleanupResult::not_attempted, ""};
    incomplete.performance.gate_status =
        myrd::M2PerformanceGateStatus::not_evaluated;
    incomplete.final_baseline_comparison.comparison =
        myrd::M2BaselineComparison::not_compared;
    incomplete.verdict = myrd::M2Verdict::incomplete;
    const auto incomplete_json = myrd::serialize_m2_report(incomplete);
    REQUIRE(incomplete_json.find("\"verdict\": \"INCOMPLETE\"") !=
            std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(incomplete_json).ok());

    auto no_go = complete_report();
    mark_performance_failed(no_go);
    no_go.failures.push_back({"performance", 23, "threshold missed"});
    no_go.verdict = myrd::M2Verdict::no_go;
    const auto no_go_json = myrd::serialize_m2_report(no_go);
    REQUIRE(no_go_json.find("\"verdict\": \"NO-GO\"") !=
            std::string::npos);
    REQUIRE(myrd::validate_m2_report_json(no_go_json).ok());
}

TEST_CASE(m2_report_validator_accepts_fail_closed_early_failure_evidence) {
    auto report = complete_report();
    report.state_before = {};
    report.state_after = {};
    report.phases[0].status = myrd::M2PhaseStatus::failed;
    report.phases[0].error = "baseline capture failed";
    for (std::size_t index = 1; index < report.phases.size(); ++index) {
        report.phases[index].started_utc.clear();
        report.phases[index].ended_utc.clear();
        report.phases[index].status = myrd::M2PhaseStatus::not_run;
        report.phases[index].elapsed_ms = 0;
    }
    report.io = {};
    report.store_stats_samples.clear();
    report.os_memory_samples.clear();
    report.performance = {};
    report.device_identity = {};
    report.cleanup_actions = {
        {myrd::M2CleanupActionId::remove_mounts, false,
         myrd::M2CleanupResult::not_attempted, ""},
        {myrd::M2CleanupActionId::remove_device, false,
         myrd::M2CleanupResult::not_attempted, ""},
        {myrd::M2CleanupActionId::stop_provider, false,
         myrd::M2CleanupResult::not_attempted, ""},
        {myrd::M2CleanupActionId::restore_service, false,
         myrd::M2CleanupResult::not_attempted, ""}};
    report.final_baseline_comparison = {};
    report.failures = {{"capture-baseline", 57, "baseline capture failed"}};
    report.verdict = myrd::M2Verdict::no_go;

    REQUIRE(myrd::validate_m2_report_json(
                myrd::serialize_m2_report(report)).ok());
}

TEST_CASE(m2_report_validator_requires_complete_failure_records) {
    auto report = complete_report();
    mark_performance_failed(report);
    report.failures = {{"", 23, "threshold missed"}};
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));

    report = complete_report();
    mark_performance_failed(report);
    report.failures = {{"performance", 23, ""}};
    report.verdict = myrd::M2Verdict::no_go;
    require_invalid(myrd::serialize_m2_report(report));
}

TEST_CASE(m2_report_paths_are_unique_and_require_an_existing_evidence_directory) {
    const auto root = unique_test_directory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    REQUIRE(!error);

    const auto first = myrd::make_unique_m2_report_path(root, "run/id");
    const auto second = myrd::make_unique_m2_report_path(root, "run/id");
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.value() != second.value());
    REQUIRE_EQ(first.value().parent_path(), root);
    REQUIRE_EQ(second.value().parent_path(), root);
    REQUIRE(first.value().extension() == ".json");
    REQUIRE(first.value().filename().wstring().find(L"run_id") !=
            std::wstring::npos);

    const auto missing = myrd::make_unique_m2_report_path(
        root / "does-not-exist", "run");
    REQUIRE(missing.has_error());
    std::filesystem::remove_all(root, error);
}

TEST_CASE(m2_report_publication_is_atomic_no_overwrite_and_valid_after_readback) {
    const auto root = unique_test_directory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    REQUIRE(!error);
    const auto path = myrd::make_unique_m2_report_path(root, "run");
    REQUIRE(path.ok());

    const auto first = myrd::publish_m2_report(complete_report(), path.value());
    REQUIRE(first.ok());
    REQUIRE(std::filesystem::exists(path.value(), error));
    REQUIRE(myrd::validate_published_m2_report(path.value()).ok());

    const auto second = myrd::publish_m2_report(complete_report(), path.value());
    REQUIRE(second.has_error());
    REQUIRE(myrd::validate_published_m2_report(path.value()).ok());

    std::ifstream input(path.value(), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    REQUIRE_EQ(content, myrd::serialize_m2_report(complete_report()));

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        REQUIRE(entry.path().filename().wstring().find(L".tmp-") ==
                std::wstring::npos);
    }
    std::filesystem::remove_all(root, error);
}
