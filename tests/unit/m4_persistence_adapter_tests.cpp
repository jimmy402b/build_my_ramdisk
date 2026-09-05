#include "tests/support/test_framework.h"

#include "src/run/m4_persistence_adapter.h"

#include <filesystem>

namespace {

using myrd::M4PersistenceAdapter;
using myrd::M4PersistenceOptions;

TEST_CASE(m4_persistence_adapter_rejects_invalid_geometry_before_create) {
    M4PersistenceOptions options{};
    options.bundle_root = std::filesystem::temp_directory_path() /
        L"myrd-m4-adapter-invalid";
    options.virtual_bytes = 0;
    M4PersistenceAdapter adapter(options);

    const auto result = adapter.create_generation_zero(myrd::OperationContext{});

    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, myrd::ErrorDomain::usage);
}

TEST_CASE(m4_persistence_adapter_rejects_recovery_geometry_mismatch) {
    M4PersistenceOptions options{};
    options.bundle_root = std::filesystem::temp_directory_path() /
        L"myrd-m4-adapter-missing";
    options.virtual_bytes = 8 * 1024 * 1024;
    M4PersistenceAdapter adapter(options);

    const auto result = adapter.recover_complete(myrd::OperationContext{});

    REQUIRE(!result.ok());
}

TEST_CASE(m4_persistence_adapter_exposes_offline_compaction_outcome) {
    M4PersistenceOptions options{};
    options.bundle_root = std::filesystem::temp_directory_path() /
        L"myrd-m4-adapter-compact-missing";
    M4PersistenceAdapter adapter(options);

    const auto outcome = adapter.compact_offline(myrd::OperationContext{});

    REQUIRE(outcome.exit_code != 0);
    REQUIRE(outcome.error.has_value());
}

} // namespace

