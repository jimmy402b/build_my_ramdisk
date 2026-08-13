#include "cli.h"
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    constexpr std::wstring_view global = L"Usage:\n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]\n  myrd compact --bundle <path>\n";
    constexpr std::wstring_view run = L"Usage:\n  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]\n";
    constexpr std::wstring_view compact = L"Usage:\n  myrd compact --bundle <path>\n";
    std::vector<std::wstring_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    auto parsed = myrd::parse_cli(args);
    if (args.empty()) { std::wcerr << global; return 2; }
    if (parsed.action == myrd::CliAction::Help) { std::wcout << (argc == 2 ? global : std::wstring_view(argv[1]) == L"run" ? run : compact); return 0; }
    if (parsed.action == myrd::CliAction::Error) { std::wcerr << parsed.message << L"\n" << global; return 2; }
    std::wcerr << (parsed.action == myrd::CliAction::Run ? L"run implementation unavailable\n" : L"compact implementation unavailable\n"); return 1;
}
