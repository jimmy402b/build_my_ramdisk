#include "test_framework.h"
#include "src/poc/lifecycle_runner.h"
#include "src/poc/m0_provider.h"
#include "src/poc/windows_raw_disk_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

static const myrd::aim::DeviceId kFakeId{42};

struct FakeProv : myrd::poc::IProviderLifecycle {
    int start_c = 0, stop_c = 0; bool rel = true, stop_ok = true;
    mutable int release_checks = 0;
    mutable int transient_release_checks = 0;
    myrd::proxy::RequestCounters request_counters{0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    myrd::Result<void> start() override { start_c++; return myrd::Result<void>::success(); }
    myrd::Result<void> stop_and_release() override { stop_c++; if (stop_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::internal,0,L"stop_and_release",L"failed"}); }
    std::wstring native_object_path() const override { return L"\\BaseNamedObjects\\Global\\myrd-m0-0123456789abcdef0123456789abcdef"; }
    myrd::proxy::RequestCounters counters() const override { return request_counters; }
    myrd::Result<bool> named_objects_released() const override {
        ++release_checks;
        if (transient_release_checks-- > 0) {
            return myrd::Result<bool>::success(false);
        }
        return myrd::Result<bool>::success(rel);
    }
};

struct FakeAd : myrd::poc::IKernelDiskAdapter {
    std::vector<std::string> log;
    std::vector<myrd::aim::DeviceId> devices_before;
    std::vector<myrd::aim::DeviceId> devices_after;
    int list_calls = 0;
    bool c_ok = true, q_ok = true, r_ok = true, rs_ok = true,
         absent_ok = true;
    std::uint32_t query_flags =
        myrd::aim::kDeviceTypeHd | myrd::aim::kTypeProxy;
    std::wstring query_path =
        L"\\BaseNamedObjects\\Global\\myrd-m0-0123456789abcdef0123456789abcdef";
    myrd::Result<std::vector<myrd::aim::DeviceId>> list_devices() override { return myrd::Result<std::vector<myrd::aim::DeviceId>>::success(list_calls++ == 0 ? devices_before : devices_after); }
    myrd::Result<myrd::aim::DeviceId> create_proxy_disk(const myrd::aim::ProxyDiskSpec&) override { log.push_back("create"); if (c_ok) return myrd::Result<myrd::aim::DeviceId>::success(kFakeId); return myrd::Result<myrd::aim::DeviceId>::failure({myrd::ErrorDomain::protocol,0,L"",L""}); }
    myrd::Result<void> rescan_adapter() { log.push_back("rescan"); if (rs_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::win32,0,L"",L""}); }
    myrd::Result<myrd::aim::DiskInfo> query_device(myrd::aim::DeviceId) override { log.push_back("query"); if (!q_ok) return myrd::Result<myrd::aim::DiskInfo>::failure({myrd::ErrorDomain::protocol,0,L"query_device",L"failed"}); myrd::aim::DiskInfo di{kFakeId, 64ull*1024*1024, 512, query_flags, query_path}; return myrd::Result<myrd::aim::DiskInfo>::success(di); }
    myrd::Result<void> remove_device(myrd::aim::DeviceId id) override { log.push_back("remove:"+std::to_string(id.value)); if (r_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::protocol,0,L"",L""}); }
    myrd::Result<void> wait_until_absent(myrd::aim::DeviceId, std::chrono::milliseconds) override { log.push_back("absent"); if (absent_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::evidence,0,L"wait_until_absent",L"timeout"}); }
};

struct FakeLoc : myrd::poc::IDiskLocator {
    bool ok = true;
    std::vector<std::uint32_t> numbers_before;
    std::vector<std::uint32_t> numbers_after;
    int snapshot_calls = 0;
    int transient_post_snapshot_failures = 0;
    myrd::Result<std::vector<std::uint32_t>> snapshot_numbers() override { if (snapshot_calls++ == 0) return myrd::Result<std::vector<std::uint32_t>>::success(numbers_before); if (transient_post_snapshot_failures-- > 0) return myrd::Result<std::vector<std::uint32_t>>::failure({myrd::ErrorDomain::win32, ERROR_FILE_NOT_FOUND, L"snapshot_numbers", L"transient disappearing interface"}); return myrd::Result<std::vector<std::uint32_t>>::success(numbers_after); }
    myrd::Result<myrd::aim::LocatedDisk> wait_for(myrd::aim::DeviceId, std::span<const std::uint32_t>) override { if (ok) return myrd::Result<myrd::aim::LocatedDisk>::success(myrd::aim::LocatedDisk{5, L"\\\\.\\PhysicalDrive5", 64ull*1024*1024}); return myrd::Result<myrd::aim::LocatedDisk>::failure({myrd::ErrorDomain::evidence,0,L"",L""}); }
};

