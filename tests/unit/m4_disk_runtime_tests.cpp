#include "tests/support/test_framework.h"

#include "src/run/disk_runtime.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace myrd;

Error runtime_error(const wchar_t* operation = L"fake") {
    return {ErrorDomain::internal, 1, operation, L"fake failure", 0};
}

struct FakeRuntime final {
    std::vector<std::string> events;
    DiskBundleKind bundle_kind{DiskBundleKind::empty};
    bool fail_preflight{};
    bool fail_lock{};
    bool fail_start_aim{};
    bool fail_stop_aim{};
    bool fail_stop_aim_once{};
    bool fail_provision{};
    bool fail_find{};
    bool fail_verify{};
    bool fail_flush{};
    bool fail_checkpoint{};
    bool fail_assign{};
    bool fail_dismount{};
    bool fail_remove{};
    bool fail_remove_once{};
    std::uint64_t checkpoint_generation{1};
    DiskCheckpointDisposition checkpoint_disposition{
        DiskCheckpointDisposition::committed};
    std::uint64_t observed_create_size{};
    std::uint64_t observed_recover_size{};
    std::uint64_t observed_provision_size{};
    std::uint64_t observed_verify_size{};
    wchar_t observed_provision_letter{};
    wchar_t observed_find_letter{};
    wchar_t observed_assign_letter{};
    bool generation_zero_ready{};
    bool recovered_image_ready{};
    bool aim_running{};
    bool volume_observed{};
    bool verified{};
    bool flushed{};
    bool drive_assigned{};
    bool provision_called{};

    DiskRuntimeOperations operations() {
        DiskRuntimeOperations ops;
        ops.preflight = [this] {
            events.push_back("preflight");
            return fail_preflight ? Result<void>::failure(runtime_error())
                                  : Result<void>::success();
        };
        ops.classify_bundle = [this](const std::filesystem::path&) {
            events.push_back("classify");
            return Result<DiskBundleKind>::success(bundle_kind);
        };
        ops.acquire_bundle_lock = [this](const std::filesystem::path&) {
            events.push_back("lock");
            return fail_lock ? Result<void>::failure(runtime_error())
                             : Result<void>::success();
        };
        ops.release_bundle_lock = [this] {
            events.push_back("unlock");
            return Result<void>::success();
        };
        ops.create_generation_zero_image = [this](std::uint64_t bytes) {
            events.push_back("generation-zero");
            observed_create_size = bytes;
            generation_zero_ready = true;
            return Result<void>::success();
        };
        ops.recover_complete_image = [this](std::uint64_t bytes) {
            events.push_back("recover");
            observed_recover_size = bytes;
            recovered_image_ready = true;
            return Result<void>::success();
        };
        ops.start_aim = [this] {
            events.push_back("aim-start");
            if (fail_start_aim) return Result<void>::failure(runtime_error());
            aim_running = true;
            return Result<void>::success();
        };
        ops.stop_aim = [this] {
            events.push_back("aim-stop");
            if (fail_stop_aim_once) {
                fail_stop_aim_once = false;
                return Result<void>::failure(runtime_error());
            }
            return fail_stop_aim ? Result<void>::failure(runtime_error())
                                 : Result<void>::success();
        };
        ops.provision_new_volume = [this](std::uint64_t bytes, wchar_t letter) {
            events.push_back("provision");
            provision_called = true;
            if (fail_provision) return Result<ProductVolume>::failure(runtime_error());
            REQUIRE(generation_zero_ready);
            REQUIRE(aim_running);
            observed_provision_size = bytes;
            observed_provision_letter = letter;
            volume_observed = true;
            return Result<ProductVolume>::success(
                ProductVolume{L"owner", L"\\\\?\\Volume{a}", {}, letter, true, true});
        };
        ops.find_recovered_volume = [this](wchar_t letter) {
            events.push_back("find-volume");
            REQUIRE(recovered_image_ready);
            REQUIRE(aim_running);
            observed_find_letter = letter;
            volume_observed = true;
            if (fail_find) return Result<ProductVolume>::failure(runtime_error());
            return Result<ProductVolume>::success(
                ProductVolume{L"owner", L"\\\\?\\Volume{a}", {}, L'R', true, false});
        };
        ops.verify_volume = [this](const ProductVolume&, std::uint64_t bytes) {
            events.push_back("verify");
            REQUIRE(volume_observed);
            observed_verify_size = bytes;
            verified = true;
            return fail_verify ? Result<void>::failure(runtime_error())
                               : Result<void>::success();
        };
        ops.flush_volume = [this](const ProductVolume&) {
            events.push_back("flush");
            REQUIRE(volume_observed);
            REQUIRE(verified);
            flushed = true;
            return fail_flush ? Result<void>::failure(runtime_error())
                              : Result<void>::success();
        };
        ops.assign_drive_letter = [this](const ProductVolume&, wchar_t letter) {
            events.push_back("assign-letter");
            REQUIRE(volume_observed);
            REQUIRE(verified);
            observed_assign_letter = letter;
            drive_assigned = !fail_assign;
            return fail_assign ? Result<void>::failure(runtime_error())
                               : Result<void>::success();
        };
        ops.dismount_volume = [this](const ProductVolume&) {
            events.push_back("dismount");
            return fail_dismount ? Result<void>::failure(runtime_error())
                                 : Result<void>::success();
        };
        ops.remove_drive_letter = [this](const ProductVolume&) {
            events.push_back("remove-letter");
            if (fail_remove_once) {
                fail_remove_once = false;
                return Result<void>::failure(runtime_error());
            }
            return fail_remove ? Result<void>::failure(runtime_error())
                               : Result<void>::success();
        };
        ops.checkpoint = [this] {
            events.push_back("checkpoint");
            if (fail_checkpoint) return Result<DiskCheckpointResult>::failure(runtime_error());
            REQUIRE(flushed);
            return Result<DiskCheckpointResult>::success(
                {checkpoint_disposition, checkpoint_generation});
        };
        ops.maintenance_stats = [] { return DiskMaintenanceStats{1, 2, 3}; };
        return ops;
    }
};

