#pragma once

#include "src/driver/aim_protocol.h"
#include "src/proxy/request_processor.h"
#include "src/proxy/shm_proxy_session.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myrd {

struct M1ErrorEvidence {
    std::string domain;
    std::uint32_t code = 0;
    std::uint32_t native_code = 0;
    std::string operation;
    std::string message;
};

struct M1DiagnosticEvidence {
    std::uint64_t sequence = 0;
    std::uint64_t request_code = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string operation;
    M1ErrorEvidence error;
};

struct M1DriverEvidence {
    std::string service_name;
    std::string service_state;
    std::string file_path;
    std::string file_version;
    std::string sha256;
    std::string authenticode_status;
    std::string signer_subject;
    std::string adapter_interface_path;
    std::uint32_t api_version = 0;
    std::uint32_t sub_version = 0;
    std::string query_version_raw;
    std::vector<std::string> imported_modules;
    std::vector<std::string> loaded_modules;
    std::vector<std::string> forbidden_loaded_modules;
    std::vector<std::string> forbidden_processes;
};

struct M1BaselineEvidence {
    std::vector<std::string> aim_device_ids_before;
    std::vector<std::string> physical_drive_numbers_before;
    std::vector<std::string> aim_device_ids_after;
    std::vector<std::string> physical_drive_numbers_after;
};

struct M1ProviderEvidence {
    std::uint64_t info_count = 0;
    std::uint64_t read_count = 0;
    std::uint64_t write_count = 0;
    std::uint64_t unmap_count = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t close_count = 0;
    std::uint64_t info_error_count = 0;
    std::uint64_t read_error_count = 0;
    std::uint64_t write_error_count = 0;
    std::uint64_t unmap_error_count = 0;
    std::uint64_t zero_error_count = 0;
    std::uint64_t fatal_protocol_error_count = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t timed_out = 0;
    std::string native_object_path;
    std::string final_state;
    std::optional<M1ErrorEvidence> last_error;
    std::vector<M1DiagnosticEvidence> diagnostics;
};

struct M1CreatedDeviceEvidence {
    std::string device_id_raw;
    std::uint8_t path_id = 0;
    std::uint8_t target_id = 0;
    std::uint8_t lun = 0;
    std::uint32_t create_flags = 0;
    bool confirmed_by_query_device = false;
    std::uint32_t physical_drive_number = 0;
    std::string physical_drive_path;
    std::uint64_t disk_length = 0;
};

struct M1RawProtocolEvidenceReport {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string write_sha256;
    std::string read_sha256;
    bool zero_write_verified = false;
    bool unmap_verified = false;
    bool flush_succeeded = false;
};

struct M1CleanupEvidence {
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

struct M1Report {
    std::uint32_t schema_version = 1;
    std::string mode;
    std::string run_id;
    std::string started_utc;
    std::string finished_utc;
    bool success = false;
    int exit_code = 1;
    std::string last_stage = "started";
    M1DriverEvidence driver;
    M1BaselineEvidence baseline;
    M1ProviderEvidence provider;
    std::optional<M1CreatedDeviceEvidence> created_device;
    std::optional<M1RawProtocolEvidenceReport> raw_protocol;
    M1CleanupEvidence cleanup;
    std::optional<M1ErrorEvidence> primary_error;
    std::vector<M1ErrorEvidence> cleanup_errors;
};

std::string serialize_m1_report(const M1Report& report);

} // namespace myrd
