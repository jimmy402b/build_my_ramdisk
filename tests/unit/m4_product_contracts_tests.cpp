#include "test_framework.h"

#include "src/product/uuid.h"
#include "src/product/product_error.h"
#include "src/product/product_state.h"
#include "src/product/product_version.h"
#include "src/persistence/persistence_types.h"

#include <chrono>
#include <string>
#include <string_view>
#include <type_traits>

using myrd::product::Uuid;

// parse: valid canonical forms --------------------------------------------

TEST_CASE(uuid_parse_valid_lowercase_hex) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789abcdef");
    REQUIRE(r.ok());
}

TEST_CASE(uuid_parse_roundtrip_to_string) {
    constexpr std::string_view input = "01234567-89ab-cdef-0123-456789abcdef";
    auto r = Uuid::parse(input);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().to_string(), std::string(input));
}

TEST_CASE(uuid_parse_bytes_match) {
    auto r = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(r.ok());
    auto b = r.value().bytes();
    REQUIRE_EQ(b[0], static_cast<std::byte>(0x00));
    REQUIRE_EQ(b[1], static_cast<std::byte>(0x11));
    REQUIRE_EQ(b[2], static_cast<std::byte>(0x22));
    REQUIRE_EQ(b[3], static_cast<std::byte>(0x33));
    REQUIRE_EQ(b[4], static_cast<std::byte>(0x44));
    REQUIRE_EQ(b[5], static_cast<std::byte>(0x55));
    REQUIRE_EQ(b[6], static_cast<std::byte>(0x66));
    REQUIRE_EQ(b[7], static_cast<std::byte>(0x77));
    REQUIRE_EQ(b[8], static_cast<std::byte>(0x88));
    REQUIRE_EQ(b[9], static_cast<std::byte>(0x99));
    REQUIRE_EQ(b[10], static_cast<std::byte>(0xaa));
    REQUIRE_EQ(b[11], static_cast<std::byte>(0xbb));
    REQUIRE_EQ(b[12], static_cast<std::byte>(0xcc));
    REQUIRE_EQ(b[13], static_cast<std::byte>(0xdd));
    REQUIRE_EQ(b[14], static_cast<std::byte>(0xee));
    REQUIRE_EQ(b[15], static_cast<std::byte>(0xff));
}

TEST_CASE(uuid_parse_all_hex_digits) {
    auto r = Uuid::parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().to_string(), "ffffffff-ffff-ffff-ffff-ffffffffffff");
}

// parse: reject invalid forms ---------------------------------------------

TEST_CASE(uuid_parse_reject_empty) {
    auto r = Uuid::parse("");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_too_short) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789abcde");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_too_long) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789abcdef0");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_uppercase) {
    auto r = Uuid::parse("01234567-89AB-CDEF-0123-456789ABCDEF");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_mixed_case) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789ABCDEF");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_missing_hyphens) {
    auto r = Uuid::parse("0123456789abcdef0123456789abcdef");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_wrong_hyphen_positions) {
    auto r = Uuid::parse("01234567-89abc-def0-1234-56789abcdef");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_non_hex_letter_g) {
    auto r = Uuid::parse("g1234567-89ab-cdef-0123-456789abcdef");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_non_hex_char) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789abcde!");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_braces) {
    auto r = Uuid::parse("{01234567-89ab-cdef-0123-456789abcdef}");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_leading_whitespace) {
    auto r = Uuid::parse(" 01234567-89ab-cdef-0123-456789abcdef");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_trailing_whitespace) {
    auto r = Uuid::parse("01234567-89ab-cdef-0123-456789abcdef ");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_inner_whitespace) {
    auto r = Uuid::parse("01234567-89ab cdef-0123-456789abcdef");
    REQUIRE(r.has_error());
}

TEST_CASE(uuid_parse_reject_prefix) {
    auto r = Uuid::parse("urn:uuid:01234567-89ab-cdef-0123-456789abcdef");
    REQUIRE(r.has_error());
}

// random() ----------------------------------------------------------------

TEST_CASE(uuid_random_succeeds) {
    auto r = Uuid::random();
    REQUIRE(r.ok());
}

TEST_CASE(uuid_random_produces_parseable) {
    auto r = Uuid::random();
    REQUIRE(r.ok());
    auto s = r.value().to_string();
    auto r2 = Uuid::parse(s);
    REQUIRE(r2.ok());
}

TEST_CASE(uuid_random_sets_version_4_bits) {
    auto r = Uuid::random();
    REQUIRE(r.ok());
    auto b = r.value().bytes();
    REQUIRE_EQ(static_cast<unsigned char>(b[6]) & 0xF0, 0x40);
}

TEST_CASE(uuid_random_sets_variant_bits) {
    auto r = Uuid::random();
    REQUIRE(r.ok());
    auto b = r.value().bytes();
    REQUIRE_EQ(static_cast<unsigned char>(b[8]) & 0xC0, 0x80);
}

// to_string() format ------------------------------------------------------

TEST_CASE(uuid_to_string_is_lowercase) {
    auto r = Uuid::parse("AABBCCDD-EEFF-0011-2233-445566778899");
    REQUIRE(r.has_error());
    auto r2 = Uuid::parse("aabbccdd-eeff-0011-2233-445566778899");
    REQUIRE(r2.ok());
    REQUIRE_EQ(r2.value().to_string(), "aabbccdd-eeff-0011-2233-445566778899");
}

TEST_CASE(uuid_to_string_hyphen_positions) {
    auto r = Uuid::parse("00000000-0000-0000-0000-000000000000");
    REQUIRE(r.ok());
    auto s = r.value().to_string();
    REQUIRE_EQ(s.size(), 36u);
    REQUIRE_EQ(s[8], '-');
    REQUIRE_EQ(s[13], '-');
    REQUIRE_EQ(s[18], '-');
    REQUIRE_EQ(s[23], '-');
}

// bytes() -----------------------------------------------------------------

TEST_CASE(uuid_bytes_size_is_16) {
    auto r = Uuid::parse("00000000-0000-0000-0000-000000000000");
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().bytes().size(), 16u);
}

