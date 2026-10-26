#include "tests/support/test_framework.h"

#include "src/run/runtime_adapter.h"
#include "src/storage/block_constants.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace myrd;

constexpr std::uint64_t kBytes = 64ull * 1024 * 1024;

void require_events(const std::vector<std::string>& actual,
                    std::initializer_list<const char*> expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    std::size_t index = 0;
    for (const auto* event : expected) REQUIRE_EQ(actual[index++], event);
}

// Every accepted M4/M3 piece sits behind these two boundaries, so a fake that
// implements both is a complete production stand-in.  The single shared event
// log is the point: the adapter's contribution is the *sequence* it issues at
// the two boundaries, not any decision of its own.
struct FakeBackend final : WindowsVolumeOperations, M3RuntimeOperations {
    explicit FakeBackend(std::vector<std::string>& log) : events(&log) {}

    std::vector<std::string>* events;
    DiskCheckpointDisposition disposition{DiskCheckpointDisposition::committed};
    std::uint64_t checkpoint_generation{7};
    DiskMaintenanceStats stats{1, 2, 3};
    std::uint64_t observed_create_bytes{};
    std::uint64_t observed_recover_bytes{};
    std::uint64_t observed_verify_bytes{};
    wchar_t observed_letter{};

    // ---- WindowsVolumeOperations ----
    Result<ProductVolume> provision(const std::uint64_t,
                                    const wchar_t letter) override {
        events->push_back("provision");
        observed_letter = letter;
        return Result<ProductVolume>::success(
            ProductVolume{L"owner", L"\\\\?\\Volume{x}", {}, letter, true, true});
    }
    Result<ProductVolume> find(const wchar_t letter) override {
        events->push_back("find");
        observed_letter = letter;
        return Result<ProductVolume>::success(
            ProductVolume{L"owner", L"\\\\?\\Volume{x}", {}, letter, true, false});
    }
    Result<void> verify(const ProductVolume&,
                        const std::uint64_t bytes) override {
        events->push_back("verify");
        observed_verify_bytes = bytes;
        return Result<void>::success();
    }
    Result<void> flush(const ProductVolume&,
                       std::optional<LockedVolume>& held) override {
        events->push_back(held.has_value() ? "flush-frozen" : "flush");
        return Result<void>::success();
    }
    Result<void> assign(const ProductVolume&, const wchar_t) override {
        events->push_back("assign");
        return Result<void>::success();
    }
    Result<void> remove_letter(const ProductVolume&) override {
        events->push_back("remove-letter");
        return Result<void>::success();
    }
    Result<VolumeLockAttempt> lock(const ProductVolume&,
                                   const std::uint32_t) override {
        events->push_back("lock");
        return Result<VolumeLockAttempt>::success({false, true});
    }
    // Records whether it dismantled through a lock the caller already held.  The
    // two are different facts -- "the runtime froze this volume and dismantled
    // it through that freeze" versus "the runtime took its own lock here" -- and
    // the log has to be able to tell them apart, which is what the M5 stop path
    // turns on.
    Result<void> dismount(const ProductVolume&,
                          std::optional<LockedVolume>& held) override {
        events->push_back(held.has_value() ? "dismount-frozen" : "dismount");
        return Result<void>::success();
    }

