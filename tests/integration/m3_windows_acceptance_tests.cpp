#include "test_framework.h"

#include "src/m3/m3_windows_acceptance.h"

#include <chrono>

namespace {

using namespace myrd;
using namespace myrd::m3;

TEST_CASE(m3_production_store_policy_preserves_named_resource_limits) {
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    const auto options = production_store_options();
    REQUIRE_EQ(options.soft_limit_bytes, 512ULL * mib);
    REQUIRE_EQ(options.hard_limit_bytes, 1'024ULL * mib);
    REQUIRE_EQ(options.eviction_low_watermark_bytes, 256ULL * mib);
    REQUIRE_EQ(options.max_snapshot_batch_bytes, 8ULL * mib);
    REQUIRE_EQ(options.max_snapshot_batch_records, 8'192U);
    REQUIRE_EQ(options.lock_stripe_count, 256U);
    REQUIRE_EQ(options.snapshot_cow_max_bytes, 64ULL * mib);
    REQUIRE_EQ(
        options.snapshot_writer_stall_limit,
        std::chrono::milliseconds(250));
}

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

TEST_CASE(m3_aim_runtime_exposes_zero_safe_metrics_before_start) {
    M3AimProviderFactoryConfig config;
    auto runtime = M3AimRuntime::create(config);

    REQUIRE(runtime);
    const auto storage = runtime.value()->storage_stats();
    const auto transport = runtime.value()->transport_snapshot();

    REQUIRE_EQ(storage.logical_size_bytes, 0U);
    REQUIRE_EQ(storage.committed_bytes, 0U);
    REQUIRE_EQ(storage.resident_dirty_blocks, 0U);
    REQUIRE_EQ(transport.requests.read, 0U);
    REQUIRE_EQ(transport.requests.write, 0U);
}

} // namespace
