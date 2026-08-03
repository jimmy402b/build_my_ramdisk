#pragma once

#include "m2_performance_probe.h"
#include "src/common/result.h"
#include "src/storage/dynamic_block_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myrd {

inline constexpr std::uint32_t kM2ReportSchemaVersion = 1;
inline constexpr std::string_view kM2NoDurableSuccessorStatement =
    "M2 has no durable backing successor; all backing generations end with this run.";

enum class M2RunMode : std::uint8_t { fake, real };
enum class M2InitialState : std::uint8_t { empty, seeded };
enum class M2StoreSamplePoint : std::uint8_t {
    ready,
    after_write,
    after_retrim,
    final
};
enum class M2PhaseId : std::uint8_t {
    capture_baseline,
    construct_provider,
    start_provider,
    create_device,
    wait_for_disk,
    setup_write,
    sample_after_write,
    delete_retrim,
    sample_after_retrim,
    performance,
    remove_mounts,
    remove_device,
    stop_provider,
    restore_service,
    compare_baseline,
    publish_report
};
enum class M2CleanupActionId : std::uint8_t {
    remove_mounts,
    remove_device,
    stop_provider,
    restore_service
};
enum class M2PhaseStatus : std::uint8_t { not_run, passed, failed };
enum class M2CleanupResult : std::uint8_t {
    not_attempted,
    succeeded,
    failed
};
enum class M2PerformanceGateStatus : std::uint8_t {
    not_evaluated,
    passed,
    failed
};
enum class M2BaselineComparison : std::uint8_t {
    not_compared,
    match,
    mismatch
};
enum class M2Verdict : std::uint8_t { incomplete, go, no_go };

std::string_view to_string(M2RunMode value) noexcept;
std::string_view to_string(M2InitialState value) noexcept;
std::string_view to_string(M2StoreSamplePoint value) noexcept;
std::string_view to_string(M2PhaseId value) noexcept;
std::string_view to_string(M2CleanupActionId value) noexcept;
std::string_view to_string(M2PhaseStatus value) noexcept;
std::string_view to_string(M2CleanupResult value) noexcept;
std::string_view to_string(M2PerformanceGateStatus value) noexcept;
std::string_view to_string(M2BaselineComparison value) noexcept;
std::string_view to_string(M2Verdict value) noexcept;

std::optional<M2RunMode> parse_m2_run_mode(std::string_view value) noexcept;
std::optional<M2InitialState> parse_m2_initial_state(
    std::string_view value) noexcept;
std::optional<M2StoreSamplePoint> parse_m2_store_sample_point(
    std::string_view value) noexcept;
std::optional<M2PhaseId> parse_m2_phase_id(std::string_view value) noexcept;
std::optional<M2CleanupActionId> parse_m2_cleanup_action_id(
    std::string_view value) noexcept;
std::optional<M2PhaseStatus> parse_m2_phase_status(
    std::string_view value) noexcept;
std::optional<M2CleanupResult> parse_m2_cleanup_result(
    std::string_view value) noexcept;
std::optional<M2PerformanceGateStatus> parse_m2_performance_gate_status(
    std::string_view value) noexcept;
std::optional<M2BaselineComparison> parse_m2_baseline_comparison(
    std::string_view value) noexcept;
std::optional<M2Verdict> parse_m2_verdict(std::string_view value) noexcept;

struct M2RunIdentity {
    std::string run_id;
    M2RunMode mode = M2RunMode::fake;
    std::string started_utc;
    std::string ended_utc;
};

struct M2BuildToolchain {
    std::string build_version;
    std::string git_commit;
    std::string configuration;
    std::string compiler;
    std::string compiler_version;
    std::string cmake_version;
    std::string executable_path;
    std::string executable_sha256;
    std::string build_fingerprint;
};

struct M2AcceptedM1Baseline {
    std::string report_path;
    std::string report_sha256;
    std::uint64_t m1_schema_version = 0;
    std::string m1_verdict;
    bool accepted = false;
};

struct M2MachineOs {
    std::string machine_name;
    std::string os_name;
    std::string os_version;
    std::string architecture;
    std::string windows_build;
    std::string cpu_model;
    std::uint64_t logical_processor_count = 0;
    std::uint64_t physical_memory_bytes = 0;
    std::string machine_fingerprint;
};

struct M2DriverServiceState {
    std::string driver_state;
    std::string service_name;
    std::string service_state;
    std::string service_start_type;
    std::string driver_file_path;
    std::string driver_file_version;
    std::string driver_sha256;
};

struct M2GeometryOptions {
    std::uint64_t disk_size_bytes = 0;
    std::uint64_t sector_size_bytes = 0;
    std::uint64_t block_size_bytes = 0;
    std::uint64_t soft_limit_bytes = 0;
    std::uint64_t hard_limit_bytes = 0;
    std::uint64_t eviction_low_watermark_bytes = 0;
    std::uint64_t max_snapshot_batch_bytes = 0;
    std::uint64_t max_snapshot_batch_records = 0;
    std::uint64_t lock_stripe_count = 0;
    bool test_policy_selected = false;
    std::string backing_kind;
};

