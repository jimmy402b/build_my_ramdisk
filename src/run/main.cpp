#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "src/run/cli.h"
#include "src/run/disk_runtime.h"
#include "src/run/driver_preflight.h"
#include "src/run/m4_persistence_adapter.h"
#include "src/run/run_loop.h"
#include "src/run/windows_volume.h"

#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/production_policy.h"

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

Result<void> require_elevated_administrator() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Result<void>::failure(product_error(
            ErrorDomain::win32, GetLastError(), L"require_elevation",
            L"OpenProcessToken failed", GetLastError()));
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL elevation_ok = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD elevation_error = GetLastError();
    CloseHandle(token);
    if (!elevation_ok || returned != sizeof(elevation) ||
        !elevation.TokenIsElevated) {
        return Result<void>::failure(product_error(
            ErrorDomain::usage, 1, L"require_elevation",
            L"run requires an elevated administrator token", elevation_error));
    }
    return Result<void>::success();
}

Result<DiskBundleKind> classify_bundle(const std::filesystem::path& bundle) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(bundle, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<DiskBundleKind>::failure(product_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"bundle status could not be read"));
    }
    if (!std::filesystem::exists(status)) {
        return Result<DiskBundleKind>::success(DiskBundleKind::empty);
    }
    if (!std::filesystem::is_directory(status)) {
        return Result<DiskBundleKind>::failure(product_error(
            ErrorDomain::usage, 1, L"classify_bundle",
            L"bundle is not a directory"));
    }

    error.clear();
    const auto manifest = std::filesystem::symlink_status(bundle / L"disk.rdm", error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<DiskBundleKind>::failure(product_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"manifest status could not be read"));
    }
    if (!error && std::filesystem::is_regular_file(manifest)) {
        return Result<DiskBundleKind>::success(DiskBundleKind::manifest);
    }

    error.clear();
    std::filesystem::directory_iterator iterator(bundle, error);
    if (error) {
        return Result<DiskBundleKind>::failure(product_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"bundle contents could not be read"));
    }
    return iterator == std::filesystem::directory_iterator{}
        ? Result<DiskBundleKind>::success(DiskBundleKind::empty)
        : Result<DiskBundleKind>::success(DiskBundleKind::unknown_nonempty);
}

Result<void> ensure_bundle_directory(const std::filesystem::path& bundle) {
    std::error_code error;
    if (!std::filesystem::create_directories(bundle, error) && error) {
        return Result<void>::failure(product_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"create_bundle_directory", L"bundle directory could not be created"));
    }
    return Result<void>::success();
}

DynamicBlockStoreOptions store_options_for(const RunOptions& options) {
    // The parser guarantees at least 64 MiB and power-of-two byte units, so
    // these limits remain block-aligned and satisfy the M3 strict ordering.
    auto result = m3::production_store_options();
    result.hard_limit_bytes = options.ram_limit_bytes;
    result.soft_limit_bytes = options.ram_limit_bytes / 2;
    result.eviction_low_watermark_bytes = options.ram_limit_bytes / 4;
    return result;
}

DiskCheckpointDisposition map_disposition(
    const persistence::CommitDisposition disposition) noexcept {
    switch (disposition) {
    case persistence::CommitDisposition::Committed:
        return DiskCheckpointDisposition::committed;
    case persistence::CommitDisposition::CommitIndeterminate:
    case persistence::CommitDisposition::CommittedNeedsReconciliation:
        return DiskCheckpointDisposition::commit_indeterminate;
    case persistence::CommitDisposition::NotCommitted:
        return DiskCheckpointDisposition::not_committed;
    }
    return DiskCheckpointDisposition::commit_indeterminate;
}

struct ForegroundRuntime final {
    explicit ForegroundRuntime(RunOptions value)
        : options(std::move(value)),
          persistence({options.bundle, options.virtual_bytes}),
          owner_token(L"myrd:" + std::to_wstring(GetCurrentProcessId())) {}

    RunOptions options;
    OperationContext context{};
    M4PersistenceAdapter persistence;
    Win32VolumeAdapter volume_adapter;
    std::unique_ptr<m3::M3AimRuntime> aim_runtime;
    std::wstring owner_token;

    Result<void> create_image(const std::uint64_t expected_bytes) {
        if (expected_bytes != options.virtual_bytes) {
            return Result<void>::failure(product_error(
                ErrorDomain::usage, 1, L"create_generation_zero_image",
                L"requested image size changed during startup"));
        }
        const auto directory = ensure_bundle_directory(options.bundle);
        if (!directory.ok()) return directory;
        const auto created = persistence.create_generation_zero(context);
        if (!created.ok()) return Result<void>::failure(created.error());
        return Result<void>::success();
    }

