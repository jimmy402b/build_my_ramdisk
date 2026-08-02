#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace myrd {

enum class M1FailurePoint : std::uint8_t {
    none,
    provider_ready,
    create_returned,
    device_confirmed,
    physical_drive_confirmed,
    write_verified,
    zero_verified,
    unmap_verified,
    device_removed
};

struct M1CliConfig {
    std::filesystem::path report_path;
    M1FailurePoint failure_point = M1FailurePoint::none;
};

Result<M1CliConfig> parse_m1_cli(
    std::span<const std::wstring_view> args, bool debug_build);

} // namespace myrd