TEST_CASE(uuid_parse_and_bytes_roundtrip) {
    auto r = Uuid::parse("a1b2c3d4-e5f6-4708-890a-bcdef0123456");
    REQUIRE(r.ok());
    auto b = r.value().bytes();
    REQUIRE_EQ(b[0], static_cast<std::byte>(0xa1));
    REQUIRE_EQ(b[1], static_cast<std::byte>(0xb2));
    REQUIRE_EQ(b[2], static_cast<std::byte>(0xc3));
    REQUIRE_EQ(b[3], static_cast<std::byte>(0xd4));
    REQUIRE_EQ(b[4], static_cast<std::byte>(0xe5));
    REQUIRE_EQ(b[5], static_cast<std::byte>(0xf6));
    REQUIRE_EQ(b[6], static_cast<std::byte>(0x47));
    REQUIRE_EQ(b[7], static_cast<std::byte>(0x08));
    REQUIRE_EQ(b[8], static_cast<std::byte>(0x89));
    REQUIRE_EQ(b[9], static_cast<std::byte>(0x0a));
    REQUIRE_EQ(b[10], static_cast<std::byte>(0xbc));
    REQUIRE_EQ(b[11], static_cast<std::byte>(0xde));
    REQUIRE_EQ(b[12], static_cast<std::byte>(0xf0));
    REQUIRE_EQ(b[13], static_cast<std::byte>(0x12));
    REQUIRE_EQ(b[14], static_cast<std::byte>(0x34));
    REQUIRE_EQ(b[15], static_cast<std::byte>(0x56));
}

// operator== --------------------------------------------------------------

TEST_CASE(uuid_equality_same) {
    auto r1 = Uuid::parse("11111111-2222-3333-4444-555555555555");
    auto r2 = Uuid::parse("11111111-2222-3333-4444-555555555555");
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(r1.value() == r2.value());
}

TEST_CASE(uuid_equality_different) {
    auto r1 = Uuid::parse("11111111-2222-3333-4444-555555555555");
    auto r2 = Uuid::parse("21111111-2222-3333-4444-555555555555");
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(!(r1.value() == r2.value()));
}

// ==========================================================================
// M4 Task 01 microtask 01.3: product error contract RED tests
// ==========================================================================

using myrd::product::ErrorCategory;
using myrd::product::OperationalClass;
using myrd::product::StableErrorCode;
using myrd::product::ProductError;
using myrd::product::ErrorDefinition;

static_assert(std::is_same_v<std::underlying_type_t<ErrorCategory>, std::uint16_t>,
    "ErrorCategory must be uint16_t per frozen PLAN");
static_assert(std::is_same_v<std::underlying_type_t<OperationalClass>, std::uint16_t>,
    "OperationalClass must be uint16_t per frozen PLAN");

// ---- catalog: 31 entries, every code exactly once, exact mapping -----------

namespace {

struct CatalogExpectation {
    StableErrorCode code;
    std::string_view stable_code;
    ErrorCategory category;
    std::optional<OperationalClass> operational_class;
    bool retryable;
};

const CatalogExpectation kCatalog[31] = {
    {StableErrorCode::usage_error,                "UsageError",                ErrorCategory::usage,                std::nullopt,                       false},
    {StableErrorCode::not_found,                  "NotFound",                  ErrorCategory::not_found,             std::nullopt,                       false},
    {StableErrorCode::request_id_conflict,        "RequestIdConflict",         ErrorCategory::conflict,              std::nullopt,                       false},
    {StableErrorCode::operation_conflict,         "OperationConflict",         ErrorCategory::conflict,              std::nullopt,                       true},
    {StableErrorCode::permission_denied,          "PermissionDenied",          ErrorCategory::permission,            std::nullopt,                       false},
    {StableErrorCode::service_unavailable,        "ServiceUnavailable",        ErrorCategory::unavailable,           OperationalClass::transient,         true},
    {StableErrorCode::protocol_incompatible,      "ProtocolIncompatible",      ErrorCategory::incompatible,          std::nullopt,                       false},
    {StableErrorCode::driver_unavailable,         "DriverUnavailable",         ErrorCategory::unavailable,           OperationalClass::transient,         true},
    {StableErrorCode::driver_mismatch,            "DriverMismatch",            ErrorCategory::incompatible,          OperationalClass::needs_attention,   false},
    {StableErrorCode::driver_busy,                "DriverBusy",                ErrorCategory::transient,             OperationalClass::transient,         true},
    {StableErrorCode::adapter_unavailable,        "AdapterUnavailable",        ErrorCategory::unavailable,           OperationalClass::transient,         true},
    {StableErrorCode::device_identity_mismatch,   "DeviceIdentityMismatch",    ErrorCategory::integrity,             OperationalClass::needs_attention,   false},
    {StableErrorCode::profile_corrupt,            "ProfileCorrupt",            ErrorCategory::integrity,             OperationalClass::needs_attention,   false},
    {StableErrorCode::profile_revision_conflict,  "ProfileRevisionConflict",   ErrorCategory::conflict,              std::nullopt,                       true},
    {StableErrorCode::bundle_locked,              "BundleLocked",              ErrorCategory::conflict,              OperationalClass::transient,         true},
    {StableErrorCode::bundle_corrupt,             "BundleCorrupt",             ErrorCategory::integrity,             OperationalClass::needs_attention,   false},
    {StableErrorCode::unsupported_vhdx,           "UnsupportedVhdx",           ErrorCategory::incompatible,          OperationalClass::needs_attention,   false},
    {StableErrorCode::mount_point_conflict,       "MountPointConflict",        ErrorCategory::conflict,              OperationalClass::needs_attention,   false},
    {StableErrorCode::volume_flush_failed,        "VolumeFlushFailed",         ErrorCategory::persistence_blocked,   OperationalClass::persistence_blocked, true},
    {StableErrorCode::memory_budget_exceeded,     "MemoryBudgetExceeded",      ErrorCategory::needs_attention,       OperationalClass::needs_attention,   false},
    {StableErrorCode::memory_pressure_blocked,    "MemoryPressureBlocked",     ErrorCategory::transient,             OperationalClass::transient,         true},
    {StableErrorCode::checkpoint_not_committed,   "CheckpointNotCommitted",    ErrorCategory::transient,             OperationalClass::transient,         true},
    {StableErrorCode::persistence_blocked,        "PersistenceBlocked",        ErrorCategory::persistence_blocked,   OperationalClass::persistence_blocked, true},
    {StableErrorCode::commit_indeterminate,       "CommitIndeterminate",       ErrorCategory::commit_indeterminate,  OperationalClass::needs_attention,   false},
    {StableErrorCode::compaction_deferred,        "CompactionDeferred",        ErrorCategory::transient,             OperationalClass::transient,         true},
    {StableErrorCode::worker_lost,                "WorkerLost",                ErrorCategory::unavailable,           OperationalClass::transient,         true},
    {StableErrorCode::worker_unresponsive,        "WorkerUnresponsive",        ErrorCategory::unavailable,           OperationalClass::transient,         true},
    {StableErrorCode::orphaned_runtime,           "OrphanedRuntime",           ErrorCategory::needs_attention,       OperationalClass::needs_attention,   false},
    {StableErrorCode::operation_timeout,          "OperationTimeout",          ErrorCategory::timeout,               std::nullopt,                       true},
    {StableErrorCode::operation_cancelled,        "OperationCancelled",        ErrorCategory::cancelled,             std::nullopt,                       false},
    {StableErrorCode::internal_invariant_violation,"InternalInvariantViolation",ErrorCategory::fatal,                  OperationalClass::fatal,             false},
};

} // namespace

