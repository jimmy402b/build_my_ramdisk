#include "test_framework.h"

#include "src/m3/m3_crash_test_control.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <thread>

using namespace myrd;
using namespace myrd::m3;

TEST_CASE(crash_control_parses_only_frozen_real_phase_names) {
    const auto journal = parse_m3_crash_test_phase("journal.flush.before");
    REQUIRE(journal.ok());
    REQUIRE_EQ(to_string(journal.value()), std::string_view{"journal.flush.before"});

    const auto manifest = parse_m3_crash_test_phase("manifest.flush.after");
    REQUIRE(manifest.ok());
    REQUIRE_EQ(to_string(manifest.value()), std::string_view{"manifest.flush.after"});

    const auto reconcile = parse_m3_crash_test_phase(
        "reconcile.before_report_success");
    REQUIRE(reconcile.ok());
    REQUIRE_EQ(to_string(reconcile.value()),
               std::string_view{"reconcile.before_report_success"});

    const auto unknown = parse_m3_crash_test_phase("journal.flush.middle");
    REQUIRE(unknown.has_error());
}

TEST_CASE(crash_control_maps_persistence_events_to_frozen_names) {
    const auto journal_before = map_m3_crash_test_phase(
        persistence::JournalWriterPhase::BeforeFlush);
    REQUIRE(journal_before.has_value());
    REQUIRE_EQ(*journal_before, M3CrashTestPhase::JournalFlushBefore);
    REQUIRE(!map_m3_crash_test_phase(
        persistence::JournalWriterPhase::BeforeBlockWrite).has_value());

    const auto manifest_after = map_m3_crash_test_phase(
        persistence::ManifestPublishTestPhase::AfterFlush);
    REQUIRE(manifest_after.has_value());
    REQUIRE_EQ(*manifest_after, M3CrashTestPhase::ManifestFlushAfter);

    const auto reconcile = map_m3_crash_test_phase(
        persistence::CheckpointReconcileTestPhase::BeforeReportSuccess);
    REQUIRE(reconcile.has_value());
    REQUIRE_EQ(*reconcile,
               M3CrashTestPhase::ReconcileBeforeReportSuccess);
}

TEST_CASE(crash_control_publishes_signal_and_releases_only_after_ack) {
    const auto root = std::filesystem::temp_directory_path() /
        (L"myrd-m3-crash-control-" +
         std::to_wstring(std::chrono::steady_clock::now()
             .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto signal = root / L"signal.txt";
    const auto release = root / L"release.txt";
    const auto phase = parse_m3_crash_test_phase("manifest.flush.after");
    REQUIRE(phase.ok());
    auto controller = M3CrashTestPauseController::create(
        phase.value(), signal, release, std::chrono::seconds(5));
    REQUIRE(controller.ok());

    auto future = std::async(std::launch::async, [&controller] {
        return controller.value().observe(
            M3CrashTestPhase::ManifestFlushAfter);
    });
    for (int i = 0; i != 100 && !std::filesystem::exists(signal); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(std::filesystem::exists(signal));
    std::ifstream input(signal);
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    input.close();
    REQUIRE(contents.find("manifest.flush.after") != std::string::npos);
    REQUIRE(future.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout);

    std::ofstream(release) << "release\n";
    REQUIRE(future.get().ok());
    std::filesystem::remove_all(root);
}
