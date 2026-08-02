#include "m1_cli.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace myrd {

static std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
        static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(),
        static_cast<int>(w.size()), result.data(), len, nullptr, nullptr);
    return result;
}

std::optional<M1CliConfig> parse_m1_cli(int argc, wchar_t* argv[]) {
    M1CliConfig config;

    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);

        if (arg == L"run") {
            config.command = "run";
        } else if (arg == L"--report" && i + 1 < argc) {
            ++i;
            config.report_path = narrow(argv[i]);
        } else if (arg == L"--debug-failure-point") {
            config.debug_failure_point = true;
        }
    }

    if (config.command.empty()) return std::nullopt;
    if (config.command == "run" && config.report_path.empty()) return std::nullopt;

    return config;
}

} // namespace myrd
