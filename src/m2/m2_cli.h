#pragma once

#include "m2_report.h"
#include "src/common/result.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myrd {

enum class M2CliMode : std::uint8_t {
    help,
    print_schema,
    validate_report,
    local_fake,
    real,
};

struct M2CliConfig {
    M2CliMode mode{M2CliMode::help};
    std::filesystem::path evidence_directory;
    std::filesystem::path report_to_validate;
    std::filesystem::path accepted_m1_report;
    std::string expected_driver_service;
    std::string expected_driver_sha256;
    std::string expected_bus_type;
    std::string expected_vendor_id;
    std::string expected_product_id;
    M2GeometryOptions geometry;
    std::optional<M2PhaseId> injected_failure_after;
    bool safety_acknowledged{};
};

Result<M2CliConfig> parse_m2_cli(
    std::span<const std::wstring_view> args);

Result<DynamicBlockStoreOptions> derive_m2_test_policy_limits(
    std::uint64_t physical_memory_bytes);

std::wstring_view m2_cli_usage() noexcept;

} // namespace myrd
