#pragma once

#include "src/common/result.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace myrd::product {

struct CheckpointInterval {
    bool enabled;
    std::chrono::seconds duration;
};

Result<std::uint64_t> parse_binary_size(std::string_view text);
Result<CheckpointInterval> parse_checkpoint_interval(std::string_view text);
Result<std::wstring> normalize_local_absolute_path(std::wstring_view text);

} // namespace myrd::product
