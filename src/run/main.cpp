#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t* argv[]) {
    constexpr std::wstring_view usage =
        L"Usage: myrd run --bundle <path> --size <N> --drive-letter <R:> [options]\n";

    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::wcout << usage;
        return 0;
    }

    std::wcerr << usage;
    return 2;
}
