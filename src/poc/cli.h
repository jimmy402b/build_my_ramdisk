#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace myrd::poc {

enum class CommandMode : std::uint8_t { query_driver, run };

enum class FailurePoint : std::uint8_t {
    none, create_returned, device_confirmed,
    physical_drive_confirmed, round_trip_verified
};

struct CommandLine {
    CommandMode mode;
    std::filesystem::path report_path;
    FailurePoint injected_failure = FailurePoint::none;
};

Result<CommandLine> parse_command_line(
    std::span<const std::wstring_view> args, bool debug_build);

} // namespace myrd::poc
