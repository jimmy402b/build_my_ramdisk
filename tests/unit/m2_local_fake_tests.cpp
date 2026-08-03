#include "test_framework.h"

#include "src/m2/m2_local_fake.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

class TempEvidenceDirectory {
public:
    TempEvidenceDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root_ = std::filesystem::temp_directory_path() /
            ("myrd-m2-local-fake-" + std::to_string(stamp) + "-" +
             std::to_string(sequence.fetch_add(1)));
        std::error_code error;
        std::filesystem::create_directory(root_, error);
        REQUIRE(!error);
    }

    ~TempEvidenceDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE(m2_local_fake_runs_the_complete_lifecycle_and_publishes_go) {
    TempEvidenceDirectory evidence;

    auto result = myrd::run_m2_local_fake(
        evidence.path(), "local-fake-0123456789abcdef");

    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().report.verdict, myrd::M2Verdict::go);
    REQUIRE_EQ(result.value().report.run.mode, myrd::M2RunMode::fake);
    REQUIRE_EQ(
        result.value().report.run.run_id,
        "local-fake-0123456789abcdef");
    REQUIRE_EQ(result.value().report.phases.size(), 16u);
    REQUIRE(std::all_of(
        result.value().report.phases.begin(),
        result.value().report.phases.end(),
        [](const auto& phase) {
            return phase.status == myrd::M2PhaseStatus::passed;
        }));
    REQUIRE_EQ(result.value().report.cleanup_actions.size(), 4u);
    REQUIRE(std::all_of(
        result.value().report.cleanup_actions.begin(),
        result.value().report.cleanup_actions.end(),
        [](const auto& action) {
            return action.attempted &&
                action.result == myrd::M2CleanupResult::succeeded;
        }));
    REQUIRE_EQ(result.value().report.store_stats_samples.size(), 4u);
    REQUIRE_EQ(result.value().report.os_memory_samples.size(), 4u);
    REQUIRE_EQ(
        result.value().report.io.write_sha256,
        result.value().report.io.read_sha256);
    REQUIRE_EQ(
        result.value().report.device_identity.disk_length_bytes,
        64ull << 30);
    REQUIRE_EQ(
        result.value().report_path.parent_path(),
        std::filesystem::weakly_canonical(evidence.path()));
    REQUIRE(std::filesystem::is_regular_file(result.value().report_path));
    REQUIRE(myrd::validate_published_m2_report(
                result.value().report_path).ok());

    for (const auto& entry :
         std::filesystem::directory_iterator(evidence.path())) {
        REQUIRE(entry.path().filename().wstring().find(L".tmp-") ==
                std::wstring::npos);
    }
}

TEST_CASE(m2_local_fake_rejects_a_missing_evidence_directory) {
    TempEvidenceDirectory evidence;
    const auto missing = evidence.path() / "missing";

    const auto result = myrd::run_m2_local_fake(
        missing, "local-fake-missing-directory");

    REQUIRE(result.has_error());
    REQUIRE(!std::filesystem::exists(missing));
}
