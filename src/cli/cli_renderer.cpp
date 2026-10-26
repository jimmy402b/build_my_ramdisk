#include "src/cli/cli_renderer.h"

#include "src/product/product_state.h"

#include <sstream>
#include <string_view>

namespace myrd::cli {

namespace {

std::string_view disposition_name(
    const product::ServiceDisposition disposition) noexcept {
    switch (disposition) {
    case product::ServiceDisposition::completed: return "completed";
    case product::ServiceDisposition::operation_queued:
        return "operation queued";
    case product::ServiceDisposition::dry_run: return "dry run";
    case product::ServiceDisposition::wait_timeout: return "wait timed out";
    case product::ServiceDisposition::client_wait_cancelled:
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

std::string operation_line(const product::ServiceOperationView& operation) {
    std::ostringstream text;
    text << "operation " << operation.operation_id.to_string() << ": ";
    switch (operation.state) {
    case product::ServiceOperationState::queued: text << "queued"; break;
    case product::ServiceOperationState::running: text << "running"; break;
    case product::ServiceOperationState::reconciling: text << "reconciling"; break;
    case product::ServiceOperationState::succeeded: text << "succeeded"; break;
    case product::ServiceOperationState::failed: text << "failed"; break;
    case product::ServiceOperationState::cancelled: text << "cancelled"; break;
    }
    if (!operation.phase.empty()) text << " (phase " << operation.phase << ')';
    if (operation.cancel_requested) text << " [cancel requested]";
    return text.str();
}

std::string profile_line(const product::ServiceDiskView& profile) {
    std::ostringstream text;
    text << profile.profile_id.to_string() << "  "
         << (profile.mount_target.has_value() ? profile.mount_target.value()
                                              : std::string("(no mount)"))
         << "  " << product::to_string(profile.lifecycle) << "  "
         // The verdict, not the durable claim: see the header.
         << product::to_string(profile.aggregate) << "  runtime="
         << product::to_string(profile.runtime) << "  activity="
         << product::to_string(profile.activity) << "  persistence="
         << product::to_string(profile.persistence) << "  health="
         << product::to_string(profile.health) << "  generation="
         << profile.confirmed_generation
         // M5 搂7: the memory the user came for.  `resident` is RAM the runtime holds,
         // `budget` is what it was opened with; both are the worker's own gauges, and
         // both are zero for a disk that is not running.
         << "  mem=" << profile.resident_bytes << '/' << profile.budget_bytes;
    return text.str();
}

} // namespace

std::string render_response(const client::ServiceResponseWire& response) {
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

CliExitCode exit_code_for(const client::ServiceResponseWire& response) {
    if (response.error.has_value()) {
        const auto& error = response.error.value();
        const auto code = error.code;
        // The service's own classifications first, because they are the ones the
        // service chose deliberately; the domain is the fallback for anything that
        // did not come through `ServiceCore`.
        switch (static_cast<product::ServiceCoreError>(code)) {
        case product::ServiceCoreError::invalid_request:
        case product::ServiceCoreError::unsupported_command:
        case product::ServiceCoreError::wait_protocol_invalid:
            return CliExitCode::usage;
        case product::ServiceCoreError::authorization_denied:
            return CliExitCode::permission;
        case product::ServiceCoreError::profile_not_found:
        case product::ServiceCoreError::state_rejected:
        case product::ServiceCoreError::operation_invalid:
        case product::ServiceCoreError::dependency_failure:
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
        case product::ServiceOperationState::failed:
        case product::ServiceOperationState::cancelled:
            // An operation that ended badly is a product failure, whether the client
            // waited for it or asked afterwards.
            return CliExitCode::failure;
        case product::ServiceOperationState::reconciling:
            // The service is still deciding whether a commit landed.  Saying
            // "succeeded" would claim something nobody knows yet, which is exactly the
            // rule "only a confirmed commit may be reported as success".
            return CliExitCode::indeterminate;
        case product::ServiceOperationState::queued:
        case product::ServiceOperationState::running:
        case product::ServiceOperationState::succeeded:
            break;
        }
    }
    if (response.disposition == product::ServiceDisposition::wait_timeout
        || response.disposition
            == product::ServiceDisposition::client_wait_cancelled) {
        return CliExitCode::indeterminate;
    }
    return CliExitCode::success;
}

} // namespace myrd::cli