DiskRuntimeOptions options() {
    return {L"bundle-a", 64ull * 1024 * 1024, L'R', std::chrono::seconds(1)};
}

void require_events(const FakeRuntime& fake,
                    std::initializer_list<const char*> expected) {
    REQUIRE_EQ(fake.events.size(), expected.size());
    std::size_t i = 0;
    for (const auto* event : expected) REQUIRE_EQ(fake.events[i++], event);
}
} // namespace

TEST_CASE(m4_disk_runtime_create_orders_generation_zero_provision_verify_flush_checkpoint) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE_EQ(fake.observed_create_size, options().virtual_bytes);
    REQUIRE_EQ(fake.observed_provision_size, options().virtual_bytes);
    REQUIRE_EQ(fake.observed_provision_letter, options().drive_letter);
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "checkpoint"});
}

TEST_CASE(m4_disk_runtime_create_shutdown_removes_created_drive_letter) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(runtime.value()->shutdown_clean().ok());
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "checkpoint", "flush", "remove-letter", "dismount",
                           "aim-stop", "unlock"});
}

TEST_CASE(m4_disk_runtime_shutdown_retry_is_idempotent_after_cleanup_error) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    fake.fail_flush = true;
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    fake.fail_flush = false;
    REQUIRE(runtime.value()->shutdown_clean().ok());
}

TEST_CASE(m4_disk_runtime_post_provision_failure_cleans_created_drive_letter) {
    FakeRuntime fake;
    fake.fail_verify = true;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(!runtime.value()->start().ok());
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "remove-letter",
                           "dismount", "aim-stop", "unlock"});
}

TEST_CASE(m4_disk_runtime_recover_never_provisions_and_assigns_existing_volume) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::manifest;
    auto runtime = DiskRuntime::recover(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE_EQ(fake.observed_recover_size, options().virtual_bytes);
    REQUIRE_EQ(fake.observed_verify_size, options().virtual_bytes);
    REQUIRE_EQ(fake.observed_find_letter, options().drive_letter);
    REQUIRE_EQ(fake.observed_assign_letter, options().drive_letter);
    REQUIRE(!fake.provision_called);
    require_events(fake, {"preflight", "classify", "lock", "recover",
                           "aim-start", "find-volume", "verify", "assign-letter"});
}