struct FakeRT : myrd::poc::IRoundTrip {
    bool ok = true;
    myrd::Result<myrd::poc::RawRoundTripEvidence> run(const myrd::aim::LocatedDisk&) override { if (ok) { myrd::poc::RawRoundTripEvidence rt{}; rt.offset=8*1024*1024; rt.length=4096; rt.written_sha256="AAA"; rt.read_sha256="AAA"; rt.flush_succeeded=true; return myrd::Result<myrd::poc::RawRoundTripEvidence>::success(rt); } return myrd::Result<myrd::poc::RawRoundTripEvidence>::failure({myrd::ErrorDomain::internal,0,L"",L""}); }
};

// 1. Success path
TEST_CASE(lifecycle_success_path) {
    FakeProv p; FakeAd a; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t1";
    cfg.started_utc = "2026-08-01T12:00:00Z";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.success);
    REQUIRE_EQ(r.started_utc, cfg.started_utc);
    REQUIRE(r.cleanup.exact_remove_succeeded);
    REQUIRE(r.cleanup.device_disappeared);
    REQUIRE_EQ(r.provider.session_id,
               std::string("0123456789abcdef0123456789abcdef"));
    REQUIRE(a.log.size() >= 5u);
    REQUIRE_EQ(a.log[0], std::string("create"));
    REQUIRE_EQ(a.log[1], std::string("rescan"));
    REQUIRE_EQ(a.log[2], std::string("query"));
}

TEST_CASE(lifecycle_report_captures_provider_counters_after_stop) {
    FakeProv p;
    p.request_counters = {.info=1, .read=2, .write=3, .close=4, .fatal_protocol_errors=5};
    FakeAd a; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "provider-counters";

    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);

    REQUIRE_EQ(p.stop_c, 1);
    REQUIRE_EQ(r.provider.info_count, 1u);
    REQUIRE_EQ(r.provider.read_count, 2u);
    REQUIRE_EQ(r.provider.write_count, 3u);
    REQUIRE_EQ(r.provider.close_count, 4u);
    REQUIRE_EQ(r.provider.fatal_protocol_error_count, 5u);
}

TEST_CASE(lifecycle_baseline_comparison_ignores_enumeration_order) {
    FakeProv p;
    FakeAd a;
    a.devices_before = {{1}, {2}};
    a.devices_after = {{2}, {1}};
    FakeLoc l;
    l.numbers_before = {3, 4};
    l.numbers_after = {4, 3};
    FakeRT rt;
    myrd::poc::RunConfig cfg;
    cfg.run_id = "reordered-baseline";

    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);

    REQUIRE(r.success);
    REQUIRE(r.cleanup.aim_device_lists_match);
    REQUIRE(r.cleanup.physical_drive_lists_match);
}

TEST_CASE(lifecycle_retries_transient_post_cleanup_disk_snapshot_error) {
    FakeProv p;
    FakeAd a;
    FakeLoc l;
    l.numbers_before = {0};
    l.numbers_after = {0};
    l.transient_post_snapshot_failures = 1;
    FakeRT rt;
    myrd::poc::RunConfig cfg;
    cfg.run_id = "transient-post-cleanup-snapshot";

    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);

    REQUIRE(r.success);
    REQUIRE(r.cleanup.physical_drive_lists_match);
    REQUIRE(l.snapshot_calls >= 3);
}

TEST_CASE(lifecycle_retries_transient_named_object_release) {
    FakeProv p;
    p.transient_release_checks = 1;
    FakeAd a;
    FakeLoc l;
    FakeRT rt;
    myrd::poc::RunConfig cfg;
    cfg.run_id = "transient-named-object-release";

    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);

    REQUIRE(r.success);
    REQUIRE(r.cleanup.named_objects_released);
    REQUIRE(p.release_checks >= 2);
}

// 2. CREATE failure — no remove
TEST_CASE(lifecycle_create_failure_no_remove) {
    FakeProv p; FakeAd a; a.c_ok = false; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t2";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.success);
    REQUIRE(!r.cleanup.exact_remove_attempted);
}

