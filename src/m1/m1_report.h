#pragma once

#include "src/proxy/request_processor.h"
#include "src/proxy/shm_proxy_session.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myrd {

struct M1ProviderEvidence {
    std::uint64_t info_count = 0;
    std::uint64_t read_count = 0;
    std::uint64_t write_count = 0;
    std::uint64_t unmap_count = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t close_count = 0;
    std::uint64_t fatal_protocol_error_count = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t timed_out = 0;
    std::string native_object_path;
    std::string final_state;
};

struct M1CleanupEvidence {
    bool exact_remove_succeeded = false;
    bool device_disappeared = false;
    bool provider_stopped = false;
    bool named_objects_released = false;
    bool aim_device_lists_match = false;
    bool physical_drive_lists_match = false;
};

struct M1RawProtocolEvidenceReport {
    bool zero_write_verified = false;
    bool unmap_verified = false;
};

struct M1Report {
    int schema_version = 1;
    std::string mode;
    std::string run_id;
    std::string started_utc;
    std::string finished_utc;
    bool success = false;
    int exit_code = 0;
    std::string last_stage;
    M1ProviderEvidence provider;
    M1RawProtocolEvidenceReport raw_protocol;
    M1CleanupEvidence cleanup;
    std::optional<std::string> primary_error;
    std::vector<std::string> cleanup_errors;
};

} // namespace myrd
