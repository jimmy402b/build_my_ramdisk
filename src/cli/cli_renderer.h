#pragma once

#include "src/common/result.h"
#include "src/product/json_writer.h"

#include <string>
#include <string_view>

namespace myrd::cli {

struct RenderOutput {
    std::string stdout_text;
    std::string stderr_text;
    int exit_code{};
};

Result<RenderOutput> render_json_success(
    const product::SuccessEnvelope& envelope);
Result<RenderOutput> render_json_failure(
    int exit_code,
    const product::ErrorEnvelope& envelope);
RenderOutput render_human_failure(int exit_code, std::string_view message);

} // namespace myrd::cli