// 3. CREATE succeeds, query fails — must remove exact id
TEST_CASE(lifecycle_query_failure_removes_id) {
    FakeProv p; FakeAd a; a.q_ok = false; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t3";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.success);
    REQUIRE(r.cleanup.exact_remove_attempted);
}

TEST_CASE(lifecycle_query_path_mismatch_fails_before_disk_location) {
    FakeProv p; FakeAd a; a.query_path = L"\\BaseNamedObjects\\Global\\wrong";
    FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "query-path-mismatch";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.success);
    REQUIRE(r.cleanup.exact_remove_succeeded);
    REQUIRE(r.primary_error.has_value());
}

TEST_CASE(lifecycle_query_flags_must_equal_normalized_m0_flags) {
    FakeProv p;
    FakeAd a;
    a.query_flags = myrd::aim::kDeviceTypeHd | myrd::aim::kTypeProxy |
        0x00000002u;
    FakeLoc l;
    FakeRT rt;
    myrd::poc::RunConfig cfg;
    cfg.run_id = "query-flags-mismatch";

    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);

    REQUIRE(!r.success);
    REQUIRE(r.cleanup.exact_remove_succeeded);
    REQUIRE(r.primary_error.has_value());
}

// 4. Locator failure — must remove exact id
TEST_CASE(lifecycle_locator_failure_removes_id) {
    FakeProv p; FakeAd a; FakeLoc l; l.ok = false; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t4";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.cleanup.exact_remove_attempted);
}

// 5. Round-trip failure — must remove exact id
TEST_CASE(lifecycle_roundtrip_failure_removes_id) {
    FakeProv p; FakeAd a; FakeLoc l; FakeRT rt; rt.ok = false;
    myrd::poc::RunConfig cfg; cfg.run_id = "t5";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.cleanup.exact_remove_attempted);
}

// 6. Remove failure is reported
TEST_CASE(lifecycle_remove_failure_reported) {
    FakeProv p; FakeAd a; a.r_ok = false; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t6";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.cleanup.exact_remove_succeeded);
}

TEST_CASE(lifecycle_device_disappearance_timeout_fails_cleanup_contract) {
    FakeProv p; FakeAd a; a.absent_ok = false; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "absent-timeout";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.success);
    REQUIRE(!r.cleanup.device_disappeared);
    REQUIRE(!r.cleanup_errors.empty());
}

TEST_CASE(lifecycle_provider_stop_failure_is_not_reported_as_success) {
    FakeProv p; p.stop_ok = false; FakeAd a; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "provider-stop";
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(!r.success);
    REQUIRE(!r.cleanup.provider_stopped);
    REQUIRE(!r.cleanup_errors.empty());
}

// 7. Debug injection after CREATE
TEST_CASE(lifecycle_debug_inject_create_returned) {
    FakeProv p; FakeAd a; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t7";
    cfg.debug_build = true;
    cfg.injected_failure = myrd::poc::FailurePoint::create_returned;
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.cleanup.exact_remove_attempted);
    REQUIRE(r.primary_error.has_value());
    REQUIRE(r.cleanup.aim_device_lists_match);
    REQUIRE(r.cleanup.physical_drive_lists_match);
}

// ── Raw round trip tests (Task 21) ────────────────────────────

TEST_CASE(m0_provider_start_services_info_request) {
    myrd::poc::M0Provider provider;
    auto started = provider.start();
    if (!started) {
        std::wcerr << L"M0Provider::start failed: code="
                   << started.error().code << L" operation="
                   << started.error().operation << L" message="
                   << started.error().message << std::endl;
    }
    REQUIRE(started.ok());

    const auto names = provider.names();
    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE, FALSE, names.mapping.c_str());
    HANDLE request = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                names.request.c_str());
    HANDLE response = OpenEventW(SYNCHRONIZE, FALSE,
                                 names.response.c_str());
    void* view = mapping == nullptr
        ? nullptr
        : MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                        0, 0, myrd::proxy::kMappingSize);

    DWORD wait_result = WAIT_FAILED;
    myrd::proxy::InfoResponse info{};
    if (view != nullptr && request != nullptr && response != nullptr) {
        const std::uint64_t request_code = myrd::proxy::kReqInfo;
        std::memcpy(view, &request_code, sizeof(request_code));
        if (SetEvent(request)) {
            wait_result = WaitForSingleObject(response, 1000);
            if (wait_result == WAIT_OBJECT_0) {
                std::memcpy(&info, view, sizeof(info));
            }
        }
    }

    const auto counters = provider.counters();
    auto stopped = provider.stop_and_release();
    if (view != nullptr) UnmapViewOfFile(view);
    if (response != nullptr) CloseHandle(response);
    if (request != nullptr) CloseHandle(request);
    if (mapping != nullptr) CloseHandle(mapping);

    REQUIRE(stopped.ok());
    REQUIRE_EQ(wait_result, WAIT_OBJECT_0);
    REQUIRE_EQ(info.file_size, myrd::aim::kM0DiskSize);
    REQUIRE_EQ(info.req_alignment, myrd::aim::kM0SectorSize);
    REQUIRE_EQ(info.flags,
        myrd::proxy::kFlagSupportsUnmap | myrd::proxy::kFlagSupportsZero);
    REQUIRE_EQ(counters.info, 1u);
}