struct M2BackingState {
    M2InitialState initial_state = M2InitialState::empty;
    std::uint64_t initial_backing_generation = 0;
    std::uint64_t current_backing_generation = 0;
    bool durable_successor_installed = false;
    bool durable_successor_generation_present = false;
    std::uint64_t durable_successor_generation = 0;
    std::string durability_scope_statement =
        std::string(kM2NoDurableSuccessorStatement);
};

struct M2Phase {
    M2PhaseId id = M2PhaseId::capture_baseline;
    std::string started_utc;
    std::string ended_utc;
    M2PhaseStatus status = M2PhaseStatus::not_run;
    std::string error;
    std::uint64_t elapsed_ms = 0;
};

struct M2IoEvidence {
    std::string write_sha256;
    std::string read_sha256;
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_deleted = 0;
};

struct M2StoreStatsSample {
    M2StoreSamplePoint point = M2StoreSamplePoint::ready;
    std::string captured_utc;
    DynamicBlockStoreStats stats;
};

struct M2OsMemorySample {
    M2StoreSamplePoint point = M2StoreSamplePoint::ready;
    M2PhaseId phase_id = M2PhaseId::start_provider;
    std::string captured_utc;
    std::uint64_t provider_process_id = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t working_set_bytes = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t commit_bytes = 0;
};

struct M2PerformanceEvidence {
    M2PerformanceProbeResult fixed;
    M2PerformanceProbeResult dynamic;
    M2PerformanceComparison comparison;
    M2PerformanceGateStatus gate_status =
        M2PerformanceGateStatus::not_evaluated;
};

struct M2DeviceIdentity {
    std::string provider_session_id;
    std::string native_object_path;
    std::string device_id_raw;
    std::uint64_t path_id = 0;
    std::uint64_t target_id = 0;
    std::uint64_t lun = 0;
    std::uint64_t create_flags = 0;
    std::uint64_t query_flags = 0;
    bool confirmed_by_query_device = false;
    std::uint64_t physical_drive_number = 0;
    std::string physical_drive_path;
    std::uint64_t disk_length_bytes = 0;
    std::string bus_type;
    std::string vendor_id;
    std::string product_id;
    std::string serial_number;
    std::string disk_unique_id;
    std::vector<std::string> volume_guids;
    std::vector<std::string> mount_points;
};

struct M2CleanupAction {
    M2CleanupActionId id = M2CleanupActionId::remove_mounts;
    bool attempted = false;
    M2CleanupResult result = M2CleanupResult::not_attempted;
    std::string error;
};

struct M2InventorySnapshot {
    std::vector<std::string> physical_disks;
    std::vector<std::string> volumes;
    std::vector<std::string> mount_points;
    std::vector<std::string> m2_device_ids;
    std::string service_state;
    std::string service_start_type;
    std::uint64_t process_handle_count = 0;
    std::uint64_t process_thread_count = 0;
    std::uint64_t process_private_bytes = 0;
};

struct M2InventoryDiff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed;
};

struct M2FinalBaselineComparison {
    M2BaselineComparison comparison = M2BaselineComparison::not_compared;
    bool exact_match = false;
    bool resources_within_tolerance = false;
    std::uint64_t private_bytes_tolerance = 0;
    std::string before_fingerprint;
    std::string after_fingerprint;
    M2InventorySnapshot before;
    M2InventorySnapshot after;
    M2InventoryDiff diff;
};

struct M2Failure {
    std::string phase;
    std::uint64_t code = 0;
    std::string message;
};

struct M2Report {
    std::uint32_t schema_version = kM2ReportSchemaVersion;
    M2RunIdentity run;
    M2BuildToolchain build_toolchain;
    M2AcceptedM1Baseline accepted_m1_baseline;
    M2MachineOs machine_os;
    M2DriverServiceState state_before;
    M2DriverServiceState state_after;
    M2GeometryOptions geometry_options;
    M2BackingState state;
    std::vector<M2Phase> phases;
    M2IoEvidence io;
    std::vector<M2StoreStatsSample> store_stats_samples;
    std::vector<M2OsMemorySample> os_memory_samples;
    M2PerformanceEvidence performance;
    M2DeviceIdentity device_identity;
    std::vector<M2CleanupAction> cleanup_actions;
    M2FinalBaselineComparison final_baseline_comparison;
    std::vector<M2Failure> failures;
    M2Verdict verdict = M2Verdict::incomplete;
};

std::string serialize_m2_report(const M2Report& report);
std::string m2_report_json_schema();
Result<void> validate_m2_report_json(std::string_view json);
Result<std::filesystem::path> make_unique_m2_report_path(
    const std::filesystem::path& evidence_directory,
    std::string_view run_id);
Result<void> publish_m2_report(
    const M2Report& report,
    const std::filesystem::path& final_path);
Result<void> validate_published_m2_report(
    const std::filesystem::path& final_path);

} // namespace myrd