    Result<void> recover_image(const std::uint64_t expected_bytes) {
        if (expected_bytes != options.virtual_bytes) {
            return Result<void>::failure(product_error(
                ErrorDomain::usage, 1, L"recover_complete_image",
                L"requested image size changed during startup"));
        }
        const auto recovered = persistence.recover_complete(context);
        if (!recovered.ok()) return Result<void>::failure(recovered.error());
        if (recovered.value().checkpoint_generation == 0) {
            return Result<void>::failure(product_error(
                ErrorDomain::persistence, 1, L"recover_complete_image",
                L"generation-zero bundle has no durable Windows volume checkpoint"));
        }
        return Result<void>::success();
    }

    Result<void> start_aim() {
        m3::M3AimProviderFactoryConfig factory{};
        factory.context = context;
        factory.provider.bundle_root = options.bundle;
        factory.provider.manifest_name = "disk.rdm";
        factory.provider.recovery.limits =
            persistence::production_persistence_limits(options.virtual_bytes);
        factory.provider.store = store_options_for(options);
        factory.provider.request_timeout = std::chrono::seconds(30);

        auto created = m3::M3AimRuntime::create(std::move(factory));
        if (!created.ok()) return Result<void>::failure(created.error());
        aim_runtime = created.move_value();
        const auto started = aim_runtime->start();
        if (!started.ok()) {
            (void)aim_runtime->stop();
            aim_runtime.reset();
            return started;
        }
        if (aim_runtime->info().virtual_bytes != options.virtual_bytes) {
            (void)aim_runtime->stop();
            aim_runtime.reset();
            return Result<void>::failure(product_error(
                ErrorDomain::persistence, 1, L"start_aim",
                L"M3 runtime geometry does not match requested size"));
        }
        return Result<void>::success();
    }

    Result<void> stop_aim() {
        if (!aim_runtime) return Result<void>::success();
        const auto stopped = aim_runtime->stop();
        if (stopped.ok()) aim_runtime.reset();
        return stopped;
    }

    Result<void> verify_volume(const ProductVolume& volume,
                               const std::uint64_t expected_bytes) const {
        if (!aim_runtime || expected_bytes != options.virtual_bytes) {
            return Result<void>::failure(product_error(
                ErrorDomain::internal, 1, L"verify_volume",
                L"M3 runtime is unavailable or geometry changed"));
        }
        const auto observed = volume_adapter.observe(volume);
        if (!observed.ok()) return Result<void>::failure(observed.error());
        const auto& actual = observed.value().volume;
        auto normalized_guid = [](std::wstring value) {
            while (!value.empty() && value.back() == L'\\') value.pop_back();
            return value;
        };
        const auto actual_guid = normalized_guid(actual.volume_guid);
        const auto expected_guid = normalized_guid(volume.volume_guid);
        const auto extent_bytes = actual.extents.empty()
            ? 0ULL : actual.extents.front().length_bytes;
        constexpr std::uint64_t kPartitionOverheadAllowance = 2ULL * 1024ULL * 1024ULL;
        if (!actual.operation_owned ||
            actual.extents.size() != 1 ||
            extent_bytes == 0 || extent_bytes > expected_bytes ||
            expected_bytes - extent_bytes > kPartitionOverheadAllowance ||
            actual.extents.front().disk_number !=
                aim_runtime->info().disk.physical_drive_number ||
            _wcsicmp(actual_guid.c_str(), expected_guid.c_str()) != 0 ||
            actual.extents != volume.extents) {
            std::wcerr << L"Diagnostic verify_volume: volume_guid="
                       << actual.volume_guid << L" aim_disk="
                       << aim_runtime->info().disk.physical_drive_number
                       << L" actual_disk="
                       << (actual.extents.empty() ? 0xFFFFFFFFu :
                           actual.extents.front().disk_number)
                       << L" expected_disk="
                       << (volume.extents.empty() ? 0xFFFFFFFFu :
                           volume.extents.front().disk_number) << L"\n";
            return Result<void>::failure(product_error(
                ErrorDomain::evidence, 1, L"verify_volume",
                L"Windows volume identity does not match the AIM disk"));
        }
        return Result<void>::success();
    }

    Result<DiskCheckpointResult> checkpoint() {
        if (!aim_runtime) {
            return Result<DiskCheckpointResult>::failure(product_error(
                ErrorDomain::internal, 1, L"checkpoint", L"M3 runtime is unavailable"));
        }
        const auto outcome = aim_runtime->checkpoint(
            context, persistence::VolumeBarrierKind::WindowsVolumeFlushed);
        return Result<DiskCheckpointResult>::success({
            map_disposition(outcome.disposition), outcome.generation});
    }

