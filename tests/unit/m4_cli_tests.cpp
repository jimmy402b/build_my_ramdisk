#include "test_framework.h"
#include "src/run/cli.h"
#include <string_view>
#include <string>
#include <vector>

using myrd::CliAction;
static std::vector<std::wstring_view> a(std::initializer_list<const wchar_t*> v) { return {v.begin(), v.end()}; }
static myrd::CliParseResult run(std::initializer_list<const wchar_t*> extra = {}) {
    std::vector<std::wstring_view> v{L"run",L"--bundle",L"bundle",L"--size",L"64M",L"--drive-letter",L"R:"};
    for (auto x : extra) v.push_back(x); return myrd::parse_cli(v);
}
static void rejected(std::vector<std::wstring_view> v) { auto r=myrd::parse_cli(v); REQUIRE_EQ(r.action,CliAction::Error); REQUIRE(!r.message.empty()); }
static myrd::RunOptions accepted_run(std::initializer_list<const wchar_t*> extra) {
    std::vector<std::wstring_view> v{L"run",L"--bundle",L"bundle",L"--size",L"64M",L"--drive-letter",L"R:"};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (*it == std::wstring_view(L"--size")) { v[4] = *++it; }
        else { v.push_back(*it); }
    }
    auto r = myrd::parse_cli(v); REQUIRE_EQ(r.action, CliAction::Run); REQUIRE(r.run.has_value()); return *r.run;
}

