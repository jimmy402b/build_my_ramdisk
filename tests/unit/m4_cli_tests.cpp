#include "test_framework.h"

#include "src/run/cli.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using myrd::CliAction;

std::vector<std::wstring_view> args(
    std::initializer_list<const wchar_t*> values) {
    return {values.begin(), values.end()};
}

myrd::CliParseResult valid_run(
    std::initializer_list<const wchar_t*> extra = {}) {
    std::vector<std::wstring_view> values{
        L"run", L"--bundle", L"bundle", L"--size", L"64M",
        L"--drive-letter", L"R:"};
    values.insert(values.end(), extra.begin(), extra.end());
    return myrd::parse_cli(values);
}

void require_rejected(std::vector<std::wstring_view> values) {
    const auto result = myrd::parse_cli(values);
    REQUIRE_EQ(result.action, CliAction::Error);
    REQUIRE(!result.message.empty());
}

} // namespace

TEST_CASE(m4_cli_help_actions) {
    REQUIRE_EQ(myrd::parse_cli(args({L"--help"})).action, CliAction::Help);
    REQUIRE_EQ(myrd::parse_cli(args({L"run", L"--help"})).action,
               CliAction::Help);
    REQUIRE_EQ(myrd::parse_cli(args({L"compact", L"--help"})).action,
               CliAction::Help);
}

TEST_CASE(m4_cli_run_defaults_and_normalizes_drive_letter) {
    const auto result = myrd::parse_cli(args({
        L"run", L"--bundle", L"bundle", L"--size", L"64M",
        L"--drive-letter", L"r:"}));
    REQUIRE_EQ(result.action, CliAction::Run);
    REQUIRE(result.run.has_value());
    REQUIRE_EQ(result.run->virtual_bytes, 64ull * 1024 * 1024);
    REQUIRE_EQ(result.run->drive_letter, L'R');
    REQUIRE_EQ(result.run->checkpoint_interval.count(), 60ll);
    REQUIRE_EQ(result.run->checkpoint_dirty_bytes, 64ull * 1024 * 1024);
    REQUIRE_EQ(result.run->ram_limit_bytes, 2ull * 1024 * 1024 * 1024);
}

TEST_CASE(m4_cli_accepts_supported_size_boundaries) {
    const auto k = valid_run({L"--size", L"65536K"});
    REQUIRE_EQ(k.action, CliAction::Error); // duplicate required option

    for (const auto* size : {L"64M", L"1G", L"1T"}) {
        std::vector<std::wstring_view> values{
            L"run", L"--bundle", L"bundle", L"--size", size,
            L"--drive-letter", L"R:"};
        const auto result = myrd::parse_cli(values);
        REQUIRE_EQ(result.action, CliAction::Run);
    }
    for (const auto* size : {L"2T", L"18446744073709551615T"}) {
        require_rejected(args({L"run", L"--bundle", L"bundle", L"--size", size,
                               L"--drive-letter", L"R:"}));
    }
}

TEST_CASE(m4_cli_rejects_malformed_sizes_and_integer_overflow) {
    for (const auto* size : {L"+64M", L"-64M", L"64.0M", L" 64M",
                             L"64M ", L"64", L"64m",
                             L"18446744073709551616M"}) {
        require_rejected(args({L"run", L"--bundle", L"x", L"--size", size,
                               L"--drive-letter", L"R:"}));
    }
    require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"63M",
                           L"--drive-letter", L"R:"}));
}

TEST_CASE(m4_cli_optional_numeric_ranges_and_overflow) {
    const auto parse = [](const wchar_t* option, const wchar_t* value) {
        return myrd::parse_cli(args({L"run", L"--bundle", L"x", L"--size",
                                     L"64M", L"--drive-letter", L"R:", option,
                                     value}));
    };
    for (const auto* value : {L"+5", L"-5", L"5.0", L" 5", L"5 ", L"",
                              L"18446744073709551616"}) {
        REQUIRE_EQ(parse(L"--checkpoint-seconds", value).action,
                    CliAction::Error);
    }
    const auto seconds = parse(L"--checkpoint-seconds", L"5");
    REQUIRE_EQ(seconds.action, CliAction::Run);
    REQUIRE(seconds.run.has_value());
    REQUIRE_EQ(seconds.run->checkpoint_interval.count(), 5ll);
    REQUIRE_EQ(parse(L"--checkpoint-seconds", L"4").action, CliAction::Error);
    REQUIRE_EQ(parse(L"--checkpoint-seconds", L"86401").action,
                CliAction::Error);

    for (const auto* value : {L"+1", L"-1", L"1.0", L" 1", L"1 ", L"",
                              L"18446744073709551616"}) {
        REQUIRE_EQ(parse(L"--checkpoint-dirty-mb", value).action,
                    CliAction::Error);
    }
    const auto dirty = parse(L"--checkpoint-dirty-mb", L"1");
    REQUIRE_EQ(dirty.action, CliAction::Run);
    REQUIRE(dirty.run.has_value());
    REQUIRE_EQ(dirty.run->checkpoint_dirty_bytes, 1ull * 1024 * 1024);
    REQUIRE_EQ(parse(L"--checkpoint-dirty-mb", L"0").action,
                CliAction::Error);
    REQUIRE_EQ(parse(L"--checkpoint-dirty-mb", L"1048577").action,
                CliAction::Error);

    for (const auto* value : {L"+64M", L"-64M", L"64.0M", L"64M ",
                              L"18446744073709551616M"}) {
        REQUIRE_EQ(parse(L"--ram-limit", value).action, CliAction::Error);
    }
    const auto ram_min = parse(L"--ram-limit", L"64M");
    REQUIRE_EQ(ram_min.action, CliAction::Run);
    REQUIRE(ram_min.run.has_value());
    REQUIRE_EQ(ram_min.run->ram_limit_bytes, 64ull * 1024 * 1024);
    const auto ram_max = parse(L"--ram-limit", L"1T");
    REQUIRE_EQ(ram_max.action, CliAction::Run);
    REQUIRE(ram_max.run.has_value());
    REQUIRE_EQ(ram_max.run->ram_limit_bytes, 1ull * 1024 * 1024 * 1024 * 1024);
    REQUIRE_EQ(parse(L"--ram-limit", L"63M").action, CliAction::Error);
    REQUIRE_EQ(parse(L"--ram-limit", L"2T").action, CliAction::Error);
}

