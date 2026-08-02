#include "test_framework.h"

#include "src/m1/m1_lifecycle_runner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using myrd::Error;
using myrd::ErrorDomain;
using myrd::Result;

Error failure(const wchar_t* operation) {
    return {ErrorDomain::internal, 0xD001, operation, L"injected", 55};
}

struct FakePreflight final : myrd::IM1ExecutionGuard {
    bool ok = true;
    int calls = 0;
    Result<void> require_elevated_and_acquire() override {
        ++calls;
        return ok ? Result<void>::success()
                  : Result<void>::failure(failure(L"preflight"));
    }
};

struct FakeProvider final : myrd::IM1ProviderLifecycle {
    bool start_ok = true;
    bool stop_ok = true;
    bool released = true;
    int start_calls = 0;
    int stop_calls = 0;
    myrd::proxy::SessionSnapshot session{};

    FakeProvider() {
        session.state = myrd::proxy::SessionState::serving;
        session.requests = {.info=1, .read=3, .write=3, .unmap=1,
                            .zero=1, .close=1};
    }
    Result<void> start() override {
        ++start_calls;
        return start_ok ? Result<void>::success()
                        : Result<void>::failure(failure(L"provider_start"));
    }
    Result<void> stop_and_release() override {
        ++stop_calls;
        return stop_ok ? Result<void>::success()
                       : Result<void>::failure(failure(L"provider_stop"));
    }
    std::wstring native_object_path() const override {
        return L"\\BaseNamedObjects\\Global\\myrd-m1-0123456789abcdef0123456789abcdef";
    }
    myrd::proxy::SessionSnapshot snapshot() const override { return session; }
    Result<bool> named_objects_released() const override {
        return Result<bool>::success(released);
    }
};

struct FakeProviderFactory final : myrd::IM1ProviderFactory {
    FakeProvider* created = nullptr;
    bool create_ok = true;
    Result<std::unique_ptr<myrd::IM1ProviderLifecycle>> create() override {
        if (!create_ok) {
            return Result<std::unique_ptr<myrd::IM1ProviderLifecycle>>::failure(
                failure(L"provider_create"));
        }
        auto provider = std::make_unique<FakeProvider>();
        created = provider.get();
        return Result<std::unique_ptr<myrd::IM1ProviderLifecycle>>::success(
            std::move(provider));
    }
};

struct FakeKernel final : myrd::IM1KernelAdapter {
    std::vector<std::string> log;
    std::vector<myrd::aim::DeviceId> before{{1}, {2}};
    std::vector<myrd::aim::DeviceId> after{{2}, {1}};
    int list_calls = 0;
    bool driver_ok = true;
    bool create_ok = true;
    bool query_ok = true;
    bool remove_ok = true;
    bool absent_ok = true;
    myrd::aim::DeviceId created{0x00030201};

    Result<myrd::M1DriverEvidence> query_driver() override {
        log.push_back("driver");
        if (!driver_ok) return Result<myrd::M1DriverEvidence>::failure(
            failure(L"query_driver"));
        myrd::M1DriverEvidence evidence;
        evidence.service_name = "phdskmnt";
        evidence.service_state = "Running";
        evidence.api_version = 0x101;
        evidence.sub_version = 0x1234;
        return Result<myrd::M1DriverEvidence>::success(std::move(evidence));
    }
    Result<std::vector<myrd::aim::DeviceId>> list_devices() override {
        log.push_back("list");
        return Result<std::vector<myrd::aim::DeviceId>>::success(
            list_calls++ == 0 ? before : after);
    }
    Result<myrd::aim::DeviceId> create_proxy_disk(
        const myrd::aim::ProxyDiskSpec&) override {
        log.push_back("create");
        return create_ok
            ? Result<myrd::aim::DeviceId>::success(created)
            : Result<myrd::aim::DeviceId>::failure(failure(L"create"));
    }
    Result<void> rescan_adapter() override {
        log.push_back("rescan"); return Result<void>::success();
    }
    Result<myrd::aim::DiskInfo> query_device(
        myrd::aim::DeviceId id) override {
        log.push_back("query");
        if (!query_ok) return Result<myrd::aim::DiskInfo>::failure(
            failure(L"query_device"));
        return Result<myrd::aim::DiskInfo>::success({
            id, myrd::aim::kM0DiskSize, myrd::aim::kM0SectorSize,
            myrd::aim::kM1QueryFlags,
            L"\\BaseNamedObjects\\Global\\myrd-m1-0123456789abcdef0123456789abcdef"});
    }
    Result<void> remove_device(myrd::aim::DeviceId id) override {
        log.push_back("remove:" + std::to_string(id.value));
        return remove_ok ? Result<void>::success()
                         : Result<void>::failure(failure(L"remove"));
    }
    Result<void> wait_until_absent(
        myrd::aim::DeviceId, std::chrono::milliseconds) override {
        log.push_back("absent");
        return absent_ok ? Result<void>::success()
                         : Result<void>::failure(failure(L"absent"));
    }
};

