#include "test_framework.h"

#include "src/m3/m3_cli.h"
#include "src/m3/m3_lifecycle_runner.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class TempBundle final {
public:
    TempBundle() {
        root = std::filesystem::temp_directory_path() /
            (L"myrd-m3-runner-" + std::to_wstring(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        std::filesystem::create_directories(root, error);
        REQUIRE(!error);
    }
    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

myrd::m3::M3CliConfig config(
    const TempBundle& bundle, std::wstring command) {
    const bool is_create = command == L"create";
    const bool is_checkpoint = command == L"checkpoint";
    const bool is_compact = command == L"compact";
    const bool is_recover_test = command == L"recover-test";
    const bool is_fault_matrix = command == L"fault-matrix";
    std::vector<std::wstring> values{
        std::move(command), L"--bundle-root", bundle.root.wstring(),
        L"--manifest", L"disk.manifest", L"--timeout-ms", L"5000",
        L"--max-virtual-bytes", L"67108864",
        L"--max-journal-bytes", L"134217728",
        L"--max-journal-records", L"1024",
        L"--max-index-cache-bytes", L"65536",
        L"--max-index-cache-entries", L"4",
        L"--max-bat-cache-bytes", L"65536",
        L"--max-bat-cache-entries", L"16",
        L"--max-io-scratch-bytes", L"65536",
        L"--max-verify-batch-bytes", L"65536",
    };
    if (is_create) {
        values.insert(values.end(), {
            L"--virtual-bytes", L"67108864",
            L"--logical-sector-size", L"512",
            L"--physical-sector-size", L"4096",
        });
    }
    if (is_checkpoint || is_compact || is_recover_test || is_fault_matrix) {
        values.insert(values.end(), {
            L"--store-soft-bytes", L"134217728",
            L"--store-hard-bytes", L"268435456",
            L"--store-low-watermark-bytes", L"67108864",
            L"--store-batch-bytes", L"4194304",
            L"--store-batch-records", L"64",
            L"--store-stripes", L"64",
            L"--snapshot-cow-bytes", L"134217728",
            L"--snapshot-stall-ms", L"250",
        });
    }
    if (is_checkpoint || is_compact) {
        values.insert(values.end(), {L"--barrier", L"raw-test"});
    }
    if (is_fault_matrix) {
        values.insert(values.end(), {L"--inject-phase", L"recovery.before-select"});
    }
    std::vector<std::wstring_view> views;
    for (const auto& value : values) views.emplace_back(value);
    auto parsed = myrd::m3::parse_m3_cli(views);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::OperationContext context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(10)};
}

} // namespace

TEST_CASE(m3_lifecycle_runner_create_recover_inspect_and_verify_use_production_paths) {
    TempBundle bundle;
    const auto created = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"create"));
    REQUIRE_EQ(created.exit_code, 0);
    REQUIRE(created.json.find("\"command\":\"create\"") !=
            std::string::npos);

    const auto recovered = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"recover-test"));
    REQUIRE_EQ(recovered.exit_code, 0);

    const auto inspected = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"inspect"));
    REQUIRE_EQ(inspected.exit_code, 0);
    REQUIRE(inspected.json.find("myrd-m3-runner") == std::string::npos);

    const auto verified = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"verify"));
    REQUIRE_EQ(verified.exit_code, 0);
}

TEST_CASE(m3_lifecycle_runner_checkpoint_reopens_and_verifies_new_generation) {
    TempBundle bundle;
    REQUIRE_EQ(myrd::m3::run_m3_lifecycle(
                    context(), config(bundle, L"create"))
                    .exit_code,
                0);
    const auto checkpoint = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"checkpoint"));
    REQUIRE_EQ(checkpoint.exit_code, 0);
    REQUIRE(checkpoint.json.find("\"generation\":1") !=
            std::string::npos);

    const auto verify = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"verify"));
    REQUIRE_EQ(verify.exit_code, 0);
    REQUIRE(verify.json.find("\"generation\":1") != std::string::npos);
}

TEST_CASE(m3_lifecycle_runner_compact_uses_production_compaction_and_reopens) {
    TempBundle bundle;
    REQUIRE_EQ(myrd::m3::run_m3_lifecycle(
                    context(), config(bundle, L"create"))
                    .exit_code,
                0);
    const auto compacted = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"compact"));
    REQUIRE_EQ(compacted.exit_code, 0);
    const auto recovered = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"recover-test"));
    REQUIRE_EQ(recovered.exit_code, 0);
    REQUIRE(recovered.json.find("\"generation\":1") != std::string::npos);
    const auto compacted_again = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"compact"));
    REQUIRE_EQ(compacted_again.exit_code, 0);
    REQUIRE(compacted_again.json.find("\"generation\":2") !=
            std::string::npos);
}

TEST_CASE(m3_lifecycle_runner_fault_matrix_is_explicit_and_path_safe) {
    TempBundle bundle;
    const auto outcome = myrd::m3::run_m3_lifecycle(
        context(), config(bundle, L"fault-matrix"));
    REQUIRE_EQ(outcome.exit_code, 1);
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.json.find("\"command\":\"fault-matrix\"") !=
            std::string::npos);
    REQUIRE(outcome.json.find("\"commit_disposition\":\"NotCommitted\"") !=
            std::string::npos);
    REQUIRE(outcome.json.find(bundle.root.string()) == std::string::npos);
    REQUIRE_EQ(outcome.diagnostics, std::string{"m3 command failed\n"});
}