    DiskMaintenanceStats maintenance_stats() const {
        if (!aim_runtime) return {};
        const auto stats = aim_runtime->storage_stats();
        // `reserved_bytes` is RAM committed by clean as well as dirty blocks;
        // checkpoint scheduling is driven only by mutations that have not yet
        // reached the M3 journal.
        constexpr std::uint64_t kBlockBytes = 2ULL * 1024ULL * 1024ULL;
        return {(stats.resident_dirty_blocks + stats.zero_dirty_blocks) *
                    kBlockBytes,
                0, stats.backing_generation};
    }
};

DiskRuntimeOperations make_operations(ForegroundRuntime& foreground) {
    DiskRuntimeOperations operations{};
    operations.preflight = [] {
        const auto elevated = require_elevated_administrator();
        if (!elevated.ok()) return elevated;
        return require_phdskmnt_service_available();
    };
    operations.classify_bundle = [](const std::filesystem::path& path) {
        return classify_bundle(path);
    };
    // M3 holds the authoritative BundleWriterLock.  Generation-zero creation
    // and M3AimRuntime::start each acquire it around their own mutation/open
    // window; a second foreground lock would self-deadlock those operations.
    operations.acquire_bundle_lock = [](const std::filesystem::path&) {
        return Result<void>::success();
    };
    operations.release_bundle_lock = [] { return Result<void>::success(); };
    operations.create_generation_zero_image = [&foreground](const std::uint64_t bytes) {
        return foreground.create_image(bytes);
    };
    operations.recover_complete_image = [&foreground](const std::uint64_t bytes) {
        return foreground.recover_image(bytes);
    };
    operations.start_aim = [&foreground] { return foreground.start_aim(); };
    operations.stop_aim = [&foreground] { return foreground.stop_aim(); };
    operations.provision_new_volume = [&foreground](const std::uint64_t,
                                                      const wchar_t letter) {
        if (!foreground.aim_runtime) {
            return Result<ProductVolume>::failure(product_error(
                ErrorDomain::internal, 1, L"provision_new_volume",
                L"M3 runtime is unavailable"));
        }
        return foreground.volume_adapter.provision_and_adopt_volume(
            foreground.aim_runtime->info().disk.physical_drive_number, letter,
            foreground.owner_token, 120000);
    };
    operations.find_recovered_volume = [&foreground](const wchar_t letter) {
        if (!foreground.aim_runtime) {
            return Result<ProductVolume>::failure(product_error(
                ErrorDomain::internal, 1, L"find_recovered_volume",
                L"M3 runtime is unavailable"));
        }
        return foreground.volume_adapter.find_recovered_volume(
            foreground.aim_runtime->info().disk.physical_drive_number, letter,
            foreground.owner_token);
    };
    operations.verify_volume = [&foreground](const ProductVolume& volume,
                                               const std::uint64_t bytes) {
        return foreground.verify_volume(volume, bytes);
    };
    operations.flush_volume = [&foreground](const ProductVolume& volume) {
        return foreground.volume_adapter.flush(volume);
    };
    operations.assign_drive_letter = [&foreground](const ProductVolume& volume,
                                                     const wchar_t letter) {
        return foreground.volume_adapter.assign_drive_letter(volume, letter);
    };
    operations.dismount_volume = [&foreground](const ProductVolume& volume) {
        auto locked = foreground.volume_adapter.lock(volume, 30000);
        if (!locked.ok()) return Result<void>::failure(locked.error());
        if (locked.value().busy || !locked.value().locked.has_value()) {
            return Result<void>::failure(product_error(
                ErrorDomain::cleanup, 1, L"dismount_volume",
                L"volume is busy; close applications using the RAM disk and retry"));
        }
        return locked.value().locked->dismount();
    };
    operations.remove_drive_letter = [&foreground](const ProductVolume& volume) {
        return foreground.volume_adapter.remove_drive_letter(volume);
    };
    operations.checkpoint = [&foreground] { return foreground.checkpoint(); };
    operations.maintenance_stats = [&foreground] {
        return foreground.maintenance_stats();
    };
    return operations;
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

    ForegroundRuntime foreground(options);
    DiskRuntimeOptions runtime_options{options.bundle, options.virtual_bytes,
                                       options.drive_letter,
                                       std::chrono::seconds(30)};
    auto runtime = bundle_kind.value() == DiskBundleKind::empty
        ? DiskRuntime::create(runtime_options, make_operations(foreground))
        : DiskRuntime::recover(runtime_options, make_operations(foreground));
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