    // ---- M3RuntimeOperations ----
    Result<void> create_generation_zero(const std::uint64_t bytes) override {
        events->push_back("generation-zero");
        observed_create_bytes = bytes;
        return Result<void>::success();
    }
    Result<void> recover(const std::uint64_t bytes) override {
        events->push_back("recover");
        observed_recover_bytes = bytes;
        return Result<void>::success();
    }
    Result<void> start() override {
        events->push_back("aim-start");
        return Result<void>::success();
    }
    Result<void> stop() override {
        events->push_back("aim-stop");
        return Result<void>::success();
    }
    Result<DiskCheckpointResult> checkpoint() override {
        events->push_back("checkpoint");
        return Result<DiskCheckpointResult>::success(
            {disposition, checkpoint_generation});
    }
    DiskMaintenanceStats maintenance_stats() const override { return stats; }
    Result<DiskCheckpointPrepared> prepare_checkpoint(
        const DiskCheckpointPrepareRequest&) override {
        events->push_back("prepare");
        Sha256Digest candidate{};
        candidate.fill(std::byte{0x5A});
        return Result<DiskCheckpointPrepared>::success({5, 6, 2, candidate});
    }
    Result<DiskCheckpointResult> commit_checkpoint(
        const DiskCheckpointCommitRequest& request) override {
        events->push_back("commit");
        return Result<DiskCheckpointResult>::success(
            {disposition, request.native_operation_id});
    }
    Result<void> abort_checkpoint(std::uint64_t) override {
        events->push_back("abort");
        return Result<void>::success();
    }
};

// One composition under test: the fake backend, a scripted environment, and
// the log they share.
struct Harness final {
    std::vector<std::string> events;
    FakeBackend backend{events};
    DiskBundleKind bundle_kind{DiskBundleKind::empty};

    RuntimeAdapterEnvironment environment() {
        RuntimeAdapterEnvironment value{};
        value.preflight = [this] {
            events.push_back("preflight");
            return Result<void>::success();
        };
        value.classify_bundle = [this](const std::filesystem::path&) {
            events.push_back("classify");
            return Result<DiskBundleKind>::success(bundle_kind);
        };
        value.acquire_bundle_lock = [this](const std::filesystem::path&) {
            events.push_back("lock");
            return Result<void>::success();
        };
        value.release_bundle_lock = [this] {
            events.push_back("unlock");
            return Result<void>::success();
        };
        return value;
    }

    Result<std::unique_ptr<DiskRuntime>> start(const bool create) {
        RuntimeAdapterDependencies dependencies{L"bundle-a", kBytes, L'R',
                                                backend,      backend, L"myrd:1",
                                                environment()};
        auto operations = make_runtime_operations(dependencies);
        if (!operations.ok()) {
            return Result<std::unique_ptr<DiskRuntime>>::failure(
                operations.error());
        }
        const DiskRuntimeOptions options{L"bundle-a", kBytes, L'R',
                                         std::chrono::seconds(1)};
        auto runtime = create
            ? DiskRuntime::create(options, operations.move_value())
            : DiskRuntime::recover(options, operations.move_value());
        if (!runtime.ok()) return runtime;
        const auto started = runtime.value()->start();
        if (!started.ok()) {
            return Result<std::unique_ptr<DiskRuntime>>::failure(
                started.error());
        }
        return runtime;
    }
};

} // namespace

TEST_CASE(m4_runtime_adapter_create_issue_the_shared_production_sequence) {
    Harness harness;
    const auto runtime = harness.start(true);
    REQUIRE(runtime.ok());
    require_events(harness.events,
                   {"preflight", "classify", "lock", "generation-zero",
                    "aim-start", "provision", "verify", "flush", "checkpoint"});
    REQUIRE_EQ(harness.backend.observed_create_bytes, kBytes);
    REQUIRE_EQ(harness.backend.observed_verify_bytes, kBytes);
    REQUIRE_EQ(harness.backend.observed_letter, L'R');
}

TEST_CASE(m4_runtime_adapter_recover_never_provisions_and_assigns_the_volume) {
    Harness harness;
    harness.bundle_kind = DiskBundleKind::manifest;
    const auto runtime = harness.start(false);
    REQUIRE(runtime.ok());
    require_events(harness.events,
                   {"preflight", "classify", "lock", "recover", "aim-start",
                    "find", "verify", "assign"});
    REQUIRE_EQ(harness.backend.observed_recover_bytes, kBytes);
    REQUIRE(std::find(harness.events.begin(), harness.events.end(),
                      "provision") == harness.events.end());
}

