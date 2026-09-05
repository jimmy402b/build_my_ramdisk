#include "test_framework.h"

#include "src/product/quantity_parser.h"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace {

using myrd::product::parse_binary_size;

struct ValidSizeCase {
    std::string_view text;
    std::uint64_t bytes;
};

struct InvalidSizeCase {
    std::string_view text;
};

} // namespace

TEST_CASE(binary_size_accepts_exact_binary_units) {
    constexpr ValidSizeCase cases[] = {
        {"1MiB", 1ULL << 20},
        {"2GiB", 2ULL << 30},
        {"3TiB", 3ULL << 40},
        {"1024MiB", 1ULL << 30},
    };

    for (const auto& test : cases) {
        auto result = parse_binary_size(test.text);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value(), test.bytes);
    }
}

TEST_CASE(binary_size_accepts_largest_nonoverflowing_multipliers) {
    constexpr ValidSizeCase cases[] = {
        {"17592186044415MiB", 18446744073708503040ULL},
        {"17179869183GiB", 18446744072635809792ULL},
        {"16777215TiB", 18446742974197923840ULL},
    };

    for (const auto& test : cases) {
        auto result = parse_binary_size(test.text);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value(), test.bytes);
    }
}

TEST_CASE(binary_size_rejects_zero) {
    constexpr InvalidSizeCase cases[] = {{"0MiB"}, {"00GiB"}, {"000TiB"}};
    for (const auto& test : cases) {
        REQUIRE(!parse_binary_size(test.text).ok());
    }
}

TEST_CASE(binary_size_rejects_overflow) {
    constexpr InvalidSizeCase cases[] = {
        {"17592186044416MiB"},
        {"17179869184GiB"},
        {"16777216TiB"},
        {"18446744073709551615MiB"},
        {"18446744073709551616MiB"},
    };
    for (const auto& test : cases) {
        REQUIRE(!parse_binary_size(test.text).ok());
    }
}

TEST_CASE(binary_size_rejects_signs_whitespace_and_fractions) {
    constexpr InvalidSizeCase cases[] = {
        {"+1MiB"}, {"-1MiB"}, {" 1MiB"}, {"1MiB "}, {"1 MiB"},
        {"1.0MiB"}, {".5GiB"}, {"1,5GiB"}, {"1e3MiB"},
    };
    for (const auto& test : cases) {
        REQUIRE(!parse_binary_size(test.text).ok());
    }
}

TEST_CASE(binary_size_rejects_missing_or_noncanonical_units) {
    constexpr InvalidSizeCase cases[] = {
        {""}, {"1"}, {"MiB"}, {"1B"}, {"1KB"}, {"1MB"},
        {"1mib"}, {"1MIB"}, {"1Gi"}, {"1GiBB"}, {"1MiBGiB"},
    };
    for (const auto& test : cases) {
        REQUIRE(!parse_binary_size(test.text).ok());
    }
}

TEST_CASE(checkpoint_interval_accepts_off) {
    auto result = myrd::product::parse_checkpoint_interval("off");
    REQUIRE(result.ok());
    REQUIRE(!result.value().enabled);
    REQUIRE_EQ(result.value().duration, std::chrono::seconds::zero());
}

TEST_CASE(checkpoint_interval_accepts_inclusive_bounds_and_exact_units) {
    struct Case {
        std::string_view text;
        std::chrono::seconds expected;
    };
    constexpr Case cases[] = {
        {"30s", std::chrono::seconds(30)},
        {"60s", std::chrono::minutes(1)},
        {"1m", std::chrono::minutes(1)},
        {"2h", std::chrono::hours(2)},
        {"7d", std::chrono::hours(24 * 7)},
        {"168h", std::chrono::hours(24 * 7)},
        {"10080m", std::chrono::hours(24 * 7)},
        {"604800s", std::chrono::hours(24 * 7)},
    };

    for (const auto& test : cases) {
        auto result = myrd::product::parse_checkpoint_interval(test.text);
        REQUIRE(result.ok());
        REQUIRE(result.value().enabled);
        REQUIRE_EQ(result.value().duration, test.expected);
    }
}

