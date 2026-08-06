#include "test_framework.h"

#include "src/m3/m3_cli.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using myrd::m3::M3CliCommand;

myrd::Result<myrd::m3::M3CliConfig> parse(
    const std::vector<std::wstring>& storage) {
    std::vector<std::wstring_view> views;
    views.reserve(storage.size());
    for (const auto& value : storage) views.emplace_back(value);
    return myrd::m3::parse_m3_cli(views);
}

myrd::Result<myrd::m3::M3CliConfig> parse(
    std::initializer_list<std::wstring> values) {
    return parse(std::vector<std::wstring>(values));
}

std::vector<std::wstring> common(std::wstring command) {
    return {
        std::move(command),
        L"--bundle-root", LR"(C:\myrd-never-created\bundle)",
        L"--manifest", L"disk.manifest",
        L"--timeout-ms", L"5000",
        L"--max-virtual-bytes", L"1073741824",
        L"--max-journal-bytes", L"134217728",
        L"--max-journal-records", L"1024",
        L"--max-index-cache-bytes", L"65536",
        L"--max-index-cache-entries", L"4",
        L"--max-bat-cache-bytes", L"65536",
        L"--max-bat-cache-entries", L"1024",
        L"--max-io-scratch-bytes", L"65536",
        L"--max-verify-batch-bytes", L"65536",
    };
}

void append_geometry(std::vector<std::wstring>& values) {
    values.insert(values.end(), {
        L"--virtual-bytes", L"67108864",
        L"--logical-sector-size", L"512",
        L"--physical-sector-size", L"4096",
    });
}

void append_store(std::vector<std::wstring>& values) {
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

void replace_value(std::vector<std::wstring>& values,
                   std::wstring_view option,
                   std::wstring replacement) {
    const auto found = std::find(values.begin(), values.end(), option);
    REQUIRE(found != values.end());
    REQUIRE(found + 1 != values.end());
    *(found + 1) = std::move(replacement);
}

void erase_option(std::vector<std::wstring>& values,
                  std::wstring_view option) {
    const auto found = std::find(values.begin(), values.end(), option);
    REQUIRE(found != values.end());
    REQUIRE(found + 1 != values.end());
    values.erase(found, found + 2);
}

} // namespace

TEST_CASE(m3_cli_has_structured_help_and_all_explicit_commands) {
    const auto help = parse({L"--help"});
    REQUIRE(help.ok());
    REQUIRE_EQ(help.value().command, M3CliCommand::Help);

    constexpr std::array commands{
        M3CliCommand::Create,
        M3CliCommand::Inspect,
        M3CliCommand::Verify,
        M3CliCommand::Checkpoint,
        M3CliCommand::Compact,
        M3CliCommand::RecoverTest,
        M3CliCommand::FaultMatrix,
    };
    for (const auto command : commands) {
        const auto parsed = myrd::m3::parse_m3_cli_command(
            myrd::m3::command_name(command));
        REQUIRE(parsed.ok());
        REQUIRE_EQ(parsed.value(), command);
    }
    REQUIRE(myrd::m3::parse_m3_cli_command("unknown").has_error());
    REQUIRE(myrd::m3::m3_cli_usage().find(L"fault-matrix") !=
            std::wstring_view::npos);
    REQUIRE(myrd::m3::m3_cli_usage().find(L"--virtual-bytes") !=
            std::wstring_view::npos);
}

TEST_CASE(m3_cli_parser_is_pure_and_requires_complete_finite_common_inputs) {
    const auto absent_path = LR"(C:\myrd-never-created\bundle)";
    auto values = common(L"inspect");
    auto parsed = parse(values);
    REQUIRE(parsed.ok());
    REQUIRE_EQ(parsed.value().command, M3CliCommand::Inspect);
    REQUIRE_EQ(parsed.value().bundle_root,
               std::filesystem::path(absent_path));
    REQUIRE_EQ(parsed.value().manifest_name, "disk.manifest");
    REQUIRE(!parsed.value().geometry.has_value());
    REQUIRE(!parsed.value().store_options.has_value());

    constexpr std::array required{
        L"--bundle-root", L"--manifest", L"--timeout-ms",
        L"--max-virtual-bytes", L"--max-journal-bytes",
        L"--max-journal-records", L"--max-index-cache-bytes",
        L"--max-index-cache-entries", L"--max-bat-cache-bytes",
        L"--max-bat-cache-entries", L"--max-io-scratch-bytes",
        L"--max-verify-batch-bytes",
    };
    for (const auto option : required) {
        auto missing = values;
        erase_option(missing, option);
        REQUIRE(parse(missing).has_error());
    }

    replace_value(values, L"--bundle-root", LR"(\\.\PhysicalDrive0)");
    const auto device = parse(values);
    REQUIRE(device.has_error());
    REQUIRE(device.error().message.find(L"PhysicalDrive0") ==
            std::wstring::npos);

    values = common(L"inspect");
    replace_value(values, L"--manifest", LR"(..\disk.manifest)");
    REQUIRE(parse(values).has_error());
}