struct FakeLocator final : myrd::IM1DiskLocator {
    std::vector<std::uint32_t> before{0, 4};
    std::vector<std::uint32_t> after{4, 0};
    int snapshots = 0;
    bool locate_ok = true;
    Result<std::vector<std::uint32_t>> snapshot_numbers() override {
        return Result<std::vector<std::uint32_t>>::success(
            snapshots++ == 0 ? before : after);
    }
    Result<myrd::aim::LocatedDisk> wait_for(
        myrd::aim::DeviceId, std::span<const std::uint32_t>) override {
        return locate_ok
            ? Result<myrd::aim::LocatedDisk>::success(
                  {7, L"\\\\.\\PhysicalDrive7", myrd::aim::kM0DiskSize,
                   1, 2, 3})
            : Result<myrd::aim::LocatedDisk>::failure(failure(L"locate"));
    }
};

struct FakeProtocol final : myrd::IM1ProtocolVerifier {
    bool ok = true;
    int calls = 0;
    Result<myrd::M1RawProtocolEvidence> verify(
        const myrd::aim::LocatedDisk&,
        const myrd::proxy::RequestCounters&,
        const std::function<myrd::proxy::RequestCounters()>&) override {
        ++calls;
        if (!ok) return Result<myrd::M1RawProtocolEvidence>::failure(
            failure(L"raw_protocol"));
        return Result<myrd::M1RawProtocolEvidence>::success({
            8ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull,
            "AA", "AA", true, true, true});
    }
};

myrd::M1RunConfig config(myrd::M1FailurePoint point =
                             myrd::M1FailurePoint::none) {
    myrd::M1RunConfig result;
    result.run_id = "test-run";
    result.started_utc = "2026-08-03T00:00:00Z";
    result.failure_point = point;
    result.cleanup_timeout = std::chrono::milliseconds(10);
    result.poll_interval = std::chrono::milliseconds(1);
    return result;
}

myrd::M1Report run(FakePreflight& guard, FakeProviderFactory& providers,
                   FakeKernel& kernel, FakeLocator& locator,
                   FakeProtocol& protocol,
                   myrd::M1RunConfig cfg = config()) {
    return myrd::run_m1_lifecycle(
        guard, providers, kernel, locator, protocol, cfg);
}

} // namespace

TEST_CASE(m1_query_contract_includes_driver_supports_zero_marker) {
    REQUIRE_EQ(myrd::aim::kM1QueryFlags, 0x00000314u);
    REQUIRE_EQ(myrd::aim::kM0QueryFlags, 0x00000310u);
}

