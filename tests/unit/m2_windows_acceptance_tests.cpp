#include "test_framework.h"

#include "src/m2/m2_windows_acceptance.h"
#include "src/storage/block_constants.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <iostream>
#include <cwctype>

namespace {

myrd::M2GeometryOptions geometry() {
    myrd::M2GeometryOptions value;
    value.disk_size_bytes = 64ull << 30;
    value.sector_size_bytes = 512;
    value.block_size_bytes = myrd::kBlockSizeBytes;
    value.soft_limit_bytes = 16ull << 30;
    value.hard_limit_bytes = 32ull << 30;
    value.eviction_low_watermark_bytes = 12ull << 30;
    value.max_snapshot_batch_bytes = 64ull << 20;
    value.max_snapshot_batch_records = 1024;
    value.lock_stripe_count = 64;
    value.test_policy_selected = true;
    value.backing_kind = "zero";
    return value;
}

myrd::M2BackingState empty_backing() {
    myrd::M2BackingState value;
    value.initial_state = myrd::M2InitialState::empty;
    value.initial_backing_generation = 0;
    value.current_backing_generation = 0;
    return value;
}

} // namespace

TEST_CASE(m2_real_provider_config_maps_exact_cli_geometry_to_clean_zero_store) {
    const auto expected = geometry();
    auto mapped = myrd::make_m2_real_provider_config(
        expected, empty_backing());

    REQUIRE(mapped.ok());
    REQUIRE_EQ(mapped.value().store.logical_size_bytes,
               expected.disk_size_bytes);
    REQUIRE_EQ(mapped.value().store.sector_size_bytes,
               expected.sector_size_bytes);
    REQUIRE_EQ(mapped.value().store.options.soft_limit_bytes,
               expected.soft_limit_bytes);
    REQUIRE_EQ(mapped.value().store.options.hard_limit_bytes,
               expected.hard_limit_bytes);
    REQUIRE_EQ(mapped.value().store.options.eviction_low_watermark_bytes,
               expected.eviction_low_watermark_bytes);
    REQUIRE_EQ(mapped.value().store.options.max_snapshot_batch_bytes,
               expected.max_snapshot_batch_bytes);
    REQUIRE_EQ(mapped.value().store.options.max_snapshot_batch_records,
               expected.max_snapshot_batch_records);
    REQUIRE_EQ(mapped.value().store.options.lock_stripe_count,
               expected.lock_stripe_count);
    REQUIRE_EQ(mapped.value().store.initial_state,
               myrd::InitialBlockState::CleanZero);
    REQUIRE_EQ(mapped.value().store.initial_backing_generation, 0u);
    REQUIRE(!mapped.value().store.backing_store);
}

TEST_CASE(m2_real_provider_config_rejects_non_m2_or_durable_backing_contracts) {
    auto wrong_geometry = geometry();
    wrong_geometry.disk_size_bytes = 32ull << 30;
    REQUIRE(myrd::make_m2_real_provider_config(
                wrong_geometry, empty_backing()).has_error());

    auto backing = empty_backing();
    backing.initial_state = myrd::M2InitialState::seeded;
    REQUIRE(myrd::make_m2_real_provider_config(
                geometry(), backing).has_error());

    backing = empty_backing();
    backing.durable_successor_installed = true;
    REQUIRE(myrd::make_m2_real_provider_config(
                geometry(), backing).has_error());
}

TEST_CASE(m2_real_provider_runtime_warmup_is_handle_stable_when_repeated) {
    REQUIRE(myrd::warm_m2_provider_runtime_for_acceptance(
                geometry(), empty_backing()).ok());
    DWORD after_first = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after_first));

    REQUIRE(myrd::warm_m2_provider_runtime_for_acceptance(
                geometry(), empty_backing()).ok());
    DWORD after_second = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after_second));

    REQUIRE_EQ(after_second, after_first);
}

TEST_CASE(m2_real_child_process_uses_an_existing_absolute_system_powershell) {
    auto executable = myrd::m2_system_powershell_path();
    REQUIRE(executable.ok());
    REQUIRE(executable.value().is_absolute());
    REQUIRE(std::filesystem::is_regular_file(executable.value()));
    auto filename = executable.value().filename().wstring();
    for (auto& ch : filename) ch = static_cast<wchar_t>(std::towlower(ch));
    REQUIRE_EQ(filename, std::wstring(L"powershell.exe"));
}

