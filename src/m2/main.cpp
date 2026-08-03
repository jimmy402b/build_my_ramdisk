#include "m2_cli.h"
#include "m2_local_fake.h"
#include "m2_report.h"

#include "src/proxy/object_names.h"

#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string narrow_ascii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        result.push_back(
            ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
    }
    return result;
}

int print_error(const myrd::Error& error, int exit_code) {
    std::fwprintf(
        stderr,
        L"M2 error (code=%u): %ls: %ls\n",
        error.code,
        error.operation.c_str(),
        error.message.c_str());
    return exit_code;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::wstring_view> arguments;
        arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        auto parsed = myrd::parse_m2_cli(arguments);
        if (!parsed) {
            print_error(parsed.error(), 2);
            std::fwprintf(stderr, L"%ls", myrd::m2_cli_usage().data());
            return 2;
        }

        switch (parsed.value().mode) {
        case myrd::M2CliMode::help:
            std::wprintf(L"%ls", myrd::m2_cli_usage().data());
            return 0;

        case myrd::M2CliMode::print_schema:
            std::printf("%s\n", myrd::m2_report_json_schema().c_str());
            return 0;

        case myrd::M2CliMode::validate_report: {
            auto validation = myrd::validate_published_m2_report(
                parsed.value().report_to_validate);
            if (!validation) return print_error(validation.error(), 3);
            std::wprintf(
                L"M2 report is valid: %ls\n",
                parsed.value().report_to_validate.c_str());
            return 0;
        }

        case myrd::M2CliMode::local_fake: {
            auto session_id = myrd::proxy::generate_session_id();
            if (!session_id) return print_error(session_id.error(), 4);
            auto run = myrd::run_m2_local_fake(
                parsed.value().evidence_directory,
                narrow_ascii(session_id.value()));
            if (!run) return print_error(run.error(), 5);
            std::wprintf(
                L"M2 local fake report: %ls\n",
                run.value().report_path.c_str());
            return 0;
        }

        case myrd::M2CliMode::real:
            std::fwprintf(
                stderr,
                L"M2 real mode is fail-closed until the Task 20 Windows "
                L"device and NTFS acceptance adapters are installed.\n");
            return 6;
        }
    } catch (const std::exception&) {
        std::fwprintf(stderr, L"M2 failed with an unexpected exception.\n");
        return 70;
    } catch (...) {
        std::fwprintf(stderr, L"M2 failed with an unknown exception.\n");
        return 71;
    }

    std::fwprintf(stderr, L"M2 reached an invalid CLI state.\n");
    return 72;
}
