#include "test_framework.h"

#define MYRD_M4_AIM_QUERY_NO_MAIN 1
#include "tests/e2e/m4_aim_query.cpp"

#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring emitted(std::uint32_t api_version, std::uint32_t sub_version,
                     const std::vector<std::uint32_t>& ids) {
    std::wostringstream out;
    write_m4_query_json(out, api_version, sub_version, ids);
    return out.str();
}

} // namespace

// The controller accepts exactly one JSON object and nothing else, so the
// emission must stay a single line with no BOM, no prologue and no trailing
// payload beyond the one newline.
TEST_CASE(m4_aim_query_success_json_is_byte_exact) {
    REQUIRE_EQ(emitted(1, 2, {}),
               std::wstring(L"{\"success\":true,\"api_version\":1,"
                            L"\"sub_version\":2,\"ids\":[]}\n"));
}

TEST_CASE(m4_aim_query_ids_are_a_bare_numeric_array) {
    REQUIRE_EQ(emitted(0x1000, 7, {5}),
               std::wstring(L"{\"success\":true,\"api_version\":4096,"
                            L"\"sub_version\":7,\"ids\":[5]}\n"));
    REQUIRE_EQ(emitted(1, 1, {0, 3, 42}),
               std::wstring(L"{\"success\":true,\"api_version\":1,"
                            L"\"sub_version\":1,\"ids\":[0,3,42]}\n"));
}

TEST_CASE(m4_aim_query_emits_no_bom_and_exactly_one_newline) {
    const auto text = emitted(1, 2, {9, 9});
    REQUIRE(text.front() == L'{');
    REQUIRE_EQ(text.find(static_cast<wchar_t>(0xFEFF)), std::wstring::npos);
    REQUIRE_EQ(std::count(text.begin(), text.end(), L'\n'), std::ptrdiff_t{1});
    REQUIRE_EQ(text.find(L'\n'), text.size() - 1);
}

// `success` is a bare JSON boolean and the version fields are bare numbers:
// no quoting, so a strict consumer can reject string-typed impostors.
TEST_CASE(m4_aim_query_success_and_versions_are_unquoted) {
    const auto text = emitted(11, 22, {});
    REQUIRE(text.find(L"\"success\":true") != std::wstring::npos);
    REQUIRE_EQ(text.find(L"\"success\":\"true\""), std::wstring::npos);
    REQUIRE(text.find(L"\"api_version\":11") != std::wstring::npos);
    REQUIRE(text.find(L"\"sub_version\":22") != std::wstring::npos);
    REQUIRE_EQ(text.find(L"\"api_version\":\""), std::wstring::npos);
    REQUIRE_EQ(text.find(L"\"sub_version\":\""), std::wstring::npos);
}

TEST_CASE(m4_aim_query_large_values_stay_numeric) {
    REQUIRE_EQ(emitted(4294967295u, 4294967294u, {4294967293u}),
               std::wstring(L"{\"success\":true,\"api_version\":4294967295,"
                            L"\"sub_version\":4294967294,"
                            L"\"ids\":[4294967293]}\n"));
}