TEST_CASE(error_catalog_size_is_31) {
    auto catalog = myrd::product::stable_error_catalog();
    REQUIRE_EQ(catalog.size(), 31u);
}

TEST_CASE(error_catalog_every_code_exactly_once) {
    auto catalog = myrd::product::stable_error_catalog();
    for (const auto& exp : kCatalog) {
        int count = 0;
        for (const auto& def : catalog) {
            if (def.code == exp.code) ++count;
        }
        REQUIRE_EQ(count, 1);
    }
}

TEST_CASE(error_catalog_exact_string_category_operational_mapping) {
    auto catalog = myrd::product::stable_error_catalog();
    for (const auto& exp : kCatalog) {
        const ErrorDefinition* found = nullptr;
        for (const auto& def : catalog) {
            if (def.code == exp.code) { found = &def; break; }
        }
        REQUIRE(found != nullptr);
        REQUIRE_EQ(found->stable_code, exp.stable_code);
        REQUIRE_EQ(found->category, exp.category);
        REQUIRE_EQ(found->operational_class.has_value(), exp.operational_class.has_value());
        if (exp.operational_class.has_value()) {
            REQUIRE_EQ(*found->operational_class, *exp.operational_class);
        }
        REQUIRE_EQ(found->retryable, exp.retryable);
    }
}

TEST_CASE(error_catalog_severity_and_action_nonempty) {
    auto catalog = myrd::product::stable_error_catalog();
    for (const auto& def : catalog) {
        REQUIRE(def.severity.size() > 0);
        REQUIRE(def.recommended_action.size() > 0);
    }
}

TEST_CASE(error_definition_lookup_matches_catalog_entry) {
    for (const auto& exp : kCatalog) {
        auto def = myrd::product::error_definition(exp.code);
        REQUIRE_EQ(def.code, exp.code);
        REQUIRE_EQ(def.stable_code, exp.stable_code);
        REQUIRE_EQ(def.category, exp.category);
        REQUIRE_EQ(def.retryable, exp.retryable);
    }
}

// ---- ErrorCategory strings and exit codes --------------------------------

TEST_CASE(error_category_exact_strings) {
    struct { ErrorCategory cat; std::string_view str; } cases[] = {
        {ErrorCategory::usage,                "Usage"},
        {ErrorCategory::not_found,            "NotFound"},
        {ErrorCategory::conflict,             "Conflict"},
        {ErrorCategory::permission,           "Permission"},
        {ErrorCategory::unavailable,          "Unavailable"},
        {ErrorCategory::transient,            "Transient"},
        {ErrorCategory::persistence_blocked,  "PersistenceBlocked"},
        {ErrorCategory::needs_attention,      "NeedsAttention"},
        {ErrorCategory::commit_indeterminate, "CommitIndeterminate"},
        {ErrorCategory::timeout,              "Timeout"},
        {ErrorCategory::cancelled,            "Cancelled"},
        {ErrorCategory::incompatible,         "Incompatible"},
        {ErrorCategory::integrity,            "Integrity"},
        {ErrorCategory::fatal,                "Fatal"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.cat), c.str);
    }
}

TEST_CASE(error_category_exact_exit_codes) {
    struct { ErrorCategory cat; int exit_code; } cases[] = {
        {ErrorCategory::usage,                2},
        {ErrorCategory::not_found,            3},
        {ErrorCategory::conflict,             4},
        {ErrorCategory::permission,           5},
        {ErrorCategory::unavailable,          6},
        {ErrorCategory::transient,            7},
        {ErrorCategory::persistence_blocked,  8},
        {ErrorCategory::needs_attention,      9},
        {ErrorCategory::commit_indeterminate, 10},
        {ErrorCategory::timeout,              11},
        {ErrorCategory::cancelled,            12},
        {ErrorCategory::incompatible,         13},
        {ErrorCategory::integrity,            14},
        {ErrorCategory::fatal,                15},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::cli_exit_code(c.cat), c.exit_code);
    }
}

TEST_CASE(operational_class_exact_strings) {
    struct { OperationalClass cls; std::string_view str; } cases[] = {
        {OperationalClass::transient,            "Transient"},
        {OperationalClass::persistence_blocked,  "PersistenceBlocked"},
        {OperationalClass::needs_attention,      "NeedsAttention"},
        {OperationalClass::fatal,                "Fatal"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.cls), c.str);
    }
}

// ---- make_product_error + has_complete_public_fields ---------------------

