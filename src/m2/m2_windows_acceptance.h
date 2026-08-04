#pragma once

#include "m2_cli.h"
#include "m2_provider.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace myrd {

Result<std::filesystem::path> m2_system_powershell_path();

bool m2_service_restoration_required(
    std::uint32_t current_state,
    std::uint32_t current_start_type,
    std::uint32_t baseline_state,
    std::uint32_t baseline_start_type) noexcept;

Result<std::string> normalize_m2_aim_descriptor_bus_type(
    std::uint32_t raw_bus_type,
    std::string_view vendor_id,
    std::string_view product_id);

Result<std::uint64_t> validate_m2_delete_retrim_state(
    std::string_view json);

Result<M2ProviderConfig> make_m2_real_provider_config(
    const M2GeometryOptions& geometry,
    const M2BackingState& backing);

Result<void> warm_m2_provider_runtime_for_acceptance(
    const M2GeometryOptions& geometry,
    const M2BackingState& backing);

struct M2RealRun {
    M2Report report;
    std::filesystem::path report_path;
};

Result<M2RealRun> finalize_m2_windows_acceptance(
    M2Report report,
    std::filesystem::path report_path);

Result<M2RealRun> run_m2_windows_acceptance(
    const M2CliConfig& config,
    const std::filesystem::path& executable_path);

} // namespace myrd
