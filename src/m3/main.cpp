#include "m3_cli.h"
#include "m3_lifecycle_runner.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_safe_parser_error(const myrd::Error& error) {
    std::fprintf(
        stderr, "m3 argument error (domain=%u code=%u)\n",
        static_cast<unsigned>(error.domain), error.code);
}

void print_parser_failure_json(const myrd::Error& error) {
    std::printf(
        "{\"schema_version\":1,\"command\":\"unknown\","
        "\"ok\":false,\"commit_disposition\":\"NotCommitted\","
        "\"generation\":0,\"error_domain\":%u,\"error_code\":%u}\n",
        static_cast<unsigned>(error.domain), error.code);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::wstring_view> args;
        args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }

        if (args.empty() || args.front() == L"help" ||
            args.front() == L"--help") {
            std::wprintf(L"%ls", myrd::m3::m3_cli_usage().data());
            return 0;
        }
        auto parsed = myrd::m3::parse_m3_cli(args);
        if (!parsed.ok()) {
            print_parser_failure_json(parsed.error());
            print_safe_parser_error(parsed.error());
            return myrd::m3::kM3CliUsageErrorCode;
        }
        const auto deadline = std::chrono::steady_clock::now() +
            parsed.value().timeout;
        const auto outcome = myrd::m3::run_m3_lifecycle(
            myrd::OperationContext{{}, deadline}, parsed.value());
        std::printf("%s\n", outcome.json.c_str());
        if (!outcome.diagnostics.empty()) {
            std::fputs(outcome.diagnostics.c_str(), stderr);
        }
        return outcome.exit_code;
    } catch (const std::exception&) {
        std::fputs("m3 unexpected exception\n", stderr);
        return 70;
    } catch (...) {
        std::fputs("m3 unknown exception\n", stderr);
        return 71;
    }
}
