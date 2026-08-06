#include "test_framework.h"

#include "src/m3/m3_windows_acceptance.h"

#include <chrono>

namespace {

using namespace myrd;
using namespace myrd::m3;

TEST_CASE(m3_aim_factory_checks_operation_context_before_open) {
    M3AimProviderFactoryConfig config;
    config.context.deadline = std::chrono::steady_clock::now() -
        std::chrono::seconds(1);
    config.provider.bundle_root = L"C:\\this-bundle-must-not-be-opened";
    config.provider.manifest_name = "manifest.json";

    M3AimProviderFactory factory(std::move(config));
    const auto created = factory.create();

    REQUIRE(!created);
    REQUIRE_EQ(created.error().operation, L"check_operation_context");
}

TEST_CASE(m3_aim_runtime_checks_context_before_driver_discovery) {
    M3AimProviderFactoryConfig config;
    config.context.deadline = std::chrono::steady_clock::now() -
        std::chrono::seconds(1);
    config.provider.bundle_root = L"C:\\this-bundle-must-not-be-opened";
    config.provider.manifest_name = "manifest.json";

    auto runtime = M3AimRuntime::create(config);
    REQUIRE(runtime);
    const auto started = runtime.value()->start();

    REQUIRE(!started);
    REQUIRE_EQ(started.error().operation, L"check_operation_context");
    REQUIRE(!runtime.value()->started());
}

TEST_CASE(m3_aim_runtime_rejects_non_positive_device_timeout) {
    M3AimProviderFactoryConfig config;
    const auto runtime = M3AimRuntime::create(config, std::chrono::milliseconds(0));

    REQUIRE(!runtime);
    REQUIRE_EQ(runtime.error().operation, L"M3AimRuntime::create");
}

} // namespace