TEST_CASE(make_product_error_populates_all_fields_from_catalog_and_args) {
    auto fixed_ts = std::chrono::system_clock::from_time_t(1730000000);
    auto req_id = Uuid::parse("a1b2c3d4-e5f6-4708-890a-bcdef0123456");
    auto op_id  = Uuid::parse("11111111-2222-3333-4444-555555555555");
    auto prof_id = Uuid::parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    REQUIRE(req_id.ok());
    REQUIRE(op_id.ok());
    REQUIRE(prof_id.ok());

    auto err = myrd::product::make_product_error(
        StableErrorCode::volume_flush_failed,
        "flush timed out after 30 s",
        "host-core/checkpoint",
        fixed_ts,
        req_id.value(),
        op_id.value(),
        prof_id.value(),
        "win32",
        0x45A
    );

    REQUIRE_EQ(err.stable_code, std::string("VolumeFlushFailed"));
    REQUIRE_EQ(err.category, ErrorCategory::persistence_blocked);
    REQUIRE(err.operational_class.has_value());
    REQUIRE_EQ(*err.operational_class, OperationalClass::persistence_blocked);
    REQUIRE(err.severity.size() > 0);
    REQUIRE(err.recommended_action.size() > 0);
    REQUIRE_EQ(err.message, std::string("flush timed out after 30 s"));
    REQUIRE_EQ(err.component, std::string("host-core/checkpoint"));
    REQUIRE_EQ(err.native_domain, std::string("win32"));
    REQUIRE_EQ(err.native_code, 0x45Au);
    REQUIRE(err.request_id.has_value());
    REQUIRE(err.operation_id.has_value());
    REQUIRE(err.profile_id.has_value());
    REQUIRE_EQ(err.request_id->to_string(), std::string("a1b2c3d4-e5f6-4708-890a-bcdef0123456"));
    REQUIRE_EQ(err.operation_id->to_string(), std::string("11111111-2222-3333-4444-555555555555"));
    REQUIRE_EQ(err.profile_id->to_string(), std::string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    REQUIRE_EQ(err.timestamp.time_since_epoch().count(), fixed_ts.time_since_epoch().count());
    auto def = myrd::product::error_definition(StableErrorCode::volume_flush_failed);
    REQUIRE_EQ(err.retryable, def.retryable);
}

TEST_CASE(make_product_error_retains_fixed_timestamp) {
    auto fixed_ts = std::chrono::system_clock::from_time_t(0);
    auto err = myrd::product::make_product_error(
        StableErrorCode::operation_timeout,
        "test",
        "test-comp",
        fixed_ts,
        std::nullopt, std::nullopt, std::nullopt,
        "none", 0
    );
    REQUIRE_EQ(err.timestamp.time_since_epoch().count(), fixed_ts.time_since_epoch().count());
}

TEST_CASE(make_product_error_optional_ids_absent_still_valid) {
    auto err = myrd::product::make_product_error(
        StableErrorCode::service_unavailable,
        "worker pool exhausted",
        "host-core/pool",
        std::chrono::system_clock::from_time_t(1),
        std::nullopt, std::nullopt, std::nullopt,
        "none", 0
    );
    REQUIRE(!err.request_id.has_value());
    REQUIRE(!err.operation_id.has_value());
    REQUIRE(!err.profile_id.has_value());
    REQUIRE(myrd::product::has_complete_public_fields(err));
}

TEST_CASE(has_complete_public_fields_true_for_valid_error) {
    auto err = myrd::product::make_product_error(
        StableErrorCode::driver_unavailable,
        "driver not responding",
        "host-core/driver",
        std::chrono::system_clock::from_time_t(1),
        std::nullopt, std::nullopt, std::nullopt,
        "win32", 0
    );
    REQUIRE(myrd::product::has_complete_public_fields(err));
}

TEST_CASE(has_complete_public_fields_rejects_each_required_field_mutation) {
    auto ts = std::chrono::system_clock::from_time_t(1);
    auto base = [&]() {
        return myrd::product::make_product_error(
            StableErrorCode::volume_flush_failed,
            "valid message",
            "valid-component",
            ts,
            std::nullopt, std::nullopt, std::nullopt,
            "none", 0);
    };

    auto mut = base(); REQUIRE(myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.stable_code = "";         REQUIRE(!myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.message = "";             REQUIRE(!myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.recommended_action = "";  REQUIRE(!myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.component = "";           REQUIRE(!myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.severity = "";            REQUIRE(!myrd::product::has_complete_public_fields(mut));
    mut = base(); mut.native_domain = "";       REQUIRE(!myrd::product::has_complete_public_fields(mut));

    auto default_ts = std::chrono::system_clock::time_point{};
    mut = base(); mut.timestamp = default_ts;   REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // category inconsistent with catalog entry for VolumeFlushFailed (persistence_blocked)
    mut = base(); mut.category = ErrorCategory::fatal;
    REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // operational_class missing when catalog requires it
    mut = base(); mut.operational_class = std::nullopt;
    REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // operational_class wrong when catalog requires persistence_blocked
    mut = base(); mut.operational_class = OperationalClass::transient;
    REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // nonempty but incorrect recommended_action (catalog value differs)
    mut = base(); mut.recommended_action = "arbitrary wrong action";
    REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // nonempty but incorrect severity (catalog value differs)
    mut = base(); mut.severity = "arbitrary wrong severity";
    REQUIRE(!myrd::product::has_complete_public_fields(mut));

    // retryable flipped vs catalog (volume_flush_failed is retryable=true)
    mut = base(); mut.retryable = false;
    REQUIRE(!myrd::product::has_complete_public_fields(mut));
}

TEST_CASE(has_complete_public_fields_rejects_extra_operational_class) {
    // UsageError has no operational_class per catalog — adding one is invalid
    auto err = myrd::product::make_product_error(
        StableErrorCode::usage_error,
        "bad args",
        "cli",
        std::chrono::system_clock::from_time_t(1),
        std::nullopt, std::nullopt, std::nullopt,
        "none", 0);
    REQUIRE(myrd::product::has_complete_public_fields(err));
    err.operational_class = OperationalClass::fatal;
    REQUIRE(!myrd::product::has_complete_public_fields(err));
}

TEST_CASE(product_error_aggregate_init_exact_plan_field_order) {
    // Frozen PLAN field order:
    // category, operational_class, stable_code, message, recommended_action,
    // component, severity, request_id, operation_id, profile_id,
    // native_domain, native_code, retryable, timestamp
    auto ts = std::chrono::system_clock::from_time_t(1);
    ProductError err{
        ErrorCategory::persistence_blocked,
        std::make_optional(OperationalClass::persistence_blocked),
        "VolumeFlushFailed",
        "flush timed out",
        "retry with backpressure",
        "host-core/checkpoint",
        "error",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        "win32",
        0x45Au,
        true,
        ts,
    };
    REQUIRE_EQ(err.category, ErrorCategory::persistence_blocked);
    REQUIRE(err.operational_class.has_value());
    REQUIRE_EQ(*err.operational_class, OperationalClass::persistence_blocked);
    REQUIRE_EQ(err.stable_code, std::string("VolumeFlushFailed"));
    REQUIRE_EQ(err.message, std::string("flush timed out"));
    REQUIRE_EQ(err.recommended_action, std::string("retry with backpressure"));
    REQUIRE_EQ(err.component, std::string("host-core/checkpoint"));
    REQUIRE_EQ(err.severity, std::string("error"));
    REQUIRE(!err.request_id.has_value());
    REQUIRE(!err.operation_id.has_value());
    REQUIRE(!err.profile_id.has_value());
    REQUIRE_EQ(err.native_domain, std::string("win32"));
    REQUIRE_EQ(err.native_code, 0x45Au);
    REQUIRE_EQ(err.retryable, true);
    REQUIRE_EQ(err.timestamp.time_since_epoch().count(), ts.time_since_epoch().count());
}

// ---- legacy Error mapping ---------------------------------------------------

namespace {

struct LegacyMappingCase {
    myrd::Error legacy;
    std::string_view expected_native_domain;
    StableErrorCode expected_code;
    ErrorCategory expected_category;
};

} // namespace

TEST_CASE(map_legacy_error_covers_every_error_domain) {
    using ED = myrd::ErrorDomain;
    LegacyMappingCase cases[] = {
        {{ED::usage,      0,          L"op", L"bad args"},       "usage",       StableErrorCode::usage_error,                ErrorCategory::usage},
        {{ED::preflight,  0,          L"op", L"check failed"},   "preflight",   StableErrorCode::usage_error,                ErrorCategory::usage},
        {{ED::protocol,   0,          L"op", L"bad frame"},      "protocol",    StableErrorCode::protocol_incompatible,       ErrorCategory::incompatible},
        {{ED::win32,      2,          L"op", L"file not found"}, "win32",       StableErrorCode::not_found,                  ErrorCategory::not_found},
        {{ED::ntstatus,   0xC0000022, L"op", L"denied"},         "ntstatus",    StableErrorCode::permission_denied,           ErrorCategory::permission},
        {{ED::storage,    static_cast<uint32_t>(myrd::OperationErrorCode::invalid_range), L"op", L"bad range"}, "storage", StableErrorCode::usage_error, ErrorCategory::usage},
        {{ED::cleanup,    0,          L"op", L"cleanup"},        "cleanup",     StableErrorCode::orphaned_runtime,           ErrorCategory::needs_attention},
        {{ED::evidence,   0,          L"op", L"evidence"},       "evidence",    StableErrorCode::internal_invariant_violation, ErrorCategory::fatal},
        {{ED::internal,   0,          L"op", L"internal"},       "internal",    StableErrorCode::internal_invariant_violation, ErrorCategory::fatal},
        {{ED::persistence,1,          L"op", L"bad format"},     "persistence", StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
    };
    auto ts = std::chrono::system_clock::from_time_t(1);
    for (const auto& c : cases) {
        auto result = myrd::product::map_legacy_error(c.legacy, "test-comp", ts, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE_EQ(result.native_domain, c.expected_native_domain);
        REQUIRE_EQ(result.stable_code, myrd::product::to_string(c.expected_code));
        REQUIRE_EQ(result.category, c.expected_category);
        REQUIRE(myrd::product::has_complete_public_fields(result));
    }
}

TEST_CASE(map_legacy_error_win32_exact_stable_mappings) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    struct { uint32_t code; StableErrorCode sc; ErrorCategory cat; } cases[] = {
        {2,     StableErrorCode::not_found,           ErrorCategory::not_found},
        {3,     StableErrorCode::not_found,           ErrorCategory::not_found},
        {5,     StableErrorCode::permission_denied,   ErrorCategory::permission},
        {1314,  StableErrorCode::permission_denied,   ErrorCategory::permission},
        {1460,  StableErrorCode::operation_timeout,   ErrorCategory::timeout},
        {258,   StableErrorCode::operation_timeout,   ErrorCategory::timeout},
        {1223,  StableErrorCode::operation_cancelled, ErrorCategory::cancelled},
        {995,   StableErrorCode::operation_cancelled, ErrorCategory::cancelled},
        {170,   StableErrorCode::operation_conflict,  ErrorCategory::conflict},
        {32,    StableErrorCode::operation_conflict,  ErrorCategory::conflict},
        {33,    StableErrorCode::operation_conflict,  ErrorCategory::conflict},
    };
    for (const auto& c : cases) {
        myrd::Error legacy{ED::win32, c.code, L"op", L""};
        auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE_EQ(result.stable_code, myrd::product::to_string(c.sc));
        REQUIRE_EQ(result.category, c.cat);
        REQUIRE_EQ(result.native_domain, std::string("win32"));
    }
}

TEST_CASE(map_legacy_error_win32_default_is_service_unavailable) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::win32, 9999, L"op", L"unknown"};
    auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.stable_code, myrd::product::to_string(StableErrorCode::service_unavailable));
    REQUIRE_EQ(result.category, ErrorCategory::unavailable);
}

TEST_CASE(map_legacy_error_storage_all_operation_error_codes_0x1001_to_0x100E) {
    using ED = myrd::ErrorDomain;
    using OEC = myrd::OperationErrorCode;
    auto ts = std::chrono::system_clock::from_time_t(1);
    struct { uint32_t code; StableErrorCode sc; ErrorCategory cat; } cases[] = {
        {static_cast<uint32_t>(OEC::protocol_malformed),     StableErrorCode::protocol_incompatible,       ErrorCategory::incompatible},
        {static_cast<uint32_t>(OEC::unknown_request),        StableErrorCode::protocol_incompatible,       ErrorCategory::incompatible},
        {static_cast<uint32_t>(OEC::invalid_range),          StableErrorCode::usage_error,                ErrorCategory::usage},
        {static_cast<uint32_t>(OEC::misaligned),             StableErrorCode::usage_error,                ErrorCategory::usage},
        {static_cast<uint32_t>(OEC::read_only),              StableErrorCode::permission_denied,           ErrorCategory::permission},
        {static_cast<uint32_t>(OEC::backing_read_failed),    StableErrorCode::service_unavailable,        ErrorCategory::unavailable},
        {static_cast<uint32_t>(OEC::memory_hard_limit),      StableErrorCode::memory_budget_exceeded,     ErrorCategory::needs_attention},
        {static_cast<uint32_t>(OEC::commit_failed),          StableErrorCode::persistence_blocked,        ErrorCategory::persistence_blocked},
        {static_cast<uint32_t>(OEC::decommit_failed),        StableErrorCode::memory_pressure_blocked,    ErrorCategory::transient},
        {static_cast<uint32_t>(OEC::cancelled),              StableErrorCode::operation_cancelled,        ErrorCategory::cancelled},
        {static_cast<uint32_t>(OEC::timed_out),              StableErrorCode::operation_timeout,          ErrorCategory::timeout},
        {static_cast<uint32_t>(OEC::session_closed),         StableErrorCode::service_unavailable,        ErrorCategory::unavailable},
        {static_cast<uint32_t>(OEC::version_changed),        StableErrorCode::operation_conflict,         ErrorCategory::conflict},
        {static_cast<uint32_t>(OEC::internal_invariant),     StableErrorCode::internal_invariant_violation,ErrorCategory::fatal},
    };
    for (const auto& c : cases) {
        myrd::Error legacy{ED::storage, c.code, L"op", L""};
        auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE_EQ(result.stable_code, myrd::product::to_string(c.sc));
        REQUIRE_EQ(result.category, c.cat);
        REQUIRE_EQ(result.native_domain, std::string("storage"));
    }
}

TEST_CASE(map_legacy_error_persistence_exact_stable_mappings_1_to_19) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    struct { uint32_t code; StableErrorCode sc; ErrorCategory cat; } kPersistenceMap[19] = {
        { 1, StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
        { 2, StableErrorCode::unsupported_vhdx,           ErrorCategory::incompatible},
        { 3, StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
        { 4, StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
        { 5, StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
        { 6, StableErrorCode::bundle_locked,              ErrorCategory::conflict},
        { 7, StableErrorCode::operation_conflict,         ErrorCategory::conflict},
        { 8, StableErrorCode::operation_conflict,         ErrorCategory::conflict},
        { 9, StableErrorCode::operation_conflict,         ErrorCategory::conflict},
        {10, StableErrorCode::checkpoint_not_committed,   ErrorCategory::transient},
        {11, StableErrorCode::memory_pressure_blocked,    ErrorCategory::transient},
        {12, StableErrorCode::commit_indeterminate,       ErrorCategory::commit_indeterminate},
        {13, StableErrorCode::persistence_blocked,        ErrorCategory::persistence_blocked},
        {14, StableErrorCode::persistence_blocked,        ErrorCategory::persistence_blocked},
        {15, StableErrorCode::persistence_blocked,        ErrorCategory::persistence_blocked},
        {16, StableErrorCode::operation_cancelled,        ErrorCategory::cancelled},
        {17, StableErrorCode::operation_timeout,          ErrorCategory::timeout},
        {18, StableErrorCode::bundle_corrupt,             ErrorCategory::integrity},
        {19, StableErrorCode::internal_invariant_violation,ErrorCategory::fatal},
    };
    for (const auto& c : kPersistenceMap) {
        myrd::Error legacy{ED::persistence, c.code, L"op", L"persistence error"};
        auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE_EQ(result.stable_code, myrd::product::to_string(c.sc));
        REQUIRE_EQ(result.category, c.cat);
        REQUIRE_EQ(result.native_domain, std::string("persistence"));
        REQUIRE(myrd::product::has_complete_public_fields(result));
    }
}

TEST_CASE(map_legacy_error_native_code_preference_with_category_and_code) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::win32, 0, L"op", L"msg"};
    legacy.native_code = 5;
    auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.native_code, 5u);
    REQUIRE_EQ(result.stable_code, myrd::product::to_string(StableErrorCode::permission_denied));
    REQUIRE_EQ(result.category, ErrorCategory::permission);
}

TEST_CASE(map_legacy_error_preserves_message_as_utf8) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::usage, 0, L"op", L"résumé"};
    auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.message, std::string("r\xC3\xA9" "sum\xC3\xA9"));
}

TEST_CASE(map_legacy_error_common_error_unchanged_after_mapping) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::win32, 2, L"read", L"file not found"};
    legacy.native_code = 2;
    auto copy = legacy;
    /* result unused — only verifying legacy is unchanged */ (void)myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(legacy.domain, copy.domain);
    REQUIRE_EQ(legacy.code, copy.code);
    REQUIRE_EQ(legacy.operation, copy.operation);
    REQUIRE_EQ(legacy.message, copy.message);
    REQUIRE_EQ(legacy.native_code, copy.native_code);
}

