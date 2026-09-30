#include "src/product/quantity_parser.h"

#include <limits>
#include <string>
#include <vector>

namespace myrd::product {

namespace {

Result<std::uint64_t> invalid_size(const wchar_t* message) {
    return Result<std::uint64_t>::failure(Error{
        ErrorDomain::usage, 1, L"parse_binary_size", message});
}

Result<CheckpointInterval> invalid_interval(const wchar_t* message) {
    return Result<CheckpointInterval>::failure(Error{
        ErrorDomain::usage, 2, L"parse_checkpoint_interval", message});
}

Result<std::wstring> invalid_path(const wchar_t* message) {
    return Result<std::wstring>::failure(Error{
        ErrorDomain::usage, 3, L"normalize_local_absolute_path", message});
}

bool is_separator(wchar_t ch) noexcept {
    return ch == L'\\' || ch == L'/';
}

bool is_ascii_letter(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

wchar_t ascii_upper(wchar_t ch) noexcept {
    return ch >= L'a' && ch <= L'z' ? static_cast<wchar_t>(ch - L'a' + L'A') : ch;
}

wchar_t ascii_lower(wchar_t ch) noexcept {
    return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

bool has_invalid_path_character(std::wstring_view segment) noexcept {
    for (const wchar_t ch : segment) {
        if (ch < 32 || ch == L':' || ch == L'<' || ch == L'>' || ch == L'"'
            || ch == L'|' || ch == L'?' || ch == L'*') {
            return true;
        }
    }
    return false;
}

bool is_reserved_dos_name(std::wstring_view segment) {
    const auto dot = segment.find(L'.');
    const auto base = segment.substr(0, dot);
    std::wstring folded;
    folded.reserve(base.size());
    for (const wchar_t ch : base) {
        folded.push_back(ascii_upper(ch));
    }
    if (folded == L"CON" || folded == L"PRN" || folded == L"AUX" || folded == L"NUL") {
        return true;
    }
    return folded.size() == 4
        && (folded.starts_with(L"COM") || folded.starts_with(L"LPT"))
        && folded[3] >= L'1' && folded[3] <= L'9';
}

std::wstring ascii_fold(std::wstring_view text) {
    std::wstring folded;
    folded.reserve(text.size());
    for (const wchar_t ch : text) {
        folded.push_back(ascii_lower(ch));
    }
    return folded;
}

bool is_at_or_below(std::wstring_view path, std::wstring_view root) noexcept {
    return path == root
        || (path.size() > root.size() && path.starts_with(root) && path[root.size()] == L'\\');
}

} // namespace

Result<std::uint64_t> parse_binary_size(std::string_view text) {
    std::uint64_t multiplier = 0;
    std::size_t suffix_length = 0;

    if (text.ends_with("MiB")) {
        multiplier = 1ULL << 20;
        suffix_length = 3;
    } else if (text.ends_with("GiB")) {
        multiplier = 1ULL << 30;
        suffix_length = 3;
    } else if (text.ends_with("TiB")) {
        multiplier = 1ULL << 40;
        suffix_length = 3;
    } else {
        return invalid_size(L"Size must end with MiB, GiB, or TiB");
    }

    const auto digits = text.substr(0, text.size() - suffix_length);
    if (digits.empty()) {
        return invalid_size(L"Size must contain a positive integer");
    }

    std::uint64_t value = 0;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') {
            return invalid_size(L"Size must contain decimal digits only");
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (kMax - digit) / 10) {
            return invalid_size(L"Size is too large");
        }
        value = value * 10 + digit;
    }

    if (value == 0) {
        return invalid_size(L"Size must be greater than zero");
    }
    if (value > kMax / multiplier) {
        return invalid_size(L"Size is too large");
    }

    return Result<std::uint64_t>::success(value * multiplier);
}

Result<CheckpointInterval> parse_checkpoint_interval(std::string_view text) {
    if (text == "off") {
        return Result<CheckpointInterval>::success(
            CheckpointInterval{false, std::chrono::seconds::zero()});
    }
    if (text.size() < 2) {
        return invalid_interval(L"Duration must contain an integer and one unit");
    }

    std::uint64_t multiplier = 0;
    switch (text.back()) {
    case 's': multiplier = 1; break;
    case 'm': multiplier = 60; break;
    case 'h': multiplier = 60 * 60; break;
    case 'd': multiplier = 24 * 60 * 60; break;
    default:
        return invalid_interval(L"Duration must end with s, m, h, or d");
    }

    const auto digits = text.substr(0, text.size() - 1);
    std::uint64_t value = 0;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') {
            return invalid_interval(L"Duration must contain decimal digits only");
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (kMax - digit) / 10) {
            return invalid_interval(L"Duration is too large");
        }
        value = value * 10 + digit;
    }

    constexpr std::uint64_t kMinimumSeconds = 30;
    constexpr std::uint64_t kMaximumSeconds = 7 * 24 * 60 * 60;
    if (value > kMaximumSeconds / multiplier) {
        return invalid_interval(L"Duration exceeds seven days");
    }
    const auto seconds = value * multiplier;
    if (seconds < kMinimumSeconds || seconds > kMaximumSeconds) {
        return invalid_interval(L"Duration must be between 30 seconds and seven days");
    }

    return Result<CheckpointInterval>::success(CheckpointInterval{
        true, std::chrono::seconds(static_cast<std::chrono::seconds::rep>(seconds))});
}

Result<std::wstring> normalize_local_absolute_path(std::wstring_view text) {
    if (text.size() < 3 || !is_ascii_letter(text[0]) || text[1] != L':'
        || !is_separator(text[2])) {
        return invalid_path(L"Path must be an absolute local drive path");
    }

    std::vector<std::wstring> segments;
    std::size_t cursor = 3;
    while (cursor < text.size()) {
        while (cursor < text.size() && is_separator(text[cursor])) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }
        const auto start = cursor;
        while (cursor < text.size() && !is_separator(text[cursor])) {
            ++cursor;
        }
        const auto segment = text.substr(start, cursor - start);
        if (segment == L".") {
            continue;
        }
        if (segment == L"..") {
            if (segments.empty()) {
                return invalid_path(L"Path cannot escape above the drive root");
            }
            segments.pop_back();
            continue;
        }
        if (segment.back() == L' ' || segment.back() == L'.') {
            return invalid_path(L"Path segments cannot end in a space or dot");
        }
        if (has_invalid_path_character(segment)) {
            return invalid_path(L"Path contains an invalid NTFS character");
        }
        if (is_reserved_dos_name(segment)) {
            return invalid_path(L"Path contains a reserved DOS device name");
        }
        segments.emplace_back(segment);
    }

    std::wstring normalized;
    normalized.reserve(text.size());
    normalized.push_back(ascii_upper(text[0]));
    normalized += L":\\";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            normalized.push_back(L'\\');
        }
        normalized += segments[i];
    }

    const auto folded = ascii_fold(normalized);
    constexpr std::wstring_view kProgramFilesRoot = L"c:\\program files\\myramdisk";
    constexpr std::wstring_view kProgramDataRoot = L"c:\\programdata\\myramdisk";
    if (is_at_or_below(folded, kProgramFilesRoot)
        || is_at_or_below(folded, kProgramDataRoot)) {
        return invalid_path(L"Bundle paths cannot be stored under product-owned directories");
    }

    return Result<std::wstring>::success(std::move(normalized));
}

} // namespace myrd::product
