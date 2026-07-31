#include "test_framework.h"
#include "src/poc/lifecycle_runner.h"
#include <string>
#include <vector>

static const myrd::aim::DeviceId kFakeId{42};

struct FakeProv : myrd::poc::IProviderLifecycle {
    int start_c = 0, stop_c = 0; bool rel = true;
    myrd::Result<void> start() override { start_c++; return myrd::Result<void>::success(); }
    myrd::Result<void> stop_and_release() override { stop_c++; return myrd::Result<void>::success(); }
    std::wstring native_object_path() const override { return L"\\BaseNamedObjects\\Global\\test"; }
    myrd::proxy::RequestCounters counters() const override { return {}; }
    myrd::Result<bool> named_objects_released() const override { return myrd::Result<bool>::success(rel); }
};

struct FakeAd : myrd::poc::IKernelDiskAdapter {
    std::vector<std::string> log;
    bool c_ok = true, q_ok = true, r_ok = true;
    myrd::Result<std::vector<myrd::aim::DeviceId>> list_devices() override { return myrd::Result<std::vector<myrd::aim::DeviceId>>::success({}); }
    myrd::Result<myrd::aim::DeviceId> create_proxy_disk(const myrd::aim::ProxyDiskSpec&) override { log.push_back("create"); if (c_ok) return myrd::Result<myrd::aim::DeviceId>::success(kFakeId); return myrd::Result<myrd::aim::DeviceId>::failure({myrd::ErrorDomain::protocol,0,L"",L""}); }
    myrd::Result<myrd::aim::DiskInfo> query_device(myrd::aim::DeviceId) override { log.push_back("query"); myrd::aim::DiskInfo di{kFakeId, 64ull*1024*1024, 512, myrd::aim::kM0CreateFlags, L"\\BaseNamedObjects\\Global\\test"}; return myrd::Result<myrd::aim::DiskInfo>::success(di); }
    myrd::Result<void> remove_device(myrd::aim::DeviceId id) override { log.push_back("remove:"+std::to_string(id.value)); if (r_ok) return myrd::Result<void>::success(); return myrd::Result<void>::failure({myrd::ErrorDomain::protocol,0,L"",L""}); }
    myrd::Result<void> wait_until_absent(myrd::aim::DeviceId, std::chrono::milliseconds) override { log.push_back("absent"); return myrd::Result<void>::success(); }
};

struct FakeLoc : myrd::poc::IDiskLocator {
    bool ok = true;
    myrd::Result<std::vector<std::uint32_t>> snapshot_numbers() override { return myrd::Result<std::vector<std::uint32_t>>::success({}); }
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
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.success);
    REQUIRE(r.cleanup.exact_remove_succeeded);
    REQUIRE(a.log.size() >= 4u); // create, query, remove:42, absent
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
    REQUIRE(r.cleanup.exact_remove_attempted);
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

// 7. Debug injection after CREATE
TEST_CASE(lifecycle_debug_inject_create_returned) {
    FakeProv p; FakeAd a; FakeLoc l; FakeRT rt;
    myrd::poc::RunConfig cfg; cfg.run_id = "t7";
    cfg.debug_build = true;
    cfg.injected_failure = myrd::poc::FailurePoint::create_returned;
    auto r = myrd::poc::run_lifecycle(p, a, l, rt, cfg);
    REQUIRE(r.cleanup.exact_remove_attempted);
    REQUIRE(r.primary_error.has_value());
}
