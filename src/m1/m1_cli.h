#pragma once

#include <optional>
#include <string>

namespace myrd {

struct M1CliConfig {
    std::string command;         // "run"
    std::string report_path;     // required for run
    bool debug_failure_point = false;
};

std::optional<M1CliConfig> parse_m1_cli(int argc, wchar_t* argv[]);

} // namespace myrd
