#pragma once

#include "json_writer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myrd::poc {

struct EnvironmentEvidence {
    std::string windows_version;
    std::string windows_build;
    std::string architecture;
    bool elevated = false;
    std::string executable_path;
    std::string executable_sha256;
};

struct DependencyEvidence {
    std::vector<std::string> imported_modules;
    std::vector<std::string> loaded_modules;
    std::vector<std::string> forbidden_loaded_modules;
    std::vector<std::string> forbidden_processes;
    std::string deviosvc_state_during_run;
};

struct DriverEvidence {
    std::string service_name;
    std::string service_state;
    std::string file_path;
    std::string file_version;
    std::string sha256;
    std::string authenticode_status;
    std::string signer_subject;
    std::string adapter_interface_path;
    std::string query_version_raw;
};

struct BaselineEvidence {
    std::vector<std::string> aim_device_ids_before;
    std::vector<std::string> physical_drive_numbers_before;
    std::vector<std::string> aim_device_ids_after;
    std::vector<std::string> physical_drive_numbers_after;
};

struct ProviderEvidence {
    std::string session_id;
    std::string native_object_path;
    std::uint64_t mapping_size = 0;
    std::uint64_t data_area_size = 0;
    std::uint64_t info_count = 0;
    std::uint64_t read_count = 0;
    std::uint64_t write_count = 0;
    std::uint64_t close_count = 0;
    std::uint64_t fatal_protocol_error_count = 0;
};

struct CreatedDeviceEvidence {
    std::string device_id_raw;
    int path_id = 0;
    int target_id = 0;
    int lun = 0;
    std::uint32_t create_flags = 0;
    bool confirmed_by_query_device = false;
    int physical_drive_number = -1;
    std::string physical_drive_path;
    std::uint64_t disk_length = 0;
};

struct RawRoundTripEvidence {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string written_sha256;
    std::string read_sha256;
    bool flush_succeeded = false;
};

struct CleanupEvidence {
    bool exact_remove_attempted = false;
    bool exact_remove_succeeded = false;
    std::string removed_device_id;
    bool device_disappeared = false;
    bool provider_stopped = false;
    bool named_objects_released = false;
    bool aim_device_lists_match = false;
    bool physical_drive_lists_match = false;
    bool deviosvc_restored = false;
    bool deviosvc_start_mode_unchanged = false;
};

struct ErrorEvidence {
    std::string domain;
    std::uint32_t code = 0;
    std::string operation;
    std::string message;
};

struct PocReport {
    std::uint32_t schema_version = 1;
    std::string mode;
    std::string run_id;
    std::string started_utc;
    std::string finished_utc;
    bool success = false;
    int exit_code = 12;
    std::string last_stage = "started";
    EnvironmentEvidence environment;
    DependencyEvidence dependency_audit;
    DriverEvidence driver;
    BaselineEvidence baseline;
    ProviderEvidence provider;
    std::optional<CreatedDeviceEvidence> created_device;
    std::optional<RawRoundTripEvidence> raw_round_trip;
    CleanupEvidence cleanup;
    std::optional<ErrorEvidence> primary_error;
    std::vector<ErrorEvidence> cleanup_errors;
};

std::string serialize_report(const PocReport& report);

} // namespace myrd::poc
