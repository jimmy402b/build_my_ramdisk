#include "tests/support/test_framework.h"

#include "src/run/disk_runtime.h"

#include <string>
#include <set>
#include <vector>

namespace {
using namespace myrd;

Error fake_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 1, operation, L"fake multi-instance failure", 0};
}

struct SharedBundle final {
    std::filesystem::path path;
    bool locked{};
};

struct InstanceLedger final {
    std::set<wchar_t> assigned_letters;
    std::set<std::uint32_t> started_aim_disks;
};

struct FakeInstance final {
    SharedBundle* bundle{};
    wchar_t drive{};
    std::wstring volume_guid;
    std::uint32_t aim_disk{};
    std::uint64_t generation{};
    InstanceLedger* ledger{};
    bool recovered{};
    bool fail_verify{};
    bool fail_checkpoint{};
    std::vector<std::string> events;

    DiskRuntimeOperations operations() {
        DiskRuntimeOperations ops;
        ops.preflight = [this] {
            events.push_back("preflight");
            return Result<void>::success();
        };
        ops.classify_bundle = [this](const std::filesystem::path& path) {
            events.push_back("classify");
            REQUIRE_EQ(path, bundle->path);
            return Result<DiskBundleKind>::success(
                recovered ? DiskBundleKind::manifest : DiskBundleKind::empty);
        };
        ops.acquire_bundle_lock = [this](const std::filesystem::path& path) {
            events.push_back("lock");
            REQUIRE_EQ(path, bundle->path);
            if (bundle->locked)
                return Result<void>::failure(fake_error(L"bundle_lock"));
            bundle->locked = true;
            return Result<void>::success();
        };
        ops.release_bundle_lock = [this] {
            events.push_back("unlock");
            bundle->locked = false;
            return Result<void>::success();
        };
        ops.create_generation_zero_image = [this](std::uint64_t) {
            events.push_back("generation-zero");
            return Result<void>::success();
        };
        ops.recover_complete_image = [this](std::uint64_t) {
            events.push_back("recover");
            return Result<void>::success();
        };
        ops.start_aim = [this] {
            events.push_back("aim-start");
            REQUIRE(ledger->started_aim_disks.insert(aim_disk).second);
            return Result<void>::success();
        };
        ops.stop_aim = [this] {
            events.push_back("aim-stop");
            REQUIRE_EQ(ledger->started_aim_disks.erase(aim_disk), 1u);
            return Result<void>::success();
        };
        ops.provision_new_volume = [this](std::uint64_t, wchar_t letter) {
            events.push_back("provision");
            REQUIRE_EQ(letter, drive);
            REQUIRE(ledger->assigned_letters.insert(letter).second);
            return Result<ProductVolume>::success(
                ProductVolume{L"owner", volume_guid,
                              {{aim_disk, 0, 64ull * 1024 * 1024}},
                              drive, true, true});
        };
        ops.find_recovered_volume = [this](wchar_t letter) {
            events.push_back("find-recovered");
            REQUIRE_EQ(letter, drive);
            return Result<ProductVolume>::success(
                ProductVolume{L"owner", volume_guid,
                              {{aim_disk, 0, 64ull * 1024 * 1024}},
                              0, true, true});
        };
        ops.verify_volume = [this](const ProductVolume& volume, std::uint64_t) {
            events.push_back("verify");
            if (recovered) REQUIRE_EQ(volume.drive_letter, L'\0');
            else REQUIRE_EQ(volume.drive_letter, drive);
            REQUIRE_EQ(volume.volume_guid, volume_guid);
            REQUIRE_EQ(volume.extents.size(), 1u);
            REQUIRE_EQ(volume.extents.front().disk_number, aim_disk);
            return fail_verify
                ? Result<void>::failure(fake_error(L"verify"))
                : Result<void>::success();
        };
        ops.flush_volume = [this](const ProductVolume&) {
            events.push_back("flush");
            return Result<void>::success();
        };
        ops.checkpoint = [this] {
            events.push_back("checkpoint");
            if (fail_checkpoint)
                return Result<DiskCheckpointResult>::failure(
                    fake_error(L"checkpoint"));
            return Result<DiskCheckpointResult>::success(
                {DiskCheckpointDisposition::committed, generation});
        };
        ops.dismount_volume = [this](const ProductVolume&) {
            events.push_back("dismount");
            return Result<void>::success();
        };
        ops.remove_drive_letter = [this](const ProductVolume&) {
            events.push_back("remove-letter");
            REQUIRE_EQ(ledger->assigned_letters.erase(drive), 1u);
            return Result<void>::success();
        };
        ops.assign_drive_letter = [this](const ProductVolume&, wchar_t letter) {
            events.push_back("assign-letter");
            REQUIRE_EQ(letter, drive);
            if (!ledger->assigned_letters.insert(letter).second)
                return Result<void>::failure(fake_error(L"assign_drive_letter"));
            return Result<void>::success();
        };
        ops.maintenance_stats = [this] {
            return DiskMaintenanceStats{0, 0, generation};
        };
        return ops;
    }
};

