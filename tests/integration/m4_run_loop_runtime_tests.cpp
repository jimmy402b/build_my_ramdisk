#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "tests/support/test_framework.h"

#include "src/run/disk_runtime.h"
#include "src/run/run_loop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace {
using namespace myrd;

Error fake_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 1, operation, L"fake failure", 0};
}

struct FakeRuntime final {
    std::vector<std::string> events;
    bool volume_ready{};
    bool verified{};
    bool flushed{};
    bool fail_remove_once{};

    DiskRuntimeOperations operations() {
        DiskRuntimeOperations ops{};
        ops.preflight = [this] {
            events.push_back("preflight");
            return Result<void>::success();
        };
        ops.classify_bundle = [this](const std::filesystem::path&) {
            events.push_back("classify");
            return Result<DiskBundleKind>::success(DiskBundleKind::empty);
        };
        ops.acquire_bundle_lock = [this](const std::filesystem::path&) {
            events.push_back("lock");
            return Result<void>::success();
        };
        ops.release_bundle_lock = [this] {
            events.push_back("unlock");
            return Result<void>::success();
        };
        ops.create_generation_zero_image = [this](std::uint64_t) {
            events.push_back("generation-zero");
            return Result<void>::success();
        };
        ops.start_aim = [this] {
            events.push_back("aim-start");
            return Result<void>::success();
        };
        ops.stop_aim = [this] {
            events.push_back("aim-stop");
            return Result<void>::success();
        };
        ops.provision_new_volume = [this](std::uint64_t, wchar_t letter) {
            events.push_back("provision");
            volume_ready = true;
            return Result<ProductVolume>::success(
                ProductVolume{L"owner", L"volume", {}, letter, true, true});
        };
        ops.verify_volume = [this](const ProductVolume&, std::uint64_t) {
            events.push_back("verify");
            if (!volume_ready) return Result<void>::failure(fake_error(L"verify"));
            verified = true;
            return Result<void>::success();
        };
        ops.flush_volume = [this](const ProductVolume&,
                                  std::optional<LockedVolume>&) {
            events.push_back("flush");
            if (!verified) return Result<void>::failure(fake_error(L"flush"));
            flushed = true;
            return Result<void>::success();
        };
        ops.checkpoint = [this] {
            events.push_back("checkpoint");
            if (!flushed) {
                return Result<DiskCheckpointResult>::failure(
                    fake_error(L"checkpoint"));
            }
            return Result<DiskCheckpointResult>::success(
                {DiskCheckpointDisposition::committed, 2});
        };
        ops.remove_drive_letter = [this](const ProductVolume&) {
            events.push_back("remove-letter");
            if (fail_remove_once) {
                fail_remove_once = false;
                return Result<void>::failure(fake_error(L"remove-drive-letter"));
            }
            return Result<void>::success();
        };
        ops.dismount_volume = [this](const ProductVolume&,
                                     std::optional<LockedVolume>&) {
            events.push_back("dismount");
            return Result<void>::success();
        };
        ops.maintenance_stats = [] { return DiskMaintenanceStats{}; };
        return ops;
    }
};

TEST_CASE(m4_concrete_run_loop_commits_final_checkpoint_before_cleanup) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(
        DiskRuntimeOptions{L"bundle", 64ull * 1024 * 1024, L'R',
                           std::chrono::seconds(1)},
        fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());

    std::atomic_bool requested{true};
    RunOptions options{};
    REQUIRE_EQ(myrd::run_loop(*runtime.value(), options, requested), 0);

    const auto initial_checkpoint =
        std::find(fake.events.begin(), fake.events.end(), "checkpoint");
    REQUIRE(initial_checkpoint != fake.events.end());
    const auto final_checkpoint =
        std::find(initial_checkpoint + 1, fake.events.end(), "checkpoint");
    REQUIRE(final_checkpoint != fake.events.end());
    const auto cleanup =
        std::find(final_checkpoint + 1, fake.events.end(), "remove-letter");
    REQUIRE(cleanup != fake.events.end());
    REQUIRE(std::find(initial_checkpoint, final_checkpoint, "flush") !=
            final_checkpoint);
    REQUIRE(std::distance(final_checkpoint, cleanup) >= 2);
}

TEST_CASE(m4_concrete_run_loop_retries_transient_cleanup_failure) {
    FakeRuntime fake;
    fake.fail_remove_once = true;
    auto runtime = DiskRuntime::create(
        DiskRuntimeOptions{L"bundle", 64ull * 1024 * 1024, L'R',
                           std::chrono::seconds(1)},
        fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());

    std::atomic_bool requested{true};
    RunOptions options{};
    REQUIRE_EQ(myrd::run_loop(*runtime.value(), options, requested), 0);
    REQUIRE_EQ(std::count(fake.events.begin(), fake.events.end(),
                          "remove-letter"), 2);
}

} // namespace