TEST_CASE(map_legacy_error_stores_component_and_timestamp_exact) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1730000000);
    myrd::Error legacy{ED::usage, 0, L"op", L"msg"};
    auto result = myrd::product::map_legacy_error(legacy, "cli-core", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.component, std::string("cli-core"));
    REQUIRE_EQ(result.timestamp.time_since_epoch().count(), ts.time_since_epoch().count());
}

TEST_CASE(map_legacy_error_correlation_ids_forwarded) {
    using ED = myrd::ErrorDomain;
    auto req_id = Uuid::parse("a1b2c3d4-e5f6-4708-890a-bcdef0123456");
    auto op_id  = Uuid::parse("11111111-2222-3333-4444-555555555555");
    auto prof_id = Uuid::parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    REQUIRE(req_id.ok());
    REQUIRE(op_id.ok());
    REQUIRE(prof_id.ok());

    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::usage, 0, L"op", L"msg"};
    auto result = myrd::product::map_legacy_error(
        legacy, "comp", ts,
        req_id.value(), op_id.value(), prof_id.value());
    REQUIRE(result.request_id.has_value());
    REQUIRE(result.operation_id.has_value());
    REQUIRE(result.profile_id.has_value());
    REQUIRE_EQ(result.request_id->to_string(), std::string("a1b2c3d4-e5f6-4708-890a-bcdef0123456"));
    REQUIRE_EQ(result.operation_id->to_string(), std::string("11111111-2222-3333-4444-555555555555"));
    REQUIRE_EQ(result.profile_id->to_string(), std::string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
}

