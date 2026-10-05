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

TEST_CASE(m4_persistence_adapter_creates_generation_zero_with_the_named_image) {
    // The service derives the image id from the create request *before* the
    // bundle exists and validates the recorded evidence against it afterwards,
    // so a create that generated its own identity could never be registered --
    // the evidence would name an image the service did not ask for.  A caller
    // that names one gets exactly it, and recovery re-reads the same identity
    // from the bundle's own bytes rather than from anything the caller
    // remembers.
    const auto bundle = std::filesystem::temp_directory_path() /
        L"myrd-m4-adapter-named-image";
    std::error_code ignored;
    std::filesystem::remove_all(bundle, ignored);
    // The adapter claims a manifest *inside* a bundle directory; making one is
    // the caller's job (`ProductionRuntimeAdapter::create_generation_zero` does
    // exactly this), and a generation-zero request against a missing directory
    // fails on its first query rather than creating one.
    std::filesystem::create_directories(bundle, ignored);

    // A recognizable identity rather than a random one, so the assertion is
    // about the named value and not about two random draws differing.
    myrd::persistence::ImageUuid expected{};
    expected.bytes[0] = std::byte{0x01};
    expected.bytes[7] = std::byte{0x8A};
    expected.bytes[15] = std::byte{0xEF};

    M4PersistenceOptions options{};
    options.bundle_root = bundle;
    options.virtual_bytes = 8 * 1024 * 1024;
    options.image_uuid = expected;
    M4PersistenceAdapter adapter(options);

    const auto created = adapter.create_generation_zero(myrd::OperationContext{});
    REQUIRE(created.ok());
    REQUIRE(created.value().image_uuid == expected);

    const auto recovered = adapter.recover_complete(myrd::OperationContext{});
    REQUIRE(recovered.ok());
    REQUIRE(recovered.value().image_uuid == expected);

    std::filesystem::remove_all(bundle, ignored);
}

} // namespace

