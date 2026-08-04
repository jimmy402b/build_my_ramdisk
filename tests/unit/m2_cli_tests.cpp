#include "test_framework.h"

#include "src/m2/m2_cli.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace {

myrd::Result<myrd::M2CliConfig> parse(
    std::initializer_list<std::wstring> values) {
    std::vector<std::wstring> storage(values);
    std::vector<std::wstring_view> views;
    views.reserve(storage.size());
    for (const auto& value : storage) views.emplace_back(value);
    return myrd::parse_m2_cli(views);
}

class TempInputTree {
public:
    TempInputTree() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            (L"myrd-m2-cli-" + std::to_wstring(suffix));
        std::error_code error;
        std::filesystem::create_directories(root / L"evidence", error);
        REQUIRE(!error);
        std::ofstream(root / L"m1-accepted.json") << "{}";
    }

    ~TempInputTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path evidence() const { return root / L"evidence"; }
    std::filesystem::path m1_report() const {
        return root / L"m1-accepted.json";
    }

    std::filesystem::path root;
};

std::vector<std::wstring> real_test_policy_args(const TempInputTree& tree) {
    return {
        L"--real",
        L"--evidence-dir", tree.evidence().wstring(),
        L"--accepted-m1-report", tree.m1_report().wstring(),
        L"--expected-driver-service", L"phdskmnt",
        L"--expected-driver-sha256",
        L"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        L"--expected-bus-type", L"Fibre Channel",
        L"--expected-vendor-id", L"MYRD",
        L"--expected-product-id", L"DynamicDisk",
        L"--disk-size-bytes", L"68719476736",
        L"--test-policy",
        L"--max-snapshot-batch-bytes", L"67108864",
        L"--max-snapshot-batch-records", L"1024",
        L"--lock-stripe-count", L"64",
        L"--i-understand-this-test-initializes-a-new-disk"};
}

myrd::Result<myrd::M2CliConfig> parse_storage(
    const std::vector<std::wstring>& storage) {
    std::vector<std::wstring_view> views;
    views.reserve(storage.size());
    for (const auto& value : storage) views.emplace_back(value);
    return myrd::parse_m2_cli(views);
}

void erase_option(std::vector<std::wstring>& values,
                  std::wstring_view option,
                  bool has_value) {
    const auto it = std::find(values.begin(), values.end(), option);
    REQUIRE(it != values.end());
    const auto count = has_value ? 2 : 1;
    values.erase(it, it + count);
}

} // namespace

TEST_CASE(m2_cli_requires_exactly_one_explicit_mode) {
    REQUIRE(parse({}).has_error());
    REQUIRE_EQ(parse({L"--help"}).value().mode, myrd::M2CliMode::help);
    REQUIRE_EQ(
        parse({L"--print-schema"}).value().mode,
        myrd::M2CliMode::print_schema);
    REQUIRE(parse({L"--help", L"--print-schema"}).has_error());
    REQUIRE(parse({L"--help", L"--wat"}).has_error());
    REQUIRE(parse({L"--local-fake"}).has_error());
    REQUIRE(parse({L"--real"}).has_error());
}

TEST_CASE(m2_cli_validate_mode_requires_one_existing_regular_report) {
    TempInputTree tree;
    auto parsed = parse(
        {L"--validate-report", tree.m1_report().wstring()});
    REQUIRE(parsed.ok());
    REQUIRE_EQ(parsed.value().mode, myrd::M2CliMode::validate_report);
    REQUIRE(parsed.value().report_to_validate.is_absolute());

    REQUIRE(parse({L"--validate-report"}).has_error());
    REQUIRE(parse(
        {L"--validate-report", (tree.root / L"missing.json").wstring()})
                .has_error());
    REQUIRE(parse({L"--validate-report", tree.evidence().wstring()})
                .has_error());
    REQUIRE(parse({L"--validate-report", LR"(\\.\PhysicalDrive0)"})
                .has_error());
    REQUIRE(parse(
        {L"--validate-report", tree.m1_report().wstring(), L"--help"})
                .has_error());
}

TEST_CASE(m2_cli_local_fake_requires_an_existing_explicit_evidence_directory) {
    TempInputTree tree;
    auto parsed = parse(
        {L"--local-fake", L"--evidence-dir", tree.evidence().wstring()});
    REQUIRE(parsed.ok());
    REQUIRE_EQ(parsed.value().mode, myrd::M2CliMode::local_fake);
    REQUIRE(parsed.value().evidence_directory.is_absolute());

    REQUIRE(parse({L"--local-fake", L"--evidence-dir"}).has_error());
    REQUIRE(parse({L"--local-fake", L"--evidence-dir",
                   (tree.root / L"missing").wstring()}).has_error());
    REQUIRE(parse({L"--local-fake", L"--evidence-dir",
                   tree.m1_report().wstring()}).has_error());
    REQUIRE(parse({L"--local-fake", L"--evidence-dir",
                   LR"(\\?\C:\evidence)"}).has_error());
    REQUIRE(parse({L"--local-fake", L"--evidence-dir",
                   tree.evidence().wstring(), L"--unknown"}).has_error());
}

