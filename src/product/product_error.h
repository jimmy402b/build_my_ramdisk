#pragma once

#include "src/product/uuid.h"
#include "src/common/error.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myrd::product {

enum class StableErrorCode : std::uint16_t {
    usage_error,
    not_found,
    request_id_conflict,
    operation_conflict,
    permission_denied,
    service_unavailable,
    protocol_incompatible,
    driver_unavailable,
    driver_mismatch,
    driver_busy,
    adapter_unavailable,
    device_identity_mismatch,
    profile_corrupt,
    profile_revision_conflict,
    bundle_locked,
    bundle_corrupt,
    unsupported_vhdx,
    mount_point_conflict,
    volume_flush_failed,
    memory_budget_exceeded,
    memory_pressure_blocked,
    checkpoint_not_committed,
    persistence_blocked,
    commit_indeterminate,
    compaction_deferred,
    worker_lost,
    worker_unresponsive,
    orphaned_runtime,
    operation_timeout,
    operation_cancelled,
    internal_invariant_violation,
};

enum class ErrorCategory : std::uint16_t {
    usage,
    not_found,
    conflict,
    permission,
    unavailable,
    transient,
    persistence_blocked,
    needs_attention,
    commit_indeterminate,
    timeout,
    cancelled,
    incompatible,
    integrity,
    fatal,
};

enum class OperationalClass : std::uint16_t {
    transient,
    persistence_blocked,
    needs_attention,
    fatal,
};

struct ErrorDefinition {
    StableErrorCode code;
    std::string_view stable_code;
    ErrorCategory category;
    std::optional<OperationalClass> operational_class;
    std::string_view severity;
    bool retryable;
    std::string_view recommended_action;
};

struct ProductError {
    ErrorCategory category{};
    std::optional<OperationalClass> operational_class;
    std::string stable_code;
    std::string message;
    std::string recommended_action;
    std::string component;
    std::string severity;
    std::optional<Uuid> request_id;
    std::optional<Uuid> operation_id;
    std::optional<Uuid> profile_id;
    std::string native_domain;
    std::uint32_t native_code{};
    bool retryable{};
    std::chrono::system_clock::time_point timestamp{};
};

std::span<const ErrorDefinition> stable_error_catalog();

const ErrorDefinition& error_definition(StableErrorCode code);

std::string to_string(StableErrorCode code);
std::string to_string(ErrorCategory cat);
std::string to_string(OperationalClass cls);

int cli_exit_code(ErrorCategory cat);

ProductError make_product_error(
    StableErrorCode code,
    std::string message,
    std::string component,
    std::chrono::system_clock::time_point timestamp,
    std::optional<Uuid> request_id,
    std::optional<Uuid> operation_id,
    std::optional<Uuid> profile_id,
    std::string native_domain,
    std::uint32_t native_code);

bool has_complete_public_fields(const ProductError& err);

ProductError map_legacy_error(
    const myrd::Error& legacy,
    std::string component,
    std::chrono::system_clock::time_point timestamp,
    std::optional<Uuid> request_id,
    std::optional<Uuid> operation_id,
    std::optional<Uuid> profile_id);

} // namespace myrd::product
