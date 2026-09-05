#pragma once

#include "src/common/result.h"
#include "src/product/json_writer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace myrd::setup {

enum class DriverArchitecture : std::uint8_t {
    x64,
    x86,
    arm64,
    unknown,
};

enum class DriverServiceState : std::uint8_t {
    absent,
    stopped,
    running,
    transitional,
    query_failed,
};

enum class DriverStatus : std::uint8_t {
    absent,
    stopped,
    running_compatible,
    version_mismatch,
    sys_hash_mismatch,
    signature_mismatch,
    architecture_mismatch,
    inspection_failure,
    busy,
};

struct DriverManifest {
    std::string_view upstream_commit;
    std::string_view archive_url;
    std::string_view archive_sha256;
    std::string_view x64_sys_sha256;
    std::string_view driver_version;
    DriverArchitecture architecture;
    std::string_view service_name;
};

struct DriverStatusResult {
    DriverStatus status{DriverStatus::inspection_failure};
    std::optional<std::string> installed_version;
    std::optional<std::string> sys_path;
    std::optional<std::string> sys_hash;
    std::optional<bool> signature_valid;
    std::optional<std::uint32_t> signature_trust_status;
    std::optional<std::string> architecture;
};

struct DriverObservation {
    DriverServiceState service_state{DriverServiceState::query_failed};
    std::optional<std::string> installed_version;
    std::optional<std::string> sys_path;
    std::optional<std::string> sys_hash;
    std::optional<bool> signature_valid;
    std::optional<std::uint32_t> signature_trust_status;
    std::optional<DriverArchitecture> architecture;
};

const DriverManifest& driver_manifest() noexcept;

Result<std::string> parse_sha256_hex(std::string_view input);

DriverStatusResult classify_driver_status(
    const DriverManifest& manifest,
    const DriverObservation& observation);

DriverStatusResult inspect_installed_driver(const DriverManifest& manifest);

std::string_view to_string(DriverStatus status) noexcept;

bool is_valid(DriverStatus status) noexcept;

product::JsonObject driver_status_to_json(
    const DriverStatusResult& result,
    bool include_paths = false);

} // namespace myrd::setup