TEST_CASE(map_legacy_error_maps_ntstatus_access_denied_to_permission) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::ntstatus, 0xC0000022, L"op", L"access denied"};
    auto result = myrd::product::map_legacy_error(legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.stable_code, myrd::product::to_string(StableErrorCode::permission_denied));
    REQUIRE_EQ(result.category, ErrorCategory::permission);
    REQUIRE_EQ(result.native_domain, std::string("ntstatus"));
}

TEST_CASE(map_legacy_error_empty_message_still_has_complete_public_fields) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{ED::usage, 7, L"op", L""};
    auto result = myrd::product::map_legacy_error(
        legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE(!result.message.empty());
    REQUIRE(myrd::product::has_complete_public_fields(result));
}

TEST_CASE(map_legacy_error_unknown_domain_fails_closed) {
    using ED = myrd::ErrorDomain;
    auto ts = std::chrono::system_clock::from_time_t(1);
    myrd::Error legacy{static_cast<ED>(0xFF), 7, L"op", L"unknown domain"};
    auto result = myrd::product::map_legacy_error(
        legacy, "comp", ts, std::nullopt, std::nullopt, std::nullopt);
    REQUIRE_EQ(result.stable_code,
               myrd::product::to_string(StableErrorCode::internal_invariant_violation));
    REQUIRE_EQ(result.category, ErrorCategory::fatal);
    REQUIRE_EQ(result.native_domain, std::string("unknown"));
}

// ==========================================================================
// M4 Task 01 microtask 01.5: product state contract RED tests
// (requires src/product/product_state.h — currently missing → RED)
// ==========================================================================

using myrd::product::RuntimeState;
using myrd::product::ActivityState;
using myrd::product::PersistenceState;
using myrd::product::HealthState;
using myrd::product::DiskState;

// ---- static assertions: enum underlying types are uint8_t -----------------

static_assert(std::is_same_v<std::underlying_type_t<RuntimeState>, std::uint8_t>,
    "RuntimeState must be uint8_t");
static_assert(std::is_same_v<std::underlying_type_t<ActivityState>, std::uint8_t>,
    "ActivityState must be uint8_t");
static_assert(std::is_same_v<std::underlying_type_t<PersistenceState>, std::uint8_t>,
    "PersistenceState must be uint8_t");