TEST_CASE(m2_service_restore_is_a_no_op_when_native_state_already_matches) {
    REQUIRE(!myrd::m2_service_restoration_required(
        SERVICE_RUNNING,
        SERVICE_AUTO_START,
        SERVICE_RUNNING,
        SERVICE_AUTO_START));
    REQUIRE(myrd::m2_service_restoration_required(
        SERVICE_STOPPED,
        SERVICE_AUTO_START,
        SERVICE_RUNNING,
        SERVICE_AUTO_START));
    REQUIRE(myrd::m2_service_restoration_required(
        SERVICE_RUNNING,
        SERVICE_DEMAND_START,
        SERVICE_RUNNING,
        SERVICE_AUTO_START));
}

TEST_CASE(m2_final_resource_sampling_retries_only_bounded_growth) {
    myrd::M2InventorySnapshot before;
    before.process_handle_count = 119;
    before.process_thread_count = 4;
    before.process_private_bytes = 2ULL << 20U;
    auto after = before;

    REQUIRE(!myrd::m2_should_retry_process_resource_sample(
        before, after, 1ULL << 20U, 0));

    after.process_handle_count += 1;
    REQUIRE(myrd::m2_should_retry_process_resource_sample(
        before, after, 1ULL << 20U, 0));
    REQUIRE(myrd::m2_should_retry_process_resource_sample(
        before, after, 1ULL << 20U, 19));
    REQUIRE(!myrd::m2_should_retry_process_resource_sample(
        before, after, 1ULL << 20U, 20));

    after = before;
    after.process_thread_count -= 1;
    REQUIRE(!myrd::m2_should_retry_process_resource_sample(
        before, after, 1ULL << 20U, 0));
}

TEST_CASE(m2_real_normalizes_only_the_pinned_aim_descriptor_identity) {
    auto aim = myrd::normalize_m2_aim_descriptor_bus_type(
        6u, "Arsenal", "Virtual");
    REQUIRE(aim.ok());
    REQUIRE_EQ(aim.value(), std::string("Fibre Channel"));

    REQUIRE(myrd::normalize_m2_aim_descriptor_bus_type(
                6u, "Other", "Virtual").has_error());
    REQUIRE(myrd::normalize_m2_aim_descriptor_bus_type(
                17u, "Arsenal", "Virtual").has_error());
    REQUIRE(myrd::normalize_m2_aim_descriptor_bus_type(
                1u, "Arsenal", "Virtual").has_error());
}

TEST_CASE(m2_real_requires_explicit_delete_notify_and_retrim_evidence) {
    const auto valid = myrd::validate_m2_delete_retrim_state(R"json({
        "phase": "retrim_complete",
        "bytes_deleted": 2147483648,
        "delete_notify_enabled": true,
        "retrim_succeeded": true
    })json");
    REQUIRE(valid.ok());
    REQUIRE_EQ(valid.value(), 2147483648ull);

    for (const auto invalid : {
             R"json({"phase":"retrim_complete","bytes_deleted":2147483648,"delete_notify_enabled":false,"retrim_succeeded":true})json",
             R"json({"phase":"retrim_complete","bytes_deleted":2147483648,"delete_notify_enabled":true,"retrim_succeeded":false})json",
             R"json({"phase":"retrim_complete","bytes_deleted":2147483648,"delete_notify_enabled":"true","retrim_succeeded":true})json",
             R"json({"phase":"setup_complete","bytes_deleted":2147483648,"delete_notify_enabled":true,"retrim_succeeded":true})json",
             R"json({"phase":"retrim_complete","bytes_deleted":536870912,"delete_notify_enabled":true,"retrim_succeeded":true})json"}) {
        REQUIRE(myrd::validate_m2_delete_retrim_state(invalid).has_error());
    }
}

TEST_CASE(m2_real_missing_report_preserves_the_original_lifecycle_failure) {
    myrd::M2Report report;
    report.failures.push_back({
        "capture_baseline", 0xE055u, "original baseline failure"});
    const auto missing = std::filesystem::temp_directory_path() /
        L"myrd-m2-intentionally-missing-report.json";

    auto finalized = myrd::finalize_m2_windows_acceptance(
        std::move(report), missing);

    REQUIRE(finalized.has_error());
    REQUIRE_EQ(finalized.error().code, 0xE055u);
    REQUIRE(
        finalized.error().message.find(L"original baseline failure") !=
        std::wstring::npos);
}

int main() {
    for (const auto& test : myrd::test::registry()) {
        test.func();
        std::cout << "PASS " << test.name << '\n';
    }
    return 0;
}