TEST_CASE(m2_cli_real_test_policy_requires_every_identity_and_safety_input) {
    TempInputTree tree;
    auto values = real_test_policy_args(tree);
    auto parsed = parse_storage(values);
    REQUIRE(parsed.ok());
    REQUIRE_EQ(parsed.value().mode, myrd::M2CliMode::real);
    REQUIRE(parsed.value().safety_acknowledged);
    REQUIRE(parsed.value().geometry.test_policy_selected);
    REQUIRE_EQ(parsed.value().geometry.disk_size_bytes, 64ull << 30);
    REQUIRE_EQ(parsed.value().geometry.sector_size_bytes, 512u);
    REQUIRE_EQ(parsed.value().geometry.block_size_bytes, 64ull << 10);
    REQUIRE(parsed.value().geometry.soft_limit_bytes > 0);
    REQUIRE(
        parsed.value().geometry.eviction_low_watermark_bytes <=
        parsed.value().geometry.soft_limit_bytes);
    REQUIRE(
        parsed.value().geometry.soft_limit_bytes <
        parsed.value().geometry.hard_limit_bytes);
    REQUIRE_EQ(parsed.value().expected_driver_service, "phdskmnt");
    REQUIRE_EQ(parsed.value().expected_bus_type, "Fibre Channel");

    struct RequiredOption {
        const wchar_t* name;
        bool has_value;
    };
    const RequiredOption required[] = {
        {L"--evidence-dir", true},
        {L"--accepted-m1-report", true},
        {L"--expected-driver-service", true},
        {L"--expected-driver-sha256", true},
        {L"--expected-bus-type", true},
        {L"--expected-vendor-id", true},
        {L"--expected-product-id", true},
        {L"--disk-size-bytes", true},
        {L"--test-policy", false},
        {L"--max-snapshot-batch-bytes", true},
        {L"--max-snapshot-batch-records", true},
        {L"--lock-stripe-count", true},
        {L"--i-understand-this-test-initializes-a-new-disk", false},
    };
    for (const auto& option : required) {
        auto missing = values;
        erase_option(missing, option.name, option.has_value);
        REQUIRE(parse_storage(missing).has_error());
    }
}

TEST_CASE(m2_cli_test_policy_derives_spec_ram_percentages_and_alignment) {
    constexpr std::uint64_t physical = 64ull << 30;
    constexpr std::uint64_t alignment = 64ull << 10;
    const auto aligned_down = [](std::uint64_t value) {
        return value - (value % alignment);
    };

    auto limits = myrd::derive_m2_test_policy_limits(physical);
    REQUIRE(limits.ok());
    REQUIRE_EQ(limits.value().soft_limit_bytes, aligned_down(physical / 4));
    REQUIRE_EQ(limits.value().hard_limit_bytes, aligned_down(physical / 2));
    REQUIRE_EQ(
        limits.value().eviction_low_watermark_bytes,
        aligned_down((physical / 4) * 4 / 5));
    REQUIRE(myrd::derive_m2_test_policy_limits(128ull << 10).has_error());
}

TEST_CASE(m2_cli_real_rejects_wrong_geometry_identity_and_ambiguous_policy) {
    TempInputTree tree;

    auto values = real_test_policy_args(tree);
    const auto disk = std::find(
        values.begin(), values.end(), L"--disk-size-bytes");
    REQUIRE(disk != values.end());
    *(disk + 1) = L"34359738368";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    const auto hash = std::find(
        values.begin(), values.end(), L"--expected-driver-sha256");
    REQUIRE(hash != values.end());
    *(hash + 1) = L"not-a-sha256";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    const auto bus = std::find(
        values.begin(), values.end(), L"--expected-bus-type");
    REQUIRE(bus != values.end());
    *(bus + 1) = L" Fibre Channel";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    const auto product = std::find(
        values.begin(), values.end(), L"--expected-product-id");
    REQUIRE(product != values.end());
    *(product + 1) = L"Virtual\nDisk";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    values.push_back(L"--soft-limit-bytes");
    values.push_back(L"4294967296");
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    values.push_back(L"--real");
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    values.push_back(L"--wat");
    REQUIRE(parse_storage(values).has_error());
}