TEST_CASE(raw_identity_check_opens_disk_with_read_access) {
    REQUIRE((myrd::poc::raw_identity_open_access() & GENERIC_READ) != 0);
}

TEST_CASE(raw_write_preflight_rejects_read_only_disk_attribute) {
    auto r = myrd::poc::validate_disk_write_preflight(
        0x2ull, true, ERROR_SUCCESS);
    REQUIRE(r.has_error());
    REQUIRE_EQ(r.error().code, static_cast<std::uint32_t>(ERROR_WRITE_PROTECT));
}

TEST_CASE(raw_write_preflight_rejects_disk_stack_write_protection) {
    auto r = myrd::poc::validate_disk_write_preflight(
        0, false, ERROR_WRITE_PROTECT);
    REQUIRE(r.has_error());
    REQUIRE_EQ(r.error().operation, std::wstring(L"IOCTL_DISK_IS_WRITABLE"));
}

TEST_CASE(raw_disk_read_only_attribute_is_cleared_non_persistently) {
    auto update = myrd::poc::writable_disk_attribute_update(0x2ull);
    REQUIRE(update.has_value());
    REQUIRE_EQ(update->attributes, 0ull);
    REQUIRE_EQ(update->attributes_mask, 0x2ull);
    REQUIRE(!update->persist);
}

TEST_CASE(raw_disk_without_read_only_attribute_needs_no_update) {
    auto update = myrd::poc::writable_disk_attribute_update(0);
    REQUIRE(!update.has_value());
}

#include "src/poc/raw_round_trip.h"

struct FakeRawIo : myrd::poc::IRawDiskIo {
    bool verify_ok=true, write_ok=true, flush_ok=true, read_ok=true;
    std::byte storage[4096]{};
    myrd::Result<void> verify_identity(const myrd::aim::LocatedDisk&) override { if(verify_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::win32,0,L"",L""}); }
    myrd::Result<void> seek(std::uint64_t) override { return myrd::Result<void>::success(); }
    myrd::Result<void> write_exact(std::span<const std::byte> d) override { if(write_ok){ std::memcpy(storage,d.data(),d.size()); return myrd::Result<void>::success(); } return myrd::Result<void>::failure({myrd::ErrorDomain::win32,0,L"",L""}); }
    myrd::Result<void> flush() override { if(flush_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::win32,0,L"flush",L"failed"}); }
    myrd::Result<void> read_exact(std::span<std::byte> d) override { if(read_ok){ std::memcpy(d.data(),storage,d.size()); return myrd::Result<void>::success(); } return myrd::Result<void>::failure({myrd::ErrorDomain::win32,0,L"",L""}); }
};

TEST_CASE(round_trip_success_hash_matches) {
    FakeRawIo io; myrd::poc::RawRoundTrip rt(io);
    auto r = rt.run({5, L"", 64ull*1024*1024});
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().written_sha256, r.value().read_sha256);
}

TEST_CASE(round_trip_flush_failure_is_not_success) {
    FakeRawIo io;
    io.flush_ok = false;
    myrd::poc::RawRoundTrip rt(io);

    auto r = rt.run(myrd::aim::LocatedDisk{});

    REQUIRE(r.has_error());
}

TEST_CASE(round_trip_identity_failure_reported) {
    FakeRawIo io; io.verify_ok = false;
    myrd::poc::RawRoundTrip rt(io);
    REQUIRE(rt.run({5, L"", 64ull*1024*1024}).has_error());
}

TEST_CASE(round_trip_no_raw_bytes_in_report) {
    FakeRawIo io; myrd::poc::RawRoundTrip rt(io);
    auto r = rt.run({5, L"", 64ull*1024*1024});
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().offset, 8ull*1024*1024);
    REQUIRE_EQ(r.value().length, 4096u);
}