TEST_CASE(m4_disk_runtime_recover_after_checkpoint_only_reopens_existing_volume) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::manifest;
    fake.checkpoint_generation = 7;
    auto runtime = DiskRuntime::recover(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    // Recovery must expose the latest committed image to AIM and bind the
    // already-existing volume.  It must not flush, provision, or publish a
    // new checkpoint as part of startup.
    require_events(fake, {"preflight", "classify", "lock", "recover",
                           "aim-start", "find-volume", "verify",
                           "assign-letter"});
    REQUIRE_EQ(fake.observed_recover_size, options().virtual_bytes);
    REQUIRE_EQ(fake.observed_verify_size, options().virtual_bytes);
    REQUIRE(!fake.provision_called);
    REQUIRE(!fake.flushed);
}

TEST_CASE(m4_disk_runtime_unknown_nonempty_does_not_start_aim) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::unknown_nonempty;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(!runtime.value()->start().ok());
    REQUIRE_EQ(fake.events.size(), 4u);
    REQUIRE_EQ(fake.events[0], std::string("preflight"));
    REQUIRE_EQ(fake.events[1], std::string("classify"));
    REQUIRE_EQ(fake.events[2], std::string("lock"));
    REQUIRE_EQ(fake.events[3], std::string("unlock"));
}

TEST_CASE(m4_disk_runtime_recovery_size_mismatch_does_not_mount) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::manifest;
    fake.fail_verify = true;
    auto runtime = DiskRuntime::recover(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(!runtime.value()->start().ok());
    for (const auto& event : fake.events) REQUIRE(event != "assign-letter");
}

TEST_CASE(m4_disk_runtime_failure_cleans_only_owned_resources) {
    FakeRuntime fake;
    fake.fail_flush = true;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(!runtime.value()->start().ok());
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "remove-letter", "dismount", "aim-stop", "unlock"});
}

TEST_CASE(m4_disk_runtime_successful_shutdown_reverses_owned_resources_exactly) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::manifest;
    auto runtime = DiskRuntime::recover(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(runtime.value()->shutdown_clean().ok());
    require_events(fake, {"preflight", "classify", "lock", "recover",
                           "aim-start", "find-volume", "verify", "assign-letter",
                           "flush", "remove-letter", "dismount", "aim-stop",
                           "unlock"});
}

TEST_CASE(m4_disk_runtime_shutdown_is_idempotent_and_continues_after_cleanup_failure) {
    FakeRuntime fake;
    fake.bundle_kind = DiskBundleKind::manifest;
    fake.fail_remove = true;
    auto runtime = DiskRuntime::recover(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    require_events(fake, {"preflight", "classify", "lock", "recover",
                           "aim-start", "find-volume", "verify", "assign-letter",
                           "flush", "remove-letter"});
    const auto event_count = fake.events.size();
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    REQUIRE_EQ(fake.events.size(), event_count + 2);
    REQUIRE_EQ(fake.events.back(), std::string("remove-letter"));
}

TEST_CASE(m4_disk_runtime_retries_transient_drive_cleanup_failure) {
    FakeRuntime fake;
    fake.fail_remove_once = true;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    REQUIRE(runtime.value()->shutdown_clean().ok());
    const auto remove_count = std::count(fake.events.begin(), fake.events.end(),
                                         std::string("remove-letter"));
    REQUIRE_EQ(remove_count, 2u);
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "checkpoint", "flush", "remove-letter",
                           "flush", "remove-letter", "dismount", "aim-stop",
                           "unlock"});
}

TEST_CASE(m4_disk_runtime_retries_dismount_failure_without_releasing_other_ownership) {
    FakeRuntime fake;
    fake.fail_dismount = true;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(!runtime.value()->shutdown_clean().ok());

    fake.fail_dismount = false;
    REQUIRE(runtime.value()->shutdown_clean().ok());
    const auto dismount_count = std::count(fake.events.begin(), fake.events.end(),
                                           std::string("dismount"));
    REQUIRE_EQ(dismount_count, 2u);
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "checkpoint", "flush", "remove-letter", "dismount",
                           "flush", "dismount", "aim-stop", "unlock"});
}

TEST_CASE(m4_disk_runtime_retries_stop_aim_before_releasing_bundle_lock) {
    FakeRuntime fake;
    fake.fail_stop_aim_once = true;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    REQUIRE_EQ(fake.events.back(), std::string("aim-stop"));

    REQUIRE(runtime.value()->shutdown_clean().ok());
    require_events(fake, {"preflight", "classify", "lock", "generation-zero",
                           "aim-start", "provision", "verify", "flush",
                           "checkpoint", "flush", "remove-letter", "dismount",
                           "aim-stop", "aim-stop", "unlock"});
}

