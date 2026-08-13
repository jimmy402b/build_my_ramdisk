#include "cli.h"

#include <array>
#include <charconv>
#include <cwctype>
#include <limits>

namespace myrd {
namespace {
constexpr std::uint64_t MiB = 1024ull * 1024;
constexpr std::uint64_t TiB = 1024ull * 1024 * 1024 * 1024;
CliParseResult error(std::wstring message = L"invalid command") { return {CliAction::Error, {}, {}, std::move(message)}; }
bool number(std::wstring_view value, std::uint64_t& out) {
    if (value.empty() || value.size() > 20) return false;
    std::array<char, 20> ascii{};
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] < L'0' || value[i] > L'9') return false;
        ascii[i] = static_cast<char>(value[i]);
    }
    auto [end, ec] = std::from_chars(ascii.data(), ascii.data() + value.size(), out);
    return ec == std::errc{} && end == ascii.data() + value.size();
}
bool sized(std::wstring_view value, std::uint64_t& out) {
    if (value.size() < 2) return false;
    std::uint64_t n{}; if (!number(value.substr(0, value.size() - 1), n)) return false;
    std::uint64_t scale{};
    switch (value.back()) { case L'K': scale=1024; break; case L'M': scale=MiB; break; case L'G': scale=MiB*1024; break; case L'T': scale=TiB; break; default: return false; }
    if (n > std::numeric_limits<std::uint64_t>::max() / scale) return false;
    out = n * scale; return true;
}
bool bundle_ok(std::wstring_view p) {
    if (p.empty() || p.find(L'\0') != std::wstring_view::npos) return false;
    auto lower = std::wstring(p); for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (lower.rfind(L"\\\\.\\", 0) == 0 || lower.rfind(L"\\\\?\\", 0) == 0 ||
        lower.rfind(L"\\??\\", 0) == 0 || lower.rfind(L"\\device\\", 0) == 0 ||
        lower.rfind(L"\\global??\\", 0) == 0 || lower.rfind(L"\\dosdevices\\", 0) == 0) return false;
    auto colon = p.find(L':');
    return colon == std::wstring_view::npos ||
        (colon == 1 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
         p.find(L':', colon + 1) == std::wstring_view::npos);
}
}
CliParseResult parse_cli(std::span<const std::wstring_view> args) {
    if (args.size() == 1 && args[0] == L"--help") return {CliAction::Help};
    if (args.empty() || (args[0] != L"run" && args[0] != L"compact")) return error(L"unknown command");
    bool is_run = args[0] == L"run";
    if (args.size() == 2 && args[1] == L"--help") return {CliAction::Help};
    if (args.size() < 3) return error(L"missing required option");
    std::optional<std::wstring_view> bundle, size, drive, seconds, dirty, ram;
    for (size_t i=1; i<args.size(); i += 2) {
        if (i + 1 == args.size()) return error(L"missing option value"); auto k=args[i], v=args[i+1];
        auto set=[&](auto& slot) { if (slot) return false; slot=v; return true; };
        if (k == L"--bundle") { if (!set(bundle)) return error(L"duplicate option"); }
        else if (is_run && k == L"--size") { if (!set(size)) return error(L"duplicate option"); }
        else if (is_run && k == L"--drive-letter") { if (!set(drive)) return error(L"duplicate option"); }
        else if (is_run && k == L"--checkpoint-seconds") { if (!set(seconds)) return error(L"duplicate option"); }
        else if (is_run && k == L"--checkpoint-dirty-mb") { if (!set(dirty)) return error(L"duplicate option"); }
        else if (is_run && k == L"--ram-limit") { if (!set(ram)) return error(L"duplicate option"); }
        else return error(L"unknown option");
    }
    if (!bundle || !bundle_ok(*bundle)) return error(L"invalid bundle path");
    if (!is_run) return {CliAction::Compact, {}, CompactOptions{std::filesystem::path(*bundle)}};
    if (!size || !drive) return error(L"missing required option");
    if (drive->size()!=2 || (*drive)[1]!=L':' || !(((*drive)[0]>=L'A'&&(*drive)[0]<=L'Z')||((*drive)[0]>=L'a'&&(*drive)[0]<=L'z'))) return error(L"invalid drive letter");
    RunOptions result; result.bundle=std::filesystem::path(*bundle); result.drive_letter=static_cast<wchar_t>(std::towupper((*drive)[0]));
    if (!sized(*size,result.virtual_bytes) || result.virtual_bytes < 64*MiB || result.virtual_bytes > TiB) return error(L"invalid size");
    std::uint64_t n{};
    if (seconds && (!number(*seconds,n) || n<5 || n>86400)) return error(L"invalid checkpoint seconds"); if (seconds) result.checkpoint_interval=std::chrono::seconds(n);
    if (dirty && (!number(*dirty,n) || n<1 || n>1048576)) return error(L"invalid checkpoint dirty mb"); if (dirty) result.checkpoint_dirty_bytes=n*MiB;
    if (ram && (!sized(*ram,n) || n<64*MiB || n>TiB)) return error(L"invalid ram limit"); if (ram) result.ram_limit_bytes=n;
    return {CliAction::Run, result};
}
}