static_assert(std::is_same_v<std::underlying_type_t<HealthState>, std::uint8_t>,
    "HealthState must be uint8_t");

// ---- static assertions: numeric values start at zero, declaration order ---

static_assert(static_cast<std::uint8_t>(RuntimeState::stopped) == 0);
static_assert(static_cast<std::uint8_t>(RuntimeState::starting) == 1);
static_assert(static_cast<std::uint8_t>(RuntimeState::running) == 2);
static_assert(static_cast<std::uint8_t>(RuntimeState::stopping) == 3);
static_assert(static_cast<std::uint8_t>(RuntimeState::orphaned) == 4);

static_assert(static_cast<std::uint8_t>(ActivityState::idle) == 0);
static_assert(static_cast<std::uint8_t>(ActivityState::provisioning) == 1);
static_assert(static_cast<std::uint8_t>(ActivityState::recovering) == 2);
static_assert(static_cast<std::uint8_t>(ActivityState::checkpointing) == 3);
static_assert(static_cast<std::uint8_t>(ActivityState::compacting) == 4);
static_assert(static_cast<std::uint8_t>(ActivityState::resolving_commit) == 5);

static_assert(static_cast<std::uint8_t>(PersistenceState::clean) == 0);
static_assert(static_cast<std::uint8_t>(PersistenceState::dirty) == 1);
static_assert(static_cast<std::uint8_t>(PersistenceState::persistence_blocked) == 2);
static_assert(static_cast<std::uint8_t>(PersistenceState::commit_indeterminate) == 3);

static_assert(static_cast<std::uint8_t>(HealthState::healthy) == 0);
static_assert(static_cast<std::uint8_t>(HealthState::degraded) == 1);
static_assert(static_cast<std::uint8_t>(HealthState::needs_attention) == 2);
static_assert(static_cast<std::uint8_t>(HealthState::fatal) == 3);

// ---- RuntimeState to_string -------------------------------------------------

TEST_CASE(state_runtime_to_string_exact) {
    struct { RuntimeState s; std::string_view expected; } cases[] = {
        {RuntimeState::stopped,  "Stopped"},
        {RuntimeState::starting, "Starting"},
        {RuntimeState::running,  "Running"},
        {RuntimeState::stopping, "Stopping"},
        {RuntimeState::orphaned, "Orphaned"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.s), c.expected);
    }
}

TEST_CASE(state_runtime_to_string_unknown_empty) {
    REQUIRE(myrd::product::to_string(static_cast<RuntimeState>(5)).empty());
    REQUIRE(myrd::product::to_string(static_cast<RuntimeState>(99)).empty());
}

// ---- RuntimeState is_valid --------------------------------------------------

TEST_CASE(state_runtime_is_valid_known) {
    RuntimeState known[] = {
        RuntimeState::stopped, RuntimeState::starting, RuntimeState::running,
        RuntimeState::stopping, RuntimeState::orphaned,
    };
    for (auto s : known) {
        REQUIRE(myrd::product::is_valid(s));
    }
}

TEST_CASE(state_runtime_is_valid_rejects_unknown) {
    REQUIRE(!myrd::product::is_valid(static_cast<RuntimeState>(5)));
    REQUIRE(!myrd::product::is_valid(static_cast<RuntimeState>(255)));
}

// ---- ActivityState to_string ------------------------------------------------

TEST_CASE(state_activity_to_string_exact) {
    struct { ActivityState s; std::string_view expected; } cases[] = {
        {ActivityState::idle,              "Idle"},
        {ActivityState::provisioning,      "Provisioning"},
        {ActivityState::recovering,        "Recovering"},
        {ActivityState::checkpointing,     "Checkpointing"},
        {ActivityState::compacting,        "Compacting"},
        {ActivityState::resolving_commit,  "ResolvingCommit"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.s), c.expected);
    }
}

TEST_CASE(state_activity_to_string_unknown_empty) {
    REQUIRE(myrd::product::to_string(static_cast<ActivityState>(6)).empty());
}

// ---- ActivityState is_valid -------------------------------------------------

TEST_CASE(state_activity_is_valid_known) {
    ActivityState known[] = {
        ActivityState::idle, ActivityState::provisioning, ActivityState::recovering,
        ActivityState::checkpointing, ActivityState::compacting, ActivityState::resolving_commit,
    };
    for (auto s : known) {
        REQUIRE(myrd::product::is_valid(s));
    }
}

TEST_CASE(state_activity_is_valid_rejects_unknown) {
    REQUIRE(!myrd::product::is_valid(static_cast<ActivityState>(6)));
    REQUIRE(!myrd::product::is_valid(static_cast<ActivityState>(255)));
}

// ---- PersistenceState to_string ---------------------------------------------

TEST_CASE(state_persistence_to_string_exact) {
    struct { PersistenceState s; std::string_view expected; } cases[] = {
        {PersistenceState::clean,                "Clean"},
        {PersistenceState::dirty,                "Dirty"},
        {PersistenceState::persistence_blocked,  "PersistenceBlocked"},
        {PersistenceState::commit_indeterminate, "CommitIndeterminate"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.s), c.expected);
    }
}

TEST_CASE(state_persistence_to_string_unknown_empty) {
    REQUIRE(myrd::product::to_string(static_cast<PersistenceState>(4)).empty());
}

// ---- PersistenceState is_valid ----------------------------------------------

TEST_CASE(state_persistence_is_valid_known) {
    PersistenceState known[] = {
        PersistenceState::clean, PersistenceState::dirty,
        PersistenceState::persistence_blocked, PersistenceState::commit_indeterminate,
    };
    for (auto s : known) {
        REQUIRE(myrd::product::is_valid(s));
    }
}

TEST_CASE(state_persistence_rejects_invalid_value_4) {
    REQUIRE(!myrd::product::is_valid(static_cast<PersistenceState>(4)));
    REQUIRE(!myrd::product::is_valid(static_cast<PersistenceState>(255)));
}

// ---- HealthState to_string --------------------------------------------------

TEST_CASE(state_health_to_string_exact) {
    struct { HealthState s; std::string_view expected; } cases[] = {
        {HealthState::healthy,         "Healthy"},
        {HealthState::degraded,        "Degraded"},
        {HealthState::needs_attention, "NeedsAttention"},
        {HealthState::fatal,           "Fatal"},
    };
    for (const auto& c : cases) {
        REQUIRE_EQ(myrd::product::to_string(c.s), c.expected);
    }
}

TEST_CASE(state_health_to_string_unknown_empty) {
    REQUIRE(myrd::product::to_string(static_cast<HealthState>(4)).empty());
}

// ---- HealthState is_valid ---------------------------------------------------