TEST_CASE(m4_cli_requires_values_and_required_options) {
    for (const auto* option : {L"--bundle", L"--size", L"--drive-letter",
                               L"--checkpoint-seconds",
                               L"--checkpoint-dirty-mb", L"--ram-limit"}) {
        require_rejected(args({L"run", option}));
    }
    require_rejected(args({L"run", L"--size", L"64M", L"--drive-letter",
                           L"R:"}));
    require_rejected(args({L"run", L"--bundle", L"x", L"--drive-letter",
                           L"R:"}));
    require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"64M"}));
}

TEST_CASE(m4_cli_rejects_duplicate_unknown_and_compact_options) {
    for (const auto* option : {L"--bundle", L"--size", L"--drive-letter",
                               L"--checkpoint-seconds",
                               L"--checkpoint-dirty-mb", L"--ram-limit"}) {
        require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"64M",
                               L"--drive-letter", L"R:", option, L"1", option,
                               L"1"}));
    }
    require_rejected(args({L"RUN", L"--bundle", L"x"}));
    require_rejected(args({L"run", L"--Bundle", L"x"}));
    require_rejected(args({L"compact", L"--bundle", L"x", L"--size", L"64M"}));
    require_rejected(args({L"compact", L"--bundle", L"x", L"--bundle", L"y"}));
    require_rejected(args({L"compact", L"--bundle", L"x", L"--wat"}));
    require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"64M",
                           L"--drive-letter", L"R:", L"trailing"}));
    require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"64M",
                           L"--drive-letter", L"R:", L"--wat", L"1"}));
}

TEST_CASE(m4_cli_rejects_malformed_drive_letters) {
    for (const auto* drive : {L"", L"R", L"RR", L"R::", L"1:", L" :",
                              L"Ä:", L"r;"}) {
        require_rejected(args({L"run", L"--bundle", L"x", L"--size", L"64M",
                               L"--drive-letter", drive}));
    }
}

TEST_CASE(m4_cli_rejects_device_namespace_bundle_paths) {
    for (const auto* path : {L"", L"C:\\x:stream", L"\\\\.\\PhysicalDrive0",
                             L"\\\\?\\C:\\x", L"\\\\?\\GLOBALROOT\\Device",
                             L"\\Device\\HarddiskVolume1",
                             L"\\??\\C:\\x", L"\\\\.\\Volume{abc}",
                             L"\\GLOBAL??\\root", L"\\DosDevices\\root"}) {
        require_rejected(args({L"compact", L"--bundle", path}));
    }
    std::wstring with_nul{L"x\0y", 3};
    require_rejected({L"compact", L"--bundle", std::wstring_view(with_nul)});

    for (const auto* path : {L"\\\\.\\PhysicalDrive0",
                             L"\\\\?\\GLOBALROOT\\Device",
                             L"\\Device\\HarddiskVolume1"}) {
        require_rejected(args({L"run", L"--bundle", path, L"--size", L"64M",
                               L"--drive-letter", L"R:"}));
    }
}

TEST_CASE(m4_cli_accepts_compact_ordinary_paths_and_rejects_incomplete) {
    for (const auto* path : {L"C:\\globalroot-not-device",
                             L"C:\\physicaldrive_notes",
                             L"C:\\DosDevices-notes"}) {
        REQUIRE_EQ(myrd::parse_cli(args({L"compact", L"--bundle", path})).action,
                   CliAction::Compact);
    }
    for (const auto values : {args({L"compact"}), args({L"compact", L"--bundle"}),
                              args({L"compact", L"x"}),
                              args({L"compact", L"--bundle", L"x", L"trailing"}),
                              args({L"compact", L"--help", L"x"})}) {
        require_rejected(values);
    }
}
