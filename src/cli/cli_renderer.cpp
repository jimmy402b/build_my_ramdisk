#include "src/cli/cli_renderer.h"

namespace myrd::cli {

Result<RenderOutput> render_json_success(
    const product::SuccessEnvelope& envelope) {
    auto document = product::write_success_envelope(envelope);
    if (!document.ok()) {
        return Result<RenderOutput>::failure(document.error());
    }
    return Result<RenderOutput>::success(RenderOutput{
        document.move_value(), {}, 0});
}

Result<RenderOutput> render_json_failure(
    int exit_code,
    const product::ErrorEnvelope& envelope) {
    auto document = product::write_error_envelope(envelope);
    if (!document.ok()) {
        return Result<RenderOutput>::failure(document.error());
    }
    return Result<RenderOutput>::success(RenderOutput{
        document.move_value(), {}, exit_code});
}

RenderOutput render_human_failure(int exit_code, std::string_view message) {
    return RenderOutput{
        {}, "Error (" + std::to_string(exit_code) + "): "
            + std::string(message) + "\n", exit_code};
}

} // namespace myrd::cli