DiskRuntimeOptions options(const SharedBundle& bundle, wchar_t drive) {
    return {bundle.path, 64ull * 1024 * 1024, drive, std::chrono::seconds(1)};
}

TEST_CASE(m4_multi_instance_keeps_bundle_drive_and_generation_independent) {
    SharedBundle bundle_a{L"bundle-a"};
    SharedBundle bundle_b{L"bundle-b"};
    InstanceLedger ledger;
    FakeInstance a{&bundle_a, L'R', L"volume-a", 101, 11, &ledger};
    FakeInstance b{&bundle_b, L'S', L"volume-b", 202, 23, &ledger};

    auto runtime_a = DiskRuntime::create(options(bundle_a, a.drive), a.operations());
    auto runtime_b = DiskRuntime::create(options(bundle_b, b.drive), b.operations());
    REQUIRE(runtime_a.ok() && runtime_b.ok());
    REQUIRE(runtime_a.value()->start().ok());
    REQUIRE(runtime_b.value()->start().ok());
    REQUIRE(bundle_a.locked && bundle_b.locked);
    REQUIRE_EQ(ledger.started_aim_disks.size(), 2u);
    REQUIRE_EQ(runtime_a.value()->maintenance_stats().generation, 11ull);
    REQUIRE_EQ(runtime_b.value()->maintenance_stats().generation, 23ull);
    REQUIRE(runtime_a.value()->shutdown_clean().ok());
    REQUIRE(bundle_b.locked);
    REQUIRE_EQ(ledger.started_aim_disks.size(), 1u);
    REQUIRE(runtime_b.value()->shutdown_clean().ok());
    REQUIRE(!bundle_a.locked && !bundle_b.locked);
    REQUIRE(ledger.assigned_letters.empty());
    REQUIRE(ledger.started_aim_disks.empty());
    REQUIRE_EQ(a.events[5], std::string("provision"));
    REQUIRE_EQ(b.events[5], std::string("provision"));
}

TEST_CASE(m4_multi_instance_rejects_second_runtime_for_same_bundle_lock) {
    SharedBundle shared{L"shared-bundle"};
    InstanceLedger ledger;
    FakeInstance first{&shared, L'R', L"volume-first", 101, 7, &ledger};
    FakeInstance second{&shared, L'S', L"volume-second", 202, 8, &ledger};
    auto runtime_first = DiskRuntime::create(options(shared, first.drive), first.operations());
    auto runtime_second = DiskRuntime::create(options(shared, second.drive), second.operations());
    REQUIRE(runtime_first.ok() && runtime_second.ok());
    REQUIRE(runtime_first.value()->start().ok());
    REQUIRE(!runtime_second.value()->start().ok());
    REQUIRE_EQ(second.events.size(), 3u);
    REQUIRE_EQ(second.events[0], std::string("preflight"));
    REQUIRE_EQ(second.events[1], std::string("classify"));
    REQUIRE_EQ(second.events[2], std::string("lock"));
    REQUIRE(shared.locked);
    REQUIRE(runtime_first.value()->shutdown_clean().ok());
    REQUIRE(!shared.locked);
}