TEST_CASE(m3_cli_create_requires_only_one_valid_explicit_geometry) {
    auto values = common(L"create");
    REQUIRE(parse(values).has_error());
    append_geometry(values);
    const auto parsed = parse(values);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().geometry.has_value());
    REQUIRE_EQ(parsed.value().geometry->virtual_bytes, 67108864ull);
    REQUIRE_EQ(parsed.value().geometry->logical_sector_size, 512u);
    REQUIRE_EQ(parsed.value().geometry->physical_sector_size, 4096u);

    auto invalid = values;
    replace_value(invalid, L"--logical-sector-size", L"1024");
    REQUIRE(parse(invalid).has_error());

    auto inspect = common(L"inspect");
    append_geometry(inspect);
    REQUIRE(parse(inspect).has_error());
}

TEST_CASE(m3_cli_store_and_barrier_inputs_are_command_scoped) {
    auto checkpoint = common(L"checkpoint");
    checkpoint.insert(checkpoint.end(), {L"--barrier", L"raw-test"});
    REQUIRE(parse(checkpoint).has_error());
    append_store(checkpoint);
    const auto parsed = parse(checkpoint);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().store_options.has_value());
    REQUIRE_EQ(*parsed.value().barrier,
               myrd::persistence::VolumeBarrierKind::
                   RawBlockTestNoFilesystemCache);

    auto fake_flush = checkpoint;
    replace_value(fake_flush, L"--barrier", L"windows-flushed");
    REQUIRE(parse(fake_flush).has_error());

    auto inspect = common(L"inspect");
    append_store(inspect);
    REQUIRE(parse(inspect).has_error());
    inspect = common(L"inspect");
    inspect.insert(inspect.end(), {L"--barrier", L"raw-test"});
    REQUIRE(parse(inspect).has_error());
}

TEST_CASE(m3_cli_verify_and_test_only_options_are_command_scoped) {
    auto verify = common(L"verify");
    verify.push_back(L"--full");
    const auto parsed = parse(verify);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().full_verify);

    auto inspect = common(L"inspect");
    inspect.push_back(L"--full");
    REQUIRE(parse(inspect).has_error());

    auto recover = common(L"recover-test");
    append_store(recover);
    recover.insert(recover.end(), {L"--inject-phase", L"journal.after-flush"});
    REQUIRE(parse(recover).ok());

    auto matrix = common(L"fault-matrix");
    append_store(matrix);
    REQUIRE(parse(matrix).has_error());
    matrix.insert(matrix.end(), {L"--inject-phase", L"manifest.flush.before"});
    REQUIRE(parse(matrix).ok());

    auto create = common(L"create");
    append_geometry(create);
    create.insert(create.end(), {L"--inject-phase", L"anything"});
    REQUIRE(parse(create).has_error());
}

TEST_CASE(m3_cli_rejects_numeric_overflow_hidden_unlimited_and_bad_order) {
    auto values = common(L"inspect");
    replace_value(values, L"--timeout-ms", L"-1");
    REQUIRE(parse(values).has_error());
    replace_value(values, L"--timeout-ms", L"18446744073709551616");
    REQUIRE(parse(values).has_error());

    values = common(L"inspect");
    replace_value(values, L"--max-journal-bytes",
                  L"18446744073709551615");
    REQUIRE(parse(values).has_error());

    auto checkpoint = common(L"checkpoint");
    append_store(checkpoint);
    checkpoint.insert(checkpoint.end(), {L"--barrier", L"raw-test"});
    replace_value(checkpoint, L"--store-soft-bytes", L"268435456");
    REQUIRE(parse(checkpoint).has_error());

    checkpoint = common(L"checkpoint");
    append_store(checkpoint);
    checkpoint.insert(checkpoint.end(), {L"--barrier", L"raw-test"});
    replace_value(checkpoint, L"--snapshot-cow-bytes",
                  L"18446744073709551615");
    REQUIRE(parse(checkpoint).has_error());

    checkpoint = common(L"checkpoint");
    append_store(checkpoint);
    checkpoint.insert(checkpoint.end(), {L"--barrier", L"raw-test"});
    replace_value(checkpoint, L"--snapshot-stall-ms",
                  L"9223372036854775808");
    REQUIRE(parse(checkpoint).has_error());
}

TEST_CASE(m3_cli_rejects_duplicate_unknown_missing_and_extra_help_arguments) {
    auto values = common(L"inspect");
    values.insert(values.end(), {L"--timeout-ms", L"50"});
    REQUIRE(parse(values).has_error());

    values = common(L"inspect");
    values.push_back(L"--unknown");
    REQUIRE(parse(values).has_error());

    values = common(L"inspect");
    values.push_back(L"--timeout-ms");
    REQUIRE(parse(values).has_error());

    REQUIRE(parse({L"--help", L"--unknown"}).has_error());
    REQUIRE(parse({}).has_error());
}