// The M5-04 exit criterion: the foreground CLI and the per-profile worker run
// the same adapter, so for identical lifecycle inputs both must issue the
// identical call sequence — including the M5-only two-phase boundary and the
// cleanup order on shutdown.
TEST_CASE(m4_runtime_adapter_foreground_and_worker_issue_identical_calls) {
    const auto run_lifecycle = [] {
        Harness harness;
        auto runtime = harness.start(true);
        REQUIRE(runtime.ok());

        const auto prepared = runtime.value()->prepare_checkpoint({3908});
        REQUIRE(prepared.ok());
        REQUIRE(runtime.value()->commit_checkpoint(
                    {3908, prepared.value().manifest_slot,
                     prepared.value().candidate_bytes_digest}).ok());
        REQUIRE(runtime.value()->shutdown_clean().ok());
        return harness.events;
    };

    const auto foreground = run_lifecycle();
    const auto worker = run_lifecycle();

    REQUIRE(foreground == worker);
    require_events(foreground,
                   {"preflight",  "classify", "lock",        "generation-zero",
                    "aim-start",  "provision", "verify",     "flush",
                    "checkpoint", "prepare",  "commit",      "flush",
                    "remove-letter", "dismount", "aim-stop", "unlock"});
}

TEST_CASE(m4_runtime_adapter_exposes_the_two_phase_boundary_to_the_worker) {
    Harness harness;
    auto runtime = harness.start(true);
    REQUIRE(runtime.ok());

    const auto prepared = runtime.value()->prepare_checkpoint({3908});
    REQUIRE(prepared.ok());
    REQUIRE(runtime.value()->checkpoint_prepared());
    // The adapter must translate the boundary faithfully: the slot and digest
    // the caller sees are the ones commit has to repeat.
    REQUIRE_EQ(prepared.value().manifest_slot, 2);
    REQUIRE(runtime.value()->commit_checkpoint(
                {3908, prepared.value().manifest_slot,
                 prepared.value().candidate_bytes_digest}).ok());
    REQUIRE(!runtime.value()->checkpoint_prepared());

    const auto prepared_it =
        std::find(harness.events.begin(), harness.events.end(), "prepare");
    const auto commit_it =
        std::find(harness.events.begin(), harness.events.end(), "commit");
    REQUIRE(prepared_it != harness.events.end());
    REQUIRE(commit_it != harness.events.end());
    REQUIRE(prepared_it < commit_it);
}

// A partially wired adapter would silently skip a safety gate, so it is
// refused outright rather than defaulted.
TEST_CASE(m4_runtime_adapter_refuses_an_incomplete_environment) {
    Harness harness;
    RuntimeAdapterDependencies dependencies{L"bundle-a", kBytes, L'R',
                                            harness.backend, harness.backend,
                                            L"myrd:1", {}};
    const auto operations = make_runtime_operations(dependencies);
    REQUIRE(!operations.ok());
    REQUIRE(harness.events.empty());
}

// The dirty-byte reading is what the M4 run loop compares against
// `--checkpoint-dirty-mb`, so its unit of measure is a behaviour, not a detail.
// It used to be 2 MiB per block against a 64 KiB block: a 64 MiB threshold
// therefore fired after ~2 MiB of real dirty data, and every existing test
// still passed because they all handed the policy a number instead of asking
// this function for one.
TEST_CASE(m4_runtime_adapter_reports_dirty_bytes_in_store_block_units) {
    REQUIRE_EQ(kBlockSizeBytes, 64ull * 1024);
    REQUIRE_EQ(dirty_bytes_from_block_counts(0, 0), 0ull);
    REQUIRE_EQ(dirty_bytes_from_block_counts(1, 0), kBlockSizeBytes);
    REQUIRE_EQ(dirty_bytes_from_block_counts(0, 1), kBlockSizeBytes);
    REQUIRE_EQ(dirty_bytes_from_block_counts(3, 2), 5ull * kBlockSizeBytes);
    // The shape of the old defect, stated as a number: five blocks must not
    // read as five units of 2 MiB.
    REQUIRE(dirty_bytes_from_block_counts(3, 2) != 5ull * 2ull * 1024ull * 1024ull);
}