TEST_CASE(m4_disk_runtime_failed_stages_clean_only_resources_already_owned) {
    struct FailureCase {
        enum class Stage { aim, provision, verify, assign } stage;
        std::vector<std::string> expected;
    };
    const FailureCase cases[] = {
        {FailureCase::Stage::aim,
         {"preflight", "classify", "lock", "generation-zero", "aim-start", "unlock"}},
        {FailureCase::Stage::provision,
         {"preflight", "classify", "lock", "generation-zero", "aim-start", "provision",
          "aim-stop", "unlock"}},
        {FailureCase::Stage::verify,
         {"preflight", "classify", "lock", "generation-zero", "aim-start", "provision",
          "verify", "remove-letter", "dismount", "aim-stop", "unlock"}},
        {FailureCase::Stage::assign,
         {"preflight", "classify", "lock", "recover", "aim-start", "find-volume",
          "verify", "assign-letter", "dismount", "aim-stop", "unlock"}},
    };
    for (const auto& c : cases) {
        FakeRuntime fake;
        fake.bundle_kind = c.stage == FailureCase::Stage::assign
                               ? DiskBundleKind::manifest
                               : DiskBundleKind::empty;
        switch (c.stage) {
        case FailureCase::Stage::aim: fake.fail_start_aim = true; break;
        case FailureCase::Stage::provision: fake.fail_provision = true; break;
        case FailureCase::Stage::verify: fake.fail_verify = true; break;
        case FailureCase::Stage::assign: fake.fail_assign = true; break;
        }
        auto runtime = fake.bundle_kind == DiskBundleKind::manifest
                           ? DiskRuntime::recover(options(), fake.operations())
                           : DiskRuntime::create(options(), fake.operations());
        REQUIRE(runtime.ok());
        REQUIRE(!runtime.value()->start().ok());
        REQUIRE_EQ(fake.events.size(), c.expected.size());
        for (std::size_t i = 0; i < c.expected.size(); ++i) {
            REQUIRE_EQ(fake.events[i], c.expected[i]);
        }
    }
}

TEST_CASE(m4_disk_runtime_second_instance_same_bundle_is_rejected_by_lock) {
    FakeRuntime first_fake;
    auto first = DiskRuntime::create(options(), first_fake.operations());
    REQUIRE(first.ok());
    REQUIRE(first.value()->start().ok());

    FakeRuntime second_fake;
    second_fake.fail_lock = true;
    auto second = DiskRuntime::create(options(), second_fake.operations());
    REQUIRE(second.ok());
    REQUIRE(!second.value()->start().ok());
    REQUIRE_EQ(second_fake.events.size(), 3u);
    REQUIRE_EQ(second_fake.events.back(), std::string("lock"));
}

TEST_CASE(m4_disk_runtime_exposes_maintenance_stats_and_checkpoint_contract) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    REQUIRE_EQ(runtime.value()->maintenance_stats().journal_bytes, 2ull);
    const auto checkpoint = runtime.value()->checkpoint();
    REQUIRE(checkpoint.ok());
    REQUIRE_EQ(checkpoint.value().generation, 1ull);
}

TEST_CASE(m4_disk_runtime_propagates_noncommitted_checkpoint_disposition) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()->start().ok());
    fake.checkpoint_disposition = DiskCheckpointDisposition::not_committed;
    const auto checkpoint = runtime.value()->checkpoint();
    REQUIRE(checkpoint.ok());
    REQUIRE_EQ(checkpoint.value().disposition,
               DiskCheckpointDisposition::not_committed);
}

TEST_CASE(m4_disk_runtime_rejects_checkpoint_before_start) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    const auto checkpoint = runtime.value()->checkpoint();
    REQUIRE(!checkpoint.ok());
    REQUIRE(fake.events.empty());
}

TEST_CASE(m4_disk_runtime_rejects_shutdown_before_start) {
    FakeRuntime fake;
    auto runtime = DiskRuntime::create(options(), fake.operations());
    REQUIRE(runtime.ok());
    REQUIRE(!runtime.value()->shutdown_clean().ok());
    REQUIRE(fake.events.empty());
}