TEST_CASE(m2_cli_real_accepts_only_complete_valid_explicit_store_limits) {
    TempInputTree tree;
    auto values = real_test_policy_args(tree);
    erase_option(values, L"--test-policy", false);
    const auto insertion = std::find(
        values.begin(), values.end(), L"--max-snapshot-batch-bytes");
    REQUIRE(insertion != values.end());
    values.insert(
        insertion,
        {L"--soft-limit-bytes", L"4294967296",
         L"--hard-limit-bytes", L"8589934592",
         L"--low-watermark-bytes", L"3221225472"});

    auto parsed = parse_storage(values);
    REQUIRE(parsed.ok());
    REQUIRE(!parsed.value().geometry.test_policy_selected);
    REQUIRE_EQ(parsed.value().geometry.soft_limit_bytes, 4294967296ull);
    REQUIRE_EQ(parsed.value().geometry.hard_limit_bytes, 8589934592ull);
    REQUIRE_EQ(
        parsed.value().geometry.eviction_low_watermark_bytes,
        3221225472ull);

    auto partial = values;
    erase_option(partial, L"--low-watermark-bytes", true);
    REQUIRE(parse_storage(partial).has_error());

    auto invalid = values;
    const auto hard = std::find(
        invalid.begin(), invalid.end(), L"--hard-limit-bytes");
    REQUIRE(hard != invalid.end());
    *(hard + 1) = L"4294967296";
    REQUIRE(parse_storage(invalid).has_error());

    invalid = values;
    const auto soft = std::find(
        invalid.begin(), invalid.end(), L"--soft-limit-bytes");
    REQUIRE(soft != invalid.end());
    *(soft + 1) = L"4294967297";
    REQUIRE(parse_storage(invalid).has_error());
}

TEST_CASE(m2_cli_numeric_inputs_reject_zero_overflow_and_bad_shape) {
    TempInputTree tree;
    auto values = real_test_policy_args(tree);
    const auto records = std::find(
        values.begin(), values.end(), L"--max-snapshot-batch-records");
    REQUIRE(records != values.end());
    *(records + 1) = L"0";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    const auto batch = std::find(
        values.begin(), values.end(), L"--max-snapshot-batch-bytes");
    REQUIRE(batch != values.end());
    *(batch + 1) = L"18446744073709551616";
    REQUIRE(parse_storage(values).has_error());

    values = real_test_policy_args(tree);
    const auto stripes = std::find(
        values.begin(), values.end(), L"--lock-stripe-count");
    REQUIRE(stripes != values.end());
    *(stripes + 1) = L"63";
    REQUIRE(parse_storage(values).has_error());
}

TEST_CASE(m2_cli_real_accepts_only_explicit_main_path_failure_injection_points) {
    TempInputTree tree;
    const std::pair<std::wstring, myrd::M2PhaseId> accepted[] = {
        {L"capture-baseline", myrd::M2PhaseId::capture_baseline},
        {L"construct-provider", myrd::M2PhaseId::construct_provider},
        {L"start-provider", myrd::M2PhaseId::start_provider},
        {L"create-device", myrd::M2PhaseId::create_device},
        {L"wait-for-disk", myrd::M2PhaseId::wait_for_disk},
        {L"setup-write", myrd::M2PhaseId::setup_write},
        {L"sample-after-write", myrd::M2PhaseId::sample_after_write},
        {L"delete-retrim", myrd::M2PhaseId::delete_retrim},
        {L"sample-after-retrim", myrd::M2PhaseId::sample_after_retrim},
        {L"performance", myrd::M2PhaseId::performance},
    };

    for (const auto& [name, phase] : accepted) {
        auto values = real_test_policy_args(tree);
        values.insert(
            values.end() - 1,
            {L"--inject-failure-after", name});
        const auto parsed = parse_storage(values);
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value().injected_failure_after.has_value());
        REQUIRE_EQ(*parsed.value().injected_failure_after, phase);
    }

    auto invalid = real_test_policy_args(tree);
    invalid.insert(
        invalid.end() - 1,
        {L"--inject-failure-after", L"remove-device"});
    REQUIRE(parse_storage(invalid).has_error());

    invalid = real_test_policy_args(tree);
    invalid.insert(
        invalid.end() - 1,
        {L"--inject-failure-after", L"setup-write",
         L"--inject-failure-after", L"performance"});
    REQUIRE(parse_storage(invalid).has_error());

    REQUIRE(parse({L"--local-fake", L"--evidence-dir",
                   tree.evidence().wstring(), L"--inject-failure-after",
                   L"setup-write"}).has_error());
}

int main() {
    for (const auto& test : myrd::test::registry()) {
        test.func();
        std::cout << "PASS " << test.name << '\n';
    }
    return 0;
}
