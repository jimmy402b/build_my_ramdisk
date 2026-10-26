#include "src/cli/cli_renderer.h"

#include "src/product/product_state.h"

#include <sstream>
#include <string_view>

namespace myrd::cli {

namespace {

std::string_view disposition_name(
    const management::ServiceResponseDisposition disposition) noexcept {
    switch (disposition) {
    case management::ServiceResponseDisposition::completed: return "completed";
    case management::ServiceResponseDisposition::operation_queued:
        return "operation queued";
    case management::ServiceResponseDisposition::dry_run: return "dry run";
    case management::ServiceResponseDisposition::wait_timeout: return "wait timed out";
    case management::ServiceResponseDisposition::client_wait_cancelled:
        return "wait cancelled";
    }
    return "unknown";
}

std::string_view lifecycle_name(const profile::ProfileLifecycle lifecycle) noexcept {
    switch (lifecycle) {
    case profile::ProfileLifecycle::provisioning: return "provisioning";
    case profile::ProfileLifecycle::ready: return "ready";
    }
    return "unknown";
}

std::string operation_line(const management::ServiceOperationProjection& operation) {
    std::ostringstream text;
    text << "operation " << operation.operation_id.to_string() << ": ";
    switch (operation.state) {
    case management::OperationState::queued: text << "queued"; break;
    case management::OperationState::running: text << "running"; break;
    case management::OperationState::reconciling: text << "reconciling"; break;
    case management::OperationState::succeeded: text << "succeeded"; break;
    case management::OperationState::failed: text << "failed"; break;
    case management::OperationState::cancelled: text << "cancelled"; break;
    }
    if (!operation.phase.empty()) text << " (phase " << operation.phase << ')';
    if (operation.cancel_requested) text << " [cancel requested]";
    return text.str();
}

std::string profile_line(const management::ServiceProfileProjection& profile) {
    std::ostringstream text;
    text << profile.profile_id.to_string() << "  "
         << (profile.mount_target.has_value() ? profile.mount_target.value()
                                              : std::string("(no mount)"))
         << "  " << lifecycle_name(profile.lifecycle) << "  "
         // The verdict, not the durable claim: see the header.
         << management::to_string(profile.aggregate) << "  runtime="
         << product::to_string(profile.runtime) << "  activity="
         << product::to_string(profile.activity) << "  persistence="
         << product::to_string(profile.persistence) << "  health="
         << product::to_string(profile.health) << "  generation="
         << profile.confirmed_generation
         // M5 §7: the memory the user came for.  `resident` is RAM the runtime holds,
         // `budget` is what it was opened with; both are the worker's own gauges, and
         // both are zero for a disk that is not running.
         << "  mem=" << profile.resident_bytes << '/' << profile.budget_bytes;
    return text.str();
}

} // namespace

std::string render_response(const management::ServiceResponseWire& response) {
    std::ostringstream text;
    if (response.error.has_value()) {
        const auto& error = response.error.value();
        // The wide message is the service's own words; narrowing it here is a display
        // choice, and the operation name is included because it says which step
        // refused, which the message alone often does not.
        std::string message;
        for (const wchar_t ch : error.message) {
            message.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        std::string operation;
        for (const wchar_t ch : error.operation) {
            operation.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        text << "error: " << message << '\n'
             << "  command: " << response.command << '\n'
             << "  step: " << (operation.empty() ? "unknown" : operation) << '\n'
             << "  code: 0x" << std::hex << error.code << std::dec
             << " domain " << static_cast<unsigned>(error.domain);
        if (error.native_code != 0) text << " native " << error.native_code;
        return text.str();
    }

    text << response.command << ": " << disposition_name(response.disposition);
    if (response.operation_id.has_value()) {
        text << '\n' << "operation " << response.operation_id->to_string();
    }
    if (response.operation.has_value()) {
        text << '\n' << operation_line(response.operation.value());
    }
    for (const auto warning : response.warnings) {
        text << '\n' << "warning: " << control::to_string(warning);
    }
    if (response.mount_target.has_value()) {
        text << '\n' << "mount: " << response.mount_target.value();
    }
    for (const auto& profile : response.profiles) {
        text << '\n' << profile_line(profile);
    }
    if (!response.profiles.empty()) {
        text << '\n' << response.profiles.size() << " disk(s)";
    }
    return text.str();
}

CliExitCode exit_code_for(const management::ServiceResponseWire& response) {
    if (response.error.has_value()) {
        const auto& error = response.error.value();
        const auto code = error.code;
        // The service's own classifications first, because they are the ones the
        // service chose deliberately; the domain is the fallback for anything that
        // did not come through `ServiceCore`.
        switch (static_cast<management::ServiceCoreError>(code)) {
        case management::ServiceCoreError::invalid_request:
        case management::ServiceCoreError::unsupported_command:
        case management::ServiceCoreError::wait_protocol_invalid:
            return CliExitCode::usage;
        case management::ServiceCoreError::authorization_denied:
            return CliExitCode::permission;
        case management::ServiceCoreError::profile_not_found:
        case management::ServiceCoreError::state_rejected:
        case management::ServiceCoreError::operation_invalid:
        case management::ServiceCoreError::dependency_failure:
            return CliExitCode::failure;
        }
        switch (static_cast<OperationErrorCode>(code)) {
        case OperationErrorCode::timed_out:
        case OperationErrorCode::cancelled:
            return CliExitCode::indeterminate;
        case OperationErrorCode::protocol_malformed:
        case OperationErrorCode::unknown_request:
        case OperationErrorCode::internal_invariant:
        case OperationErrorCode::version_changed:
        case OperationErrorCode::session_closed:
            // The exchange could not be carried out at all, which is what
            // "unavailable" means to a caller -- not that the product said no.
            return CliExitCode::unavailable;
        default:
            break;
        }
        if (error.code == 0x2203) return CliExitCode::indeterminate;
        return CliExitCode::failure;
    }

    if (response.operation.has_value()) {
        switch (response.operation->state) {
        case management::OperationState::failed:
        case management::OperationState::cancelled:
            // An operation that ended badly is a product failure, whether the client
            // waited for it or asked afterwards.
            return CliExitCode::failure;
        case management::OperationState::reconciling:
            // The service is still deciding whether a commit landed.  Saying
            // "succeeded" would claim something nobody knows yet, which is exactly the
            // rule "only a confirmed commit may be reported as success".
            return CliExitCode::indeterminate;
        case management::OperationState::queued:
        case management::OperationState::running:
        case management::OperationState::succeeded:
            break;
        }
    }
    if (response.disposition == management::ServiceResponseDisposition::wait_timeout
        || response.disposition
            == management::ServiceResponseDisposition::client_wait_cancelled) {
        return CliExitCode::indeterminate;
    }
    return CliExitCode::success;
}

} // namespace myrd::cli