TEST_CASE(checkpoint_interval_rejects_values_outside_bounds) {
    constexpr std::string_view cases[] = {
        "0s", "1s", "29s", "0m", "604801s", "10081m", "169h", "8d",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::parse_checkpoint_interval(text).ok());
    }
}

TEST_CASE(checkpoint_interval_rejects_mixed_or_malformed_units) {
    constexpr std::string_view cases[] = {
        "", "OFF", "Off", "1", "s", "1ms", "30ss", "1h30m", "1d1h",
        "30S", "1M", "1H", "1D", "1.5m", ".5h", "1e2s", "+30s", "-30s",
        " 30s", "30s ", "30 s", "18446744073709551616s",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::parse_checkpoint_interval(text).ok());
    }
}

TEST_CASE(local_path_normalizes_drive_separators_and_dot_segments) {
    struct Case {
        std::wstring_view input;
        std::wstring_view expected;
    };
    constexpr Case cases[] = {
        {L"C:\\images\\disk.myrd", L"C:\\images\\disk.myrd"},
        {L"c:/images//disk.myrd/", L"C:\\images\\disk.myrd"},
        {L"D:\\images\\.\\set\\..\\disk", L"D:\\images\\disk"},
        {L"E:\\资料\\内存盘", L"E:\\资料\\内存盘"},
        {L"z:\\", L"Z:\\"},
    };
    for (const auto& test : cases) {
        auto result = myrd::product::normalize_local_absolute_path(test.input);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value(), std::wstring(test.expected));
    }
}

TEST_CASE(local_path_rejects_nonlocal_or_nonabsolute_forms) {
    constexpr std::wstring_view cases[] = {
        L"", L"relative\\bundle", L".\\bundle", L"..\\bundle",
        L"C:bundle", L"\\bundle", L"/bundle",
        L"\\\\server\\share\\bundle", L"//server/share/bundle",
        L"\\\\?\\C:\\bundle", L"\\\\.\\C:\\bundle",
        L"\\??\\C:\\bundle", L"\\Device\\HarddiskVolume1\\bundle",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_rejects_ads_invalid_characters_and_controls) {
    constexpr std::wstring_view cases[] = {
        L"C:\\bundle:stream", L"C:\\dir\\file:stream",
        L"C:\\bad<name", L"C:\\bad>name", L"C:\\bad\"name",
        L"C:\\bad|name", L"C:\\bad?name", L"C:\\bad*name",
        L"C:\\line\nbreak",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_rejects_trailing_space_or_dot_ambiguity) {
    constexpr std::wstring_view cases[] = {
        L"C:\\bundle ", L"C:\\bundle.", L"C:\\dir.\\bundle",
        L"C:\\dir \\bundle", L"C:\\bundle.\\child",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_rejects_reserved_dos_device_segments) {
    constexpr std::wstring_view cases[] = {
        L"C:\\CON", L"C:\\nul.txt", L"C:\\dir\\AUX",
        L"C:\\COM1\\bundle", L"C:\\lpt9.dat",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_rejects_parent_escape_above_drive_root) {
    constexpr std::wstring_view cases[] = {
        L"C:\\..\\bundle", L"C:\\dir\\..\\..\\bundle",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_rejects_product_owned_bundle_roots_case_insensitively) {
    constexpr std::wstring_view cases[] = {
        L"C:\\Program Files\\MyRamDisk",
        L"c:\\program files\\myramdisk\\images\\disk",
        L"C:\\ProgramData\\MyRamDisk",
        L"C:/PROGRAMDATA/MYRAMDISK/bundle",
    };
    for (const auto text : cases) {
        REQUIRE(!myrd::product::normalize_local_absolute_path(text).ok());
    }
}

TEST_CASE(local_path_allows_similar_but_unowned_roots) {
    constexpr std::wstring_view cases[] = {
        L"C:\\Program Files\\MyRamDiskData\\bundle",
        L"C:\\ProgramData\\OtherProduct\\bundle",
        L"D:\\ProgramData\\MyRamDisk\\bundle",
    };
    for (const auto text : cases) {
        REQUIRE(myrd::product::normalize_local_absolute_path(text).ok());
    }
}