TEST_CASE(cli_help_actions) {
    REQUIRE_EQ(myrd::parse_cli(a({L"--help"})).action, CliAction::Help);
    REQUIRE_EQ(myrd::parse_cli(a({L"run", L"--help"})).action, CliAction::Help);
    REQUIRE_EQ(myrd::parse_cli(a({L"compact", L"--help"})).action, CliAction::Help);
}
TEST_CASE(cli_run_defaults_and_normalizes_drive) {
    auto r = myrd::parse_cli(a({L"run",L"--bundle",L"bundle",L"--size",L"64M",L"--drive-letter",L"r:"}));
    REQUIRE_EQ(r.action, CliAction::Run); REQUIRE(r.run.has_value());
    REQUIRE_EQ(r.run->virtual_bytes, 64ull*1024*1024); REQUIRE_EQ(r.run->drive_letter, L'R');
    REQUIRE_EQ(r.run->checkpoint_interval.count(), 600ll); REQUIRE_EQ(r.run->checkpoint_dirty_bytes, 256ull*1024*1024);
    REQUIRE_EQ(r.run->ram_limit_bytes, 2ull*1024*1024*1024);
}
TEST_CASE(cli_accepts_each_size_suffix_and_every_boundary) {
    REQUIRE_EQ(run({L"--size",L"64M"}).action, CliAction::Error); // duplicate size
    REQUIRE_EQ(accepted_run({L"--size",L"65536K"}).virtual_bytes, 64ull*1024*1024);
    REQUIRE_EQ(accepted_run({L"--size",L"64M"}).virtual_bytes, 64ull*1024*1024);
    REQUIRE_EQ(accepted_run({L"--size",L"1G"}).virtual_bytes, 1024ull*1024*1024);
    REQUIRE_EQ(accepted_run({L"--size",L"1T"}).virtual_bytes, 1024ull*1024*1024*1024);
}
TEST_CASE(cli_rejects_all_numeric_malformed_forms) {
    for (auto value : {L"+64M",L"-64M",L"64.0M",L" 64M",L"64M ",L"64",L"64m",L"18446744073709551616M"}) rejected(a({L"run",L"--bundle",L"x",L"--size",value,L"--drive-letter",L"R:"}));
}
TEST_CASE(cli_reaches_each_optional_numeric_policy) {
    auto optional = [](const wchar_t* option, const wchar_t* value) { return myrd::parse_cli(a({L"run",L"--bundle",L"x",L"--size",L"64M",L"--drive-letter",L"R:",option,value})); };
    for (auto value : {L"+5",L"-5",L"5.0",L" 5",L"5 ",L"",L"18446744073709551616"}) REQUIRE_EQ(optional(L"--checkpoint-seconds",value).action,CliAction::Error);
    REQUIRE_EQ(accepted_run({L"--checkpoint-seconds",L"5"}).checkpoint_interval.count(),5ll); REQUIRE_EQ(accepted_run({L"--checkpoint-seconds",L"86400"}).checkpoint_interval.count(),86400ll);
    for (auto value : {L"+1",L"-1",L"1.0",L" 1",L"1 ",L"",L"18446744073709551616"}) REQUIRE_EQ(optional(L"--checkpoint-dirty-mb",value).action,CliAction::Error);
    REQUIRE_EQ(accepted_run({L"--checkpoint-dirty-mb",L"1"}).checkpoint_dirty_bytes,1024ull*1024); REQUIRE_EQ(accepted_run({L"--checkpoint-dirty-mb",L"1048576"}).checkpoint_dirty_bytes,1048576ull*1024*1024);
    for (auto value : {L"+64M",L"-64M",L"64.0M",L" 64M",L"64M ",L"64",L"18446744073709551616M"}) REQUIRE_EQ(optional(L"--ram-limit",value).action,CliAction::Error);
    REQUIRE_EQ(accepted_run({L"--ram-limit",L"64M"}).ram_limit_bytes,64ull*1024*1024); REQUIRE_EQ(accepted_run({L"--ram-limit",L"1T"}).ram_limit_bytes,1024ull*1024*1024*1024);
}
TEST_CASE(cli_requires_values_and_required_options) {
    for (auto option : {L"--bundle",L"--size",L"--drive-letter",L"--checkpoint-seconds",L"--checkpoint-dirty-mb",L"--ram-limit"}) rejected(a({L"run",option}));
    rejected(a({L"run",L"--size",L"64M",L"--drive-letter",L"R:"}));
    rejected(a({L"run",L"--bundle",L"x",L"--drive-letter",L"R:"}));
    rejected(a({L"run",L"--bundle",L"x",L"--size",L"64M"}));
}
TEST_CASE(cli_rejects_duplicates_unknown_case_and_compact_run_options) {
    for (auto option : {L"--bundle",L"--size",L"--drive-letter",L"--checkpoint-seconds",L"--checkpoint-dirty-mb",L"--ram-limit"}) rejected(a({L"run",L"--bundle",L"x",L"--size",L"64M",L"--drive-letter",L"R:",option,L"1",option,L"1"}));
    rejected(a({L"RUN",L"--bundle",L"x"})); rejected(a({L"run",L"--Bundle",L"x"})); rejected(a({L"compact",L"--bundle",L"x",L"--size",L"64M"}));
}
TEST_CASE(cli_rejects_every_malformed_drive) {
    for (auto drive : {L"",L"R",L"RR",L"R::",L"1:",L" :",L"Ä:",L"r;"}) rejected(a({L"run",L"--bundle",L"x",L"--size",L"64M",L"--drive-letter",drive}));
}
TEST_CASE(cli_rejects_all_disallowed_bundle_forms) {
    for (auto path : {L"",L"C:\\x:stream",L"\\\\.\\PhysicalDrive0",L"\\\\?\\C:\\x",L"\\\\?\\GLOBALROOT\\Device",L"\\Device\\HarddiskVolume1",L"\\??\\C:\\x",L"\\\\.\\Volume{abc}",L"\\GLOBAL??\\root",L"\\DosDevices\\root",L"\\gLoBaL??\\root",L"\\dOsDeViCeS\\root"}) rejected(a({L"compact",L"--bundle",path}));
    std::wstring nul{L"x\0y",3}; rejected({L"compact",L"--bundle",std::wstring_view(nul)});
}
TEST_CASE(cli_accepts_ordinary_path_words_that_are_not_namespaces) {
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"C:\\globalroot-not-device"})).action,CliAction::Compact);
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"C:\\physicaldrive_notes"})).action,CliAction::Compact);
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"C:\\GLOBAL??-notes"})).action,CliAction::Compact);
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"C:\\DosDevices-notes"})).action,CliAction::Compact);
}
TEST_CASE(cli_rejects_compact_incomplete_unknown_and_noncanonical_forms) {
    for (auto values : {a({L"compact"}),a({L"compact",L"--bundle"}),a({L"compact",L"x"}),a({L"compact",L"--bundle",L"x",L"--bundle",L"y"}),a({L"compact",L"--bundle",L"x",L"--wat"}),a({L"compact",L"--bundle",L"x",L"trailing"}),a({L"compact",L"--help",L"x"}),a({L"Compact",L"--help"})}) rejected(values);
    for (auto option : {L"--size",L"--drive-letter",L"--checkpoint-seconds",L"--checkpoint-dirty-mb",L"--ram-limit"}) rejected(a({L"compact",L"--bundle",L"x",option,L"1"}));
}
TEST_CASE(cli_compact_and_reject_errors) {
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"x"})).action, CliAction::Compact);
    REQUIRE_EQ(myrd::parse_cli(a({})).action, CliAction::Error);
    REQUIRE_EQ(myrd::parse_cli(a({L"run",L"--bundle",L"x"})).action, CliAction::Error);
    REQUIRE_EQ(myrd::parse_cli(a({L"run",L"--bundle",L"x",L"--size",L"64M",L"--drive-letter",L"R:",L"--wat"})).action, CliAction::Error);
}
TEST_CASE(cli_enforces_numeric_bounds_and_syntax) {
    auto run = [](const wchar_t* size, const wchar_t* seconds=L"600", const wchar_t* dirty=L"256", const wchar_t* ram=L"2G") {
        return myrd::parse_cli(a({L"run",L"--bundle",L"x",L"--size",size,L"--drive-letter",L"R:",L"--checkpoint-seconds",seconds,L"--checkpoint-dirty-mb",dirty,L"--ram-limit",ram})); };
    REQUIRE_EQ(run(L"63M").action, CliAction::Error); REQUIRE_EQ(run(L"1T").action, CliAction::Run);
    REQUIRE_EQ(run(L"64m").action, CliAction::Error); REQUIRE_EQ(run(L"64M ").action, CliAction::Error);
    REQUIRE_EQ(run(L"64M", L"4").action, CliAction::Error); REQUIRE_EQ(run(L"64M", L"86401").action, CliAction::Error);
    REQUIRE_EQ(run(L"64M", L"5", L"0").action, CliAction::Error); REQUIRE_EQ(run(L"64M", L"5", L"1048577").action, CliAction::Error);
    REQUIRE_EQ(run(L"64M", L"5", L"1", L"63M").action, CliAction::Error); REQUIRE_EQ(run(L"64M", L"5", L"1", L"1T").action, CliAction::Run);
}
TEST_CASE(cli_rejects_device_ads_duplicate_and_bad_drive_paths) {
    auto bad = [](const wchar_t* bundle) { return myrd::parse_cli(a({L"compact",L"--bundle",bundle})); };
    REQUIRE_EQ(bad(L"\\\\.\\PhysicalDrive0").action, CliAction::Error); REQUIRE_EQ(bad(L"\\\\?\\GLOBALROOT\\Device").action, CliAction::Error);
    REQUIRE_EQ(bad(L"C:\\bundle:stream").action, CliAction::Error);
    REQUIRE_EQ(myrd::parse_cli(a({L"compact",L"--bundle",L"x",L"--bundle",L"y"})).action, CliAction::Error);
    REQUIRE_EQ(myrd::parse_cli(a({L"run",L"--bundle",L"x",L"--size",L"64M",L"--drive-letter",L"RR"})).action, CliAction::Error);
}
