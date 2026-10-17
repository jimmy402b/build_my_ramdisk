#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "src/run/cli.h"
#include "src/run/disk_runtime.h"
#include "src/run/driver_preflight.h"
#include "src/run/m4_persistence_adapter.h"
#include "src/run/run_loop.h"
#include "src/run/runtime_adapter.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace myrd;

constexpr std::wstring_view kUsage =
    L"Usage:\n"
    L"  myrd run --bundle <path> --size <N> --drive-letter <R:> "
    L"[--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] "
    L"[--ram-limit <N>]\n"
    L"  myrd compact --bundle <path>\n";

std::atomic_bool g_shutdown_requested{false};

BOOL WINAPI console_handler(const DWORD event) {
    return request_console_shutdown(g_shutdown_requested, event) ? TRUE : FALSE;
}

Error product_error(const ErrorDomain domain, const std::uint32_t code,
                    const wchar_t* operation, const wchar_t* message,
                    const std::uint32_t native_code = 0) {
    return {domain, code, operation, message, native_code};
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
std::wstring test_seam_value(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) return {};
    value.resize(written);
    return value;
}
#endif

void print_error(const Error& error) {
    std::wcerr << L"Error: " << error.operation << L": "
               << error.message;
    if (error.native_code != 0) {
        std::wcerr << L" (Windows error " << error.native_code << L")";
    }
    std::wcerr << L"\n";
}

int run_product(const RunOptions& options) {
    // This gate is intentionally ahead of every bundle status/open/create
    // operation.  A missing or disabled driver therefore leaves the bundle
    // untouched, including a not-yet-created bundle directory.
#if defined(MYRD_ENABLE_TEST_SEAMS)
    const auto preflight_seam = test_seam_value(L"MYRD_M4_TEST_PREFLIGHT");
    if (preflight_seam == L"unavailable") {
        print_error(product_error(
            ErrorDomain::usage, 1, L"driver_unavailable",
            L"phdskmnt is disabled or absent"));
        return 1;
    }
    if (preflight_seam != L"available") {
#endif
        const auto elevated = require_elevated_administrator();
        if (!elevated.ok()) {
            print_error(elevated.error());
            return 1;
        }
        const auto driver = require_phdskmnt_service_available();
        if (!driver.ok()) {
            print_error(driver.error());
            return 1;
        }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    }
#endif

    const auto bundle_kind = classify_bundle(options.bundle);
    if (!bundle_kind.ok()) {
        print_error(bundle_kind.error());
        return 1;
    }

#if defined(MYRD_ENABLE_TEST_SEAMS)
    // Exercise executable-level mode dispatch without creating an AIM device
    // or mutating a Windows volume.  The seam is compiled only in test builds.
    const auto entry_seam = test_seam_value(L"MYRD_M4_TEST_ENTRY");
    if (!entry_seam.empty()) {
        const auto expected = entry_seam == L"create"
            ? DiskBundleKind::empty : DiskBundleKind::manifest;
        if ((entry_seam != L"create" && entry_seam != L"recover") ||
            bundle_kind.value() != expected) {
            print_error(product_error(
                ErrorDomain::usage, 1, L"test_entry",
                L"test entry does not match bundle classification"));
            return 1;
        }
        std::wcout << L"Test " << entry_seam << L" entry selected.\n";
        return 0;
    }
#endif

    // Foreground composition is now the shared production adapter plus this
    // CLI's own bundle/letter selection.  The adapter is the same object the
    // per-profile worker uses, so `myrd run` and the M5 worker cannot drift
    // into two execution implementations.
    ProductionRuntimeAdapter adapter(ProductionRuntimeAdapter::Config{
        options.bundle, options.virtual_bytes, options.drive_letter,
        L"myrd:" + std::to_wstring(GetCurrentProcessId()),
        options.ram_limit_bytes,
        // The foreground path derives its identity as before.
        std::nullopt,
        // A non-empty bundle is recovered, never provisioned, so it attaches to the volume that already
        // exists.  Stating that here is what lets the M3 runtime skip locating the physical drive -- which
        // it would otherwise do by opening every candidate disk, measured blocking for 11 seconds while a
        // device retires.  An empty bundle is a create and still needs the drive number.
        bundle_kind.value() != DiskBundleKind::empty});
    RuntimeAdapterDependencies dependencies{
        options.bundle,
        options.virtual_bytes,
        options.drive_letter,
        adapter,
        adapter,
        adapter.config().owner_token,
        make_production_environment()};

    auto operations = make_runtime_operations(dependencies);
    if (!operations.ok()) {
        print_error(operations.error());
        return 1;
    }

    DiskRuntimeOptions runtime_options{options.bundle, options.virtual_bytes,
                                       options.drive_letter,
                                       std::chrono::seconds(30)};
    auto runtime = bundle_kind.value() == DiskBundleKind::empty
        ? DiskRuntime::create(runtime_options, operations.move_value())
        : DiskRuntime::recover(runtime_options, operations.move_value());
    if (!runtime.ok()) {
        print_error(runtime.error());
        return 1;
    }
    const auto started = runtime.value()->start();
    if (!started.ok()) {
        print_error(started.error());
        return 1;
    }

    const auto stats = runtime.value()->maintenance_stats();
    std::wcout << L"Ready: " << options.drive_letter << L": generation "
               << stats.generation << L".\n" << std::flush;
    g_shutdown_requested.store(false);
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        const auto error = GetLastError();
        (void)runtime.value()->shutdown_clean();
        print_error(product_error(ErrorDomain::win32, error,
            L"SetConsoleCtrlHandler", L"console handler registration failed", error));
        return 1;
    }
    const auto exit_code = run_loop(*runtime.value(), options, g_shutdown_requested);
    if (!SetConsoleCtrlHandler(console_handler, FALSE)) {
        const auto error = GetLastError();
        print_error(product_error(ErrorDomain::win32, error,
            L"SetConsoleCtrlHandler", L"console handler removal failed", error));
        return 1;
    }
    return exit_code;
}

int compact_product(const CompactOptions& options) {
    const OperationContext context{};
    const auto outcome = m4_compact_offline(
        context, options.bundle);
    if (outcome.exit_code == 0) {
        std::wcout << L"Compacted bundle to generation " << outcome.generation
                   << L".\n";
        return 0;
    }
    if (outcome.error.has_value()) print_error(*outcome.error);
    else std::wcerr << L"Error: compact failed.\n";
    return 1;
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    std::vector<std::wstring_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

    const auto parsed = parse_cli(arguments);
    if (parsed.action == CliAction::Help) {
        std::wcout << kUsage;
        return 0;
    }
    if (parsed.action == CliAction::Error) {
        std::wcerr << L"Error: " << parsed.message << L"\n" << kUsage;
        return 2;
    }
    if (parsed.action == CliAction::Compact) return compact_product(*parsed.compact);
    return run_product(*parsed.run);
}