TEST_CASE(m1_lifecycle_success_uses_exact_order_and_proves_cleanup) {
    FakePreflight guard; FakeProviderFactory providers; FakeKernel kernel;
    FakeLocator locator; FakeProtocol protocol;
    auto report = run(guard, providers, kernel, locator, protocol);
    REQUIRE(report.success);
    REQUIRE_EQ(report.exit_code, 0);
    REQUIRE(report.created_device.has_value());
    REQUIRE(report.created_device->confirmed_by_query_device);
    REQUIRE(report.raw_protocol.has_value());
    REQUIRE(report.raw_protocol->zero_write_verified);
    REQUIRE(report.raw_protocol->unmap_verified);
    REQUIRE(report.cleanup.exact_remove_attempted);
    REQUIRE(report.cleanup.exact_remove_succeeded);
    REQUIRE(report.cleanup.device_disappeared);
    REQUIRE(report.cleanup.provider_stopped);
    REQUIRE(report.cleanup.named_objects_released);
    REQUIRE(report.cleanup.aim_device_lists_match);
    REQUIRE(report.cleanup.physical_drive_lists_match);
    REQUIRE(kernel.log == std::vector<std::string>({
        "list", "driver", "create", "rescan", "query",
        "remove:197121", "absent", "list"}));
}

TEST_CASE(m1_lifecycle_rejects_sentinel_owned_id_without_removing_any_device) {
    FakePreflight guard; FakeProviderFactory providers; FakeKernel kernel;
    FakeLocator locator; FakeProtocol protocol;
    kernel.created = {myrd::aim::kAllDevices};
    auto report = run(guard, providers, kernel, locator, protocol);
    REQUIRE(!report.success);
    REQUIRE(report.primary_error.has_value());
    REQUIRE(!report.cleanup.exact_remove_attempted);
    REQUIRE(kernel.log.end() == std::find(
        kernel.log.begin(), kernel.log.end(), "remove:16777215"));
}

TEST_CASE(m1_lifecycle_failures_after_create_remove_only_owned_id) {
    for (int failure_index = 0; failure_index < 3; ++failure_index) {
        FakePreflight guard; FakeProviderFactory providers; FakeKernel kernel;
        FakeLocator locator; FakeProtocol protocol;
        if (failure_index == 0) kernel.query_ok = false;
        if (failure_index == 1) locator.locate_ok = false;
        if (failure_index == 2) protocol.ok = false;
        auto report = run(guard, providers, kernel, locator, protocol);
        REQUIRE(!report.success);
        REQUIRE(report.primary_error.has_value());
        REQUIRE(report.cleanup.exact_remove_attempted);
        REQUIRE(report.cleanup.exact_remove_succeeded);
        REQUIRE(report.cleanup.removed_device_id == "197121");
    }
}

TEST_CASE(m1_lifecycle_primary_error_survives_cleanup_errors) {
    FakePreflight guard; FakeProviderFactory providers; FakeKernel kernel;
    FakeLocator locator; FakeProtocol protocol;
    protocol.ok = false;
    kernel.remove_ok = false;
    auto report = run(guard, providers, kernel, locator, protocol);
    REQUIRE(!report.success);
    REQUIRE(report.primary_error.has_value());
    REQUIRE(report.primary_error->operation == "raw_protocol");
    REQUIRE(!report.cleanup_errors.empty());
    REQUIRE(report.cleanup_errors.front().operation == "remove");
}

TEST_CASE(m1_lifecycle_all_debug_failure_points_use_common_cleanup) {
    for (const auto point : {
             myrd::M1FailurePoint::provider_ready,
             myrd::M1FailurePoint::create_returned,
             myrd::M1FailurePoint::device_confirmed,
             myrd::M1FailurePoint::physical_drive_confirmed,
             myrd::M1FailurePoint::write_verified,
             myrd::M1FailurePoint::zero_verified,
             myrd::M1FailurePoint::unmap_verified,
             myrd::M1FailurePoint::device_removed}) {
        FakePreflight guard; FakeProviderFactory providers; FakeKernel kernel;
        FakeLocator locator; FakeProtocol protocol;
        auto report = run(guard, providers, kernel, locator, protocol,
                          config(point));
        REQUIRE(!report.success);
        REQUIRE(report.primary_error.has_value());
        REQUIRE(report.cleanup.provider_stopped);
        REQUIRE(report.cleanup.named_objects_released);
        REQUIRE(report.cleanup.aim_device_lists_match);
        REQUIRE(report.cleanup.physical_drive_lists_match);
        if (point != myrd::M1FailurePoint::provider_ready) {
            REQUIRE(report.cleanup.exact_remove_succeeded);
        }
    }
}