TEST_CASE(m4_multi_instance_failure_in_a_does_not_affect_b) {
    SharedBundle bundle_a{L"bundle-failing-a"};
    SharedBundle bundle_b{L"bundle-surviving-b"};
    InstanceLedger ledger;
    FakeInstance a{&bundle_a, L'R', L"volume-a", 101, 31, &ledger};
    FakeInstance b{&bundle_b, L'S', L"volume-b", 202, 47, &ledger};
    a.fail_verify = true;

    auto runtime_a = DiskRuntime::create(options(bundle_a, a.drive), a.operations());
    auto runtime_b = DiskRuntime::create(options(bundle_b, b.drive), b.operations());
    REQUIRE(runtime_a.ok() && runtime_b.ok());
    REQUIRE(!runtime_a.value()->start().ok());
    REQUIRE(!bundle_a.locked);
    REQUIRE(runtime_b.value()->start().ok());
    REQUIRE(bundle_b.locked);
    REQUIRE_EQ(runtime_b.value()->maintenance_stats().generation, 47ull);
    REQUIRE(runtime_b.value()->shutdown_clean().ok());
    REQUIRE(!bundle_b.locked);
    REQUIRE_EQ(b.events[8], std::string("checkpoint"));
}

TEST_CASE(m4_multi_instance_recover_owns_its_mount_without_provisioning) {
    SharedBundle bundle{L"recover-bundle"};
    InstanceLedger ledger;
    FakeInstance recovered{&bundle, L'T', L"volume-recovered", 303, 59,
                           &ledger, true};
    auto runtime = DiskRuntime::recover(options(bundle, recovered.drive),
                                        recovered.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE_EQ(recovered.events[3], std::string("recover"));
    REQUIRE_EQ(recovered.events[5], std::string("find-recovered"));
    REQUIRE_EQ(recovered.events[7], std::string("assign-letter"));
    REQUIRE(std::find(recovered.events.begin(), recovered.events.end(),
                      "provision") == recovered.events.end());
    REQUIRE(ledger.assigned_letters.contains(L'T'));
    REQUIRE(runtime.value()->shutdown_clean().ok());
    REQUIRE(ledger.assigned_letters.empty());
    REQUIRE(ledger.started_aim_disks.empty());
}

TEST_CASE(m4_multi_instance_rejects_drive_letter_collision_without_harming_owner) {
    SharedBundle bundle_a{L"bundle-a"};
    SharedBundle bundle_b{L"bundle-b"};
    InstanceLedger ledger;
    FakeInstance first{&bundle_a, L'R', L"volume-a", 101, 7, &ledger, true};
    FakeInstance second{&bundle_b, L'R', L"volume-b", 202, 8, &ledger, true};
    auto runtime_first = DiskRuntime::recover(options(bundle_a, L'R'), first.operations());
    auto runtime_second = DiskRuntime::recover(options(bundle_b, L'R'), second.operations());
    REQUIRE(runtime_first.ok() && runtime_second.ok());
    REQUIRE(runtime_first.value()->start().ok());
    REQUIRE(!runtime_second.value()->start().ok());
    REQUIRE(ledger.assigned_letters.contains(L'R'));
    REQUIRE(bundle_a.locked);
    REQUIRE(!bundle_b.locked);
    REQUIRE_EQ(ledger.started_aim_disks.size(), 1u);
    REQUIRE(runtime_first.value()->shutdown_clean().ok());
    REQUIRE(ledger.assigned_letters.empty());
    REQUIRE(ledger.started_aim_disks.empty());
}

} // namespace
