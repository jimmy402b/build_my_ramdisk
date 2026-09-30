#include "product_error.h"

#include <windows.h>

#include <climits>

namespace myrd::product {

namespace {

constexpr std::array<ErrorDefinition, 31> kErrorCatalog = {{
    {StableErrorCode::usage_error,                "UsageError",                ErrorCategory::usage,                std::nullopt,                      "Error",   false, "Check command-line arguments and retry"},
    {StableErrorCode::not_found,                  "NotFound",                  ErrorCategory::not_found,             std::nullopt,                      "Error",   false, "Verify the requested resource exists"},
    {StableErrorCode::request_id_conflict,        "RequestIdConflict",         ErrorCategory::conflict,              std::nullopt,                      "Error",   false, "Use a unique request identifier and retry"},
    {StableErrorCode::operation_conflict,         "OperationConflict",         ErrorCategory::conflict,              std::nullopt,                      "Error",   true,  "Retry the operation or resolve the conflict"},
    {StableErrorCode::permission_denied,          "PermissionDenied",          ErrorCategory::permission,            std::nullopt,                      "Error",   false, "Verify access rights and permissions"},
    {StableErrorCode::service_unavailable,        "ServiceUnavailable",        ErrorCategory::unavailable,           OperationalClass::transient,       "Warning", true,  "Retry the operation later"},
    {StableErrorCode::protocol_incompatible,      "ProtocolIncompatible",      ErrorCategory::incompatible,          std::nullopt,                      "Error",   false, "Update client to a compatible protocol version"},
    {StableErrorCode::driver_unavailable,         "DriverUnavailable",         ErrorCategory::unavailable,           OperationalClass::transient,       "Warning", true,  "Verify driver installation and retry"},
    {StableErrorCode::driver_mismatch,            "DriverMismatch",            ErrorCategory::incompatible,          OperationalClass::needs_attention, "Error",   false, "Install the matching driver version"},
    {StableErrorCode::driver_busy,                "DriverBusy",                ErrorCategory::transient,             OperationalClass::transient,       "Warning", true,  "Wait for the driver to become available"},
    {StableErrorCode::adapter_unavailable,        "AdapterUnavailable",        ErrorCategory::unavailable,           OperationalClass::transient,       "Warning", true,  "Verify adapter availability and retry"},
    {StableErrorCode::device_identity_mismatch,   "DeviceIdentityMismatch",    ErrorCategory::integrity,             OperationalClass::needs_attention, "Error",   false, "Verify device identity and configuration"},
    {StableErrorCode::profile_corrupt,            "ProfileCorrupt",            ErrorCategory::integrity,             OperationalClass::needs_attention, "Error",   false, "Restore profile from backup or recreate"},
    {StableErrorCode::profile_revision_conflict,  "ProfileRevisionConflict",   ErrorCategory::conflict,              std::nullopt,                      "Error",   true,  "Resolve profile revision conflict"},
    {StableErrorCode::bundle_locked,              "BundleLocked",              ErrorCategory::conflict,              OperationalClass::transient,       "Warning", true,  "Wait for the bundle lock to be released"},
    {StableErrorCode::bundle_corrupt,             "BundleCorrupt",             ErrorCategory::integrity,             OperationalClass::needs_attention, "Error",   false, "Restore bundle from backup"},
    {StableErrorCode::unsupported_vhdx,           "UnsupportedVhdx",           ErrorCategory::incompatible,          OperationalClass::needs_attention, "Error",   false, "Convert VHDX to a supported format"},
    {StableErrorCode::mount_point_conflict,       "MountPointConflict",        ErrorCategory::conflict,              OperationalClass::needs_attention, "Error",   false, "Resolve the mount point conflict"},
    {StableErrorCode::volume_flush_failed,        "VolumeFlushFailed",         ErrorCategory::persistence_blocked,   OperationalClass::persistence_blocked, "Error", true,  "Ensure underlying storage is healthy"},
    {StableErrorCode::memory_budget_exceeded,     "MemoryBudgetExceeded",      ErrorCategory::needs_attention,       OperationalClass::needs_attention, "Error",   false, "Reduce memory usage or increase the budget"},
    {StableErrorCode::memory_pressure_blocked,    "MemoryPressureBlocked",     ErrorCategory::transient,             OperationalClass::transient,       "Warning", true,  "Wait for memory pressure to subside"},
    {StableErrorCode::checkpoint_not_committed,   "CheckpointNotCommitted",    ErrorCategory::transient,             OperationalClass::transient,       "Warning", true,  "Wait for checkpoint commit to complete"},
    {StableErrorCode::persistence_blocked,        "PersistenceBlocked",        ErrorCategory::persistence_blocked,   OperationalClass::persistence_blocked, "Error", true,  "Ensure the storage subsystem is operational"},
    {StableErrorCode::commit_indeterminate,       "CommitIndeterminate",       ErrorCategory::commit_indeterminate,  OperationalClass::needs_attention, "Error",   false, "Verify commit outcome and recover if needed"},
    {StableErrorCode::compaction_deferred,        "CompactionDeferred",        ErrorCategory::transient,             OperationalClass::transient,       "Warning", true,  "Compaction will proceed when resources allow"},
    {StableErrorCode::worker_lost,                "WorkerLost",                ErrorCategory::unavailable,           OperationalClass::transient,       "Warning", true,  "Restart the worker or fail over"},
    {StableErrorCode::worker_unresponsive,        "WorkerUnresponsive",        ErrorCategory::unavailable,           OperationalClass::transient,       "Warning", true,  "Check worker health and restart if needed"},
    {StableErrorCode::orphaned_runtime,           "OrphanedRuntime",           ErrorCategory::needs_attention,       OperationalClass::needs_attention, "Error",   false, "Clean up orphaned runtime resources"},
    {StableErrorCode::operation_timeout,          "OperationTimeout",          ErrorCategory::timeout,               std::nullopt,                      "Error",   true,  "Increase the timeout or retry the operation"},
    {StableErrorCode::operation_cancelled,        "OperationCancelled",        ErrorCategory::cancelled,             std::nullopt,                      "Info",    false, "Operation was cancelled; retry if needed"},
    {StableErrorCode::internal_invariant_violation,"InternalInvariantViolation",ErrorCategory::fatal,                  OperationalClass::fatal,           "Fatal",   false, "Report this error to the development team"},
}};

const ErrorDefinition* find_definition(StableErrorCode code) {
    for (const auto& def : kErrorCatalog) {
        if (def.code == code) return &def;
    }
    return nullptr;
}

const ErrorDefinition* find_definition_by_stable_code(std::string_view sc) {
    for (const auto& def : kErrorCatalog) {
        if (def.stable_code == sc) return &def;
    }
    return nullptr;
}

std::string wstring_to_utf8(std::wstring_view ws) {
    if (ws.empty()) return {};
    if (ws.size() > static_cast<size_t>(INT_MAX)) return {};
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      ws.data(), static_cast<int>(ws.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       ws.data(), static_cast<int>(ws.size()),
                                       result.data(), needed, nullptr, nullptr);
    if (written <= 0 || written != needed) return {};
    return result;
}

} // namespace

std::span<const ErrorDefinition> stable_error_catalog() {
    return kErrorCatalog;
}

const ErrorDefinition& error_definition(StableErrorCode code) {
    const auto* def = find_definition(code);
    if (!def) {
        return kErrorCatalog.back();
    }
    return *def;
}

std::string to_string(StableErrorCode code) {
    const auto& def = error_definition(code);
    return std::string(def.stable_code);
}

std::string to_string(ErrorCategory cat) {
    switch (cat) {
    case ErrorCategory::usage:                return "Usage";
    case ErrorCategory::not_found:            return "NotFound";
    case ErrorCategory::conflict:             return "Conflict";
    case ErrorCategory::permission:           return "Permission";
    case ErrorCategory::unavailable:          return "Unavailable";
    case ErrorCategory::transient:            return "Transient";
    case ErrorCategory::persistence_blocked:  return "PersistenceBlocked";
    case ErrorCategory::needs_attention:      return "NeedsAttention";
    case ErrorCategory::commit_indeterminate: return "CommitIndeterminate";
    case ErrorCategory::timeout:              return "Timeout";
    case ErrorCategory::cancelled:            return "Cancelled";
    case ErrorCategory::incompatible:         return "Incompatible";
    case ErrorCategory::integrity:            return "Integrity";
    case ErrorCategory::fatal:                return "Fatal";
    }
    return {};
}

std::string to_string(OperationalClass cls) {
    switch (cls) {
    case OperationalClass::transient:           return "Transient";
    case OperationalClass::persistence_blocked: return "PersistenceBlocked";
    case OperationalClass::needs_attention:     return "NeedsAttention";
    case OperationalClass::fatal:               return "Fatal";
    }
    return {};
}

int cli_exit_code(ErrorCategory cat) {
    switch (cat) {
    case ErrorCategory::usage:                return 2;
    case ErrorCategory::not_found:            return 3;
    case ErrorCategory::conflict:             return 4;
    case ErrorCategory::permission:           return 5;
    case ErrorCategory::unavailable:          return 6;
    case ErrorCategory::transient:            return 7;
    case ErrorCategory::persistence_blocked:  return 8;
    case ErrorCategory::needs_attention:      return 9;
    case ErrorCategory::commit_indeterminate: return 10;
    case ErrorCategory::timeout:              return 11;
    case ErrorCategory::cancelled:            return 12;
    case ErrorCategory::incompatible:         return 13;
    case ErrorCategory::integrity:            return 14;
    case ErrorCategory::fatal:                return 15;
    }
    return 1;
}

ProductError make_product_error(
    StableErrorCode code,
    std::string message,
    std::string component,
    std::chrono::system_clock::time_point timestamp,
    std::optional<Uuid> request_id,
    std::optional<Uuid> operation_id,
    std::optional<Uuid> profile_id,
    std::string native_domain,
    std::uint32_t native_code)
{
    const auto& def = error_definition(code);
    ProductError err;
    err.category = def.category;
    err.operational_class = def.operational_class;
    err.stable_code = std::string(def.stable_code);
    err.message = std::move(message);
    err.recommended_action = std::string(def.recommended_action);
    err.component = std::move(component);
    err.severity = std::string(def.severity);
    err.request_id = std::move(request_id);
    err.operation_id = std::move(operation_id);
    err.profile_id = std::move(profile_id);
    err.native_domain = std::move(native_domain);
    err.native_code = native_code;
    err.retryable = def.retryable;
    err.timestamp = timestamp;
    return err;
}

bool has_complete_public_fields(const ProductError& err) {
    if (err.stable_code.empty()) return false;
    if (err.message.empty()) return false;
    if (err.recommended_action.empty()) return false;
    if (err.component.empty()) return false;
    if (err.severity.empty()) return false;
    if (err.native_domain.empty()) return false;
    if (err.timestamp == std::chrono::system_clock::time_point{}) return false;

    const auto* def = find_definition_by_stable_code(err.stable_code);
    if (!def) return false;
    if (err.category != def->category) return false;
    if (err.operational_class.has_value() != def->operational_class.has_value()) return false;
    if (err.operational_class.has_value() && *err.operational_class != *def->operational_class) return false;
    if (err.severity != def->severity) return false;
    if (err.recommended_action != def->recommended_action) return false;
    if (err.retryable != def->retryable) return false;

    return true;
}

namespace {

const char* error_domain_to_string(myrd::ErrorDomain domain) {
    switch (domain) {
    case myrd::ErrorDomain::usage:       return "usage";
    case myrd::ErrorDomain::preflight:   return "preflight";
    case myrd::ErrorDomain::protocol:    return "protocol";
    case myrd::ErrorDomain::win32:       return "win32";
    case myrd::ErrorDomain::ntstatus:    return "ntstatus";
    case myrd::ErrorDomain::storage:     return "storage";
    case myrd::ErrorDomain::cleanup:     return "cleanup";
    case myrd::ErrorDomain::evidence:    return "evidence";
    case myrd::ErrorDomain::internal:    return "internal";
    case myrd::ErrorDomain::persistence: return "persistence";
    }
    return "unknown";
}

StableErrorCode classify_win32(uint32_t lookup_code) {
    switch (lookup_code) {
    case 2:    return StableErrorCode::not_found;
    case 3:    return StableErrorCode::not_found;
    case 5:    return StableErrorCode::permission_denied;
    case 1314: return StableErrorCode::permission_denied;
    case 1460: return StableErrorCode::operation_timeout;
    case 258:  return StableErrorCode::operation_timeout;
    case 1223: return StableErrorCode::operation_cancelled;
    case 995:  return StableErrorCode::operation_cancelled;
    case 170:  return StableErrorCode::operation_conflict;
    case 32:   return StableErrorCode::operation_conflict;
    case 33:   return StableErrorCode::operation_conflict;
    default:   return StableErrorCode::service_unavailable;
    }
}

StableErrorCode classify_ntstatus(uint32_t lookup_code) {
    switch (lookup_code) {
    case 0xC0000022: return StableErrorCode::permission_denied;
    default:         return StableErrorCode::service_unavailable;
    }
}

StableErrorCode classify_storage(uint32_t code) {
    using OEC = myrd::OperationErrorCode;
    switch (static_cast<OEC>(code)) {
    case OEC::protocol_malformed:  return StableErrorCode::protocol_incompatible;
    case OEC::unknown_request:     return StableErrorCode::protocol_incompatible;
    case OEC::invalid_range:       return StableErrorCode::usage_error;
    case OEC::misaligned:          return StableErrorCode::usage_error;
    case OEC::read_only:           return StableErrorCode::permission_denied;
    case OEC::backing_read_failed: return StableErrorCode::service_unavailable;
    case OEC::memory_hard_limit:   return StableErrorCode::memory_budget_exceeded;
    case OEC::commit_failed:       return StableErrorCode::persistence_blocked;
    case OEC::decommit_failed:     return StableErrorCode::memory_pressure_blocked;
    case OEC::cancelled:           return StableErrorCode::operation_cancelled;
    case OEC::timed_out:           return StableErrorCode::operation_timeout;
    case OEC::session_closed:      return StableErrorCode::service_unavailable;
    case OEC::version_changed:     return StableErrorCode::operation_conflict;
    case OEC::internal_invariant:  return StableErrorCode::internal_invariant_violation;
    }
    return StableErrorCode::service_unavailable;
}

StableErrorCode classify_persistence(uint32_t code) {
    switch (code) {
    case 1:  return StableErrorCode::bundle_corrupt;
    case 2:  return StableErrorCode::unsupported_vhdx;
    case 3:  return StableErrorCode::bundle_corrupt;
    case 4:  return StableErrorCode::bundle_corrupt;
    case 5:  return StableErrorCode::bundle_corrupt;
    case 6:  return StableErrorCode::bundle_locked;
    case 7:  return StableErrorCode::operation_conflict;
    case 8:  return StableErrorCode::operation_conflict;
    case 9:  return StableErrorCode::operation_conflict;
    case 10: return StableErrorCode::checkpoint_not_committed;
    case 11: return StableErrorCode::memory_pressure_blocked;
    case 12: return StableErrorCode::commit_indeterminate;
    case 13: return StableErrorCode::persistence_blocked;
    case 14: return StableErrorCode::persistence_blocked;
    case 15: return StableErrorCode::persistence_blocked;
    case 16: return StableErrorCode::operation_cancelled;
    case 17: return StableErrorCode::operation_timeout;
    case 18: return StableErrorCode::bundle_corrupt;
    case 19: return StableErrorCode::internal_invariant_violation;
    default: return StableErrorCode::service_unavailable;
    }
}

} // namespace

ProductError map_legacy_error(
    const myrd::Error& legacy,
    std::string component,
    std::chrono::system_clock::time_point timestamp,
    std::optional<Uuid> request_id,
    std::optional<Uuid> operation_id,
    std::optional<Uuid> profile_id)
{
    StableErrorCode sc = StableErrorCode::internal_invariant_violation;
    const char* domain_str = error_domain_to_string(legacy.domain);
    uint32_t native_code = legacy.native_code != 0 ? legacy.native_code : legacy.code;

    switch (legacy.domain) {
    case myrd::ErrorDomain::win32: {
        uint32_t lookup = legacy.native_code != 0 ? legacy.native_code : legacy.code;
        sc = classify_win32(lookup);
        break;
    }
    case myrd::ErrorDomain::ntstatus: {
        uint32_t lookup = legacy.native_code != 0 ? legacy.native_code : legacy.code;
        sc = classify_ntstatus(lookup);
        break;
    }
    case myrd::ErrorDomain::storage:
        sc = classify_storage(legacy.code);
        break;
    case myrd::ErrorDomain::persistence:
        sc = classify_persistence(legacy.code);
        break;
    case myrd::ErrorDomain::usage:
        sc = StableErrorCode::usage_error;
        break;
    case myrd::ErrorDomain::preflight:
        sc = StableErrorCode::usage_error;
        break;
    case myrd::ErrorDomain::protocol:
        sc = StableErrorCode::protocol_incompatible;
        break;
    case myrd::ErrorDomain::cleanup:
        sc = StableErrorCode::orphaned_runtime;
        break;
    case myrd::ErrorDomain::evidence:
        sc = StableErrorCode::internal_invariant_violation;
        break;
    case myrd::ErrorDomain::internal:
        sc = StableErrorCode::internal_invariant_violation;
        break;
    }

    std::string utf8_message = wstring_to_utf8(legacy.message);
    if (legacy.message.empty()) {
        utf8_message = "Legacy operation failed";
    } else if (utf8_message.empty()) {
        sc = StableErrorCode::internal_invariant_violation;
        utf8_message = "Internal invariant violation: message conversion failure";
    }

    auto err = make_product_error(sc, std::move(utf8_message), std::move(component),
                                   timestamp, std::move(request_id),
                                   std::move(operation_id), std::move(profile_id),
                                   std::string(domain_str), native_code);
    return err;
}

} // namespace myrd::product