TEST_CASE(state_health_is_valid_known) {
    HealthState known[] = {
        HealthState::healthy, HealthState::degraded,
        HealthState::needs_attention, HealthState::fatal,
    };
    for (auto s : known) {
        REQUIRE(myrd::product::is_valid(s));
    }
}

TEST_CASE(state_health_is_valid_rejects_unknown) {
    REQUIRE(!myrd::product::is_valid(static_cast<HealthState>(4)));
    REQUIRE(!myrd::product::is_valid(static_cast<HealthState>(255)));
}

// ---- DiskState cross-product validity (dimensions orthogonal) ----------------

TEST_CASE(state_disk_state_cross_product_all_480_valid) {
    RuntimeState runtimes[] = {
        RuntimeState::stopped, RuntimeState::starting, RuntimeState::running,
        RuntimeState::stopping, RuntimeState::orphaned,
    };
    ActivityState activities[] = {
        ActivityState::idle, ActivityState::provisioning, ActivityState::recovering,
        ActivityState::checkpointing, ActivityState::compacting, ActivityState::resolving_commit,
    };
    PersistenceState persistences[] = {
        PersistenceState::clean, PersistenceState::dirty,
        PersistenceState::persistence_blocked, PersistenceState::commit_indeterminate,
    };
    HealthState healths[] = {
        HealthState::healthy, HealthState::degraded,
        HealthState::needs_attention, HealthState::fatal,
    };

    int count = 0;
    for (auto rt : runtimes) {
        for (auto act : activities) {
            for (auto pers : persistences) {
                for (auto hlth : healths) {
                    DiskState ds{rt, act, pers, hlth};
                    REQUIRE(myrd::product::is_valid(ds));
                    ++count;
                }
            }
        }
    }
    REQUIRE_EQ(count, 480);
}

TEST_CASE(state_disk_state_explicit_running_idle_persistence_blocked_degraded) {
    DiskState ds{
        RuntimeState::running,
        ActivityState::idle,
        PersistenceState::persistence_blocked,
        HealthState::degraded,
    };
    REQUIRE(myrd::product::is_valid(ds));
}

TEST_CASE(state_disk_state_is_valid_rejects_invalid_runtime) {
    DiskState ds{
        static_cast<RuntimeState>(99),
        ActivityState::idle,
        PersistenceState::clean,
        HealthState::healthy,
    };
    REQUIRE(!myrd::product::is_valid(ds));
}

TEST_CASE(state_disk_state_is_valid_rejects_invalid_persistence) {
    DiskState ds{
        RuntimeState::running,
        ActivityState::checkpointing,
        static_cast<PersistenceState>(254),
        HealthState::degraded,
    };
    REQUIRE(!myrd::product::is_valid(ds));
}

TEST_CASE(state_disk_state_to_string_invalid_returns_empty) {
    // Even one invalid field should fail is_valid
    DiskState ds{
        static_cast<RuntimeState>(7),
        ActivityState::idle,
        PersistenceState::dirty,
        HealthState::healthy,
    };
    REQUIRE(!myrd::product::is_valid(ds));
}

// ==========================================================================
// M4 Task 01 microtask 01.5: product version contract RED tests
// (requires src/product/product_version.h — currently missing → RED)
// ==========================================================================

// ---- version constants -------------------------------------------------------

static_assert(myrd::product::kProductVersionMajor == 1u);
static_assert(myrd::product::kProductVersionMinor == 0u);
static_assert(myrd::product::kProductVersionPatch == 0u);

TEST_CASE(version_kProductVersion_string) {
    REQUIRE_EQ(
        std::string_view(myrd::product::kProductVersion),
        std::string_view("1.0.0"));
}

TEST_CASE(version_constants_exact_values) {
    REQUIRE_EQ(myrd::product::kProductVersionMajor, 1u);
    REQUIRE_EQ(myrd::product::kProductVersionMinor, 0u);
    REQUIRE_EQ(myrd::product::kProductVersionPatch, 0u);
    REQUIRE_EQ(myrd::product::kCliSchemaVersion, 1u);
    REQUIRE_EQ(myrd::product::kPublicIpcVersionMajor, 1u);
    REQUIRE_EQ(myrd::product::kPublicIpcVersionMinor, 0u);
    REQUIRE_EQ(myrd::product::kPrivateIpcVersionMajor, 1u);
    REQUIRE_EQ(myrd::product::kPrivateIpcVersionMinor, 0u);
    REQUIRE_EQ(myrd::product::kProfileSchemaVersion, 1u);
    REQUIRE_EQ(myrd::product::kSkillVersion, 1u);
}

TEST_CASE(version_bundle_constants_alias_m3) {
    REQUIRE_EQ(
        myrd::product::kBundleFormatMajor,
        myrd::persistence::kM3FormatMajor);
    REQUIRE_EQ(
        myrd::product::kBundleFormatMinor,
        myrd::persistence::kM3FormatMinor);
}

// ---- static assert: kBundleFormat aliases are exact same values --------------

static_assert(myrd::product::kBundleFormatMajor == myrd::persistence::kM3FormatMajor);
static_assert(myrd::product::kBundleFormatMinor == myrd::persistence::kM3FormatMinor);

// ==========================================================================
// M4 Task 01 microtask 01.5: NotCommitted proof
// ==========================================================================

// Prove persistence::CommitDisposition::NotCommitted exists and is wire 0
static_assert(
    static_cast<std::uint8_t>(myrd::persistence::CommitDisposition::NotCommitted) == 0);

TEST_CASE(notcommitted_persistence_to_string_is_NotCommitted) {
    REQUIRE_EQ(
        myrd::persistence::to_string(myrd::persistence::CommitDisposition::NotCommitted),
        std::string_view("NotCommitted"));
}

// Prove product PersistenceState has exactly four known values;
// invalid value 4 (and beyond) is rejected.  Do NOT reference a nonexistent
// enumerator (e.g. NotCommitted) on the product-side enum or use source-text
// scanning.

TEST_CASE(state_persistence_exactly_four_known_values_no_fifth) {
    // The four known values are all valid
    REQUIRE(myrd::product::is_valid(PersistenceState::clean));
    REQUIRE(myrd::product::is_valid(PersistenceState::dirty));
    REQUIRE(myrd::product::is_valid(PersistenceState::persistence_blocked));
    REQUIRE(myrd::product::is_valid(PersistenceState::commit_indeterminate));

    // An out-of-range value (4) is not valid
    REQUIRE(!myrd::product::is_valid(static_cast<PersistenceState>(4)));

    // to_string also rejects it
    REQUIRE(myrd::product::to_string(static_cast<PersistenceState>(4)).empty());
}
