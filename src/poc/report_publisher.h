#pragma once

#include "src/common/result.h"

#include <filesystem>
#include <string_view>

namespace myrd::poc {

Result<void> publish_report_atomic(
    std::string_view json,
    const std::filesystem::path& final_path);

} // namespace myrd::poc
