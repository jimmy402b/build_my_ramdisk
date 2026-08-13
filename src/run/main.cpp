#include "cli.h"
#include "src/m3/m3_lifecycle_runner.h"
#include "src/m3/m3_windows_acceptance.h"
#include "windows_volume.h"
#include "run_loop.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/vhdx_reader.h"
#include <bcrypt.h>
#include <windows.h>
#include <atomic>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {
enum class BundleMode { Create, Recover };
std::atomic_bool g_shutdown_backing{false};
std::atomic_bool* g_shutdown_requested{&g_shutdown_backing};
BOOL WINAPI console_shutdown_handler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
        g_shutdown_requested->store(true, std::memory_order_relaxed);
        return TRUE;
    default: return FALSE;
    }
}

myrd::Result<void> require_elevation() {
    HANDLE token{}; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&token)) return myrd::Result<void>::failure({myrd::ErrorDomain::win32,GetLastError(),L"require_elevation",L"OpenProcessToken failed"});
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY; PSID admin{};
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&admin)){DWORD error=GetLastError();CloseHandle(token);return myrd::Result<void>::failure({myrd::ErrorDomain::win32,error,L"require_elevation",L"administrator SID creation failed"});}
    TOKEN_ELEVATION elevation{}; DWORD returned{}; BOOL ok=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&returned); DWORD error=GetLastError(); HANDLE impersonation{}; BOOL duplicated=DuplicateToken(token,SecurityIdentification,&impersonation); DWORD duplicate_error=GetLastError(); BOOL member{}; BOOL member_ok=duplicated&&CheckTokenMembership(impersonation,admin,&member); DWORD member_error=GetLastError(); if(impersonation)CloseHandle(impersonation); FreeSid(admin); CloseHandle(token);
    if(!duplicated)return myrd::Result<void>::failure({myrd::ErrorDomain::win32,duplicate_error,L"require_elevation",L"DuplicateToken failed"});
    if(!ok || returned<sizeof(elevation) || !member_ok || !elevation.TokenIsElevated || !member) return myrd::Result<void>::failure({myrd::ErrorDomain::usage,!member_ok?member_error:error,L"require_elevation",L"run requires an elevated administrator token"});
    return myrd::Result<void>::success();
}

myrd::Result<BundleMode> classify_bundle(const std::filesystem::path& root) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(root, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return myrd::Result<BundleMode>::failure({myrd::ErrorDomain::storage,
            static_cast<std::uint32_t>(ec.value()), L"classify_bundle",
            L"bundle status could not be read"});
    }
    if (!std::filesystem::exists(status)) return myrd::Result<BundleMode>::success(BundleMode::Create);
    if (!std::filesystem::is_directory(status)) return myrd::Result<BundleMode>::failure({
        myrd::ErrorDomain::usage, 1, L"classify_bundle", L"bundle is not a directory"});
    const auto manifest_status = std::filesystem::symlink_status(
        root / L"disk.rdm", ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return myrd::Result<BundleMode>::failure({myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(ec.value()), L"classify_bundle",
        L"manifest status could not be read"});
    if (!ec && std::filesystem::is_regular_file(manifest_status))
        return myrd::Result<BundleMode>::success(BundleMode::Recover);
    ec.clear();
    auto iterator = std::filesystem::directory_iterator(root, ec);
    if (ec) return myrd::Result<BundleMode>::failure({myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(ec.value()), L"classify_bundle",
        L"bundle contents could not be read"});
    if (iterator == std::filesystem::directory_iterator{})
        return myrd::Result<BundleMode>::success(BundleMode::Create);
    return myrd::Result<BundleMode>::failure({myrd::ErrorDomain::usage, 1,
        L"classify_bundle", L"non-empty bundle has no disk.rdm"});
}

myrd::Result<myrd::persistence::RecoveredImage> create_image(
    const myrd::OperationContext& context, const myrd::RunOptions& options) {
    using namespace myrd::persistence;
    std::error_code directory_error;
    if (!std::filesystem::create_directories(options.bundle, directory_error) &&
        directory_error) {
        return myrd::Result<RecoveredImage>::failure({myrd::ErrorDomain::storage,
            static_cast<std::uint32_t>(directory_error.value()),
            L"create_product_image", L"bundle directory could not be created"});
    }
    GenerationZeroCreateRequest request{};
    request.bundle_root = options.bundle;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base-0000.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {options.virtual_bytes, 512, 4096};
    NTSTATUS random_status{};
    auto random = [&random_status](auto& bytes) {
        random_status = BCryptGenRandom(nullptr,
            reinterpret_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return random_status >= 0;
    };
    if (!random(request.image_uuid.bytes) ||
        !random(request.base_identity.file_identity.file_write_guid) ||
        !random(request.base_identity.file_identity.data_write_guid) ||
        !random(request.base_identity.virtual_disk_id) ||
        !random(request.journal_identity.journal_uuid)) {
        return myrd::Result<RecoveredImage>::failure({myrd::ErrorDomain::ntstatus,
            static_cast<std::uint32_t>(random_status), L"create_product_image",
            L"BCryptGenRandom failed"});
    }
    request.journal_epoch = 1;
    request.operation_id = 1;
    request.created_time_100ns = 1;
    const auto payload_base = options.virtual_bytes / kVhdxDataBlockBytes;
    if (payload_base == (std::numeric_limits<std::uint64_t>::max)()) {
        return myrd::Result<RecoveredImage>::failure({myrd::ErrorDomain::usage,
            1, L"create_product_image", L"VHDX payload count overflow"});
    }
    const auto payloads = payload_base + 1;
    request.vhdx_limits = {payloads, payloads, kVhdxDataBlockBytes};
    request.recovery = {myrd::m3::production_persistence_limits(options.virtual_bytes)};
    auto created = create_generation_zero_image(context, request);
    if (!created.ok()) return created;
    if (created.value().checkpoint_generation != 0) {
        return myrd::Result<RecoveredImage>::failure({
            myrd::ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"create_product_image", L"created bundle did not reopen at generation zero"});
    }
    return created;
}

void print_run_error(const std::filesystem::path& bundle,
                     const myrd::Error& error) {
    std::wcerr << L"Error: run failed for " << bundle.wstring() << L": "
               << error.operation << L": " << error.message << L"\n";
}
}

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
    if (parsed.action == myrd::CliAction::Compact) {
        const auto root = parsed.compact->bundle;
        const auto outcome = myrd::m3::run_product_offline_compaction(
            {{}, std::chrono::steady_clock::now() + std::chrono::minutes(5)}, root);
        if (outcome.exit_code == 0) {
            std::wcout << L"Compacted bundle to generation "
                       << outcome.generation << L".\n";
            return 0;
        }
        if (outcome.error.has_value() &&
            outcome.error->domain == myrd::ErrorDomain::persistence &&
            outcome.error->code == static_cast<std::uint32_t>(
                myrd::persistence::PersistenceErrorCode::ImageInUse)) {
            std::wcerr << L"Error: bundle is in use. Stop myrd run for this bundle, then retry: myrd compact --bundle \""
                       << root.wstring() << L"\"\n";
            return 1;
        }
        std::wcerr << L"Error: compact failed for " << root.wstring();
        if (outcome.error.has_value()) {
            std::wcerr << L": " << outcome.error->operation << L": "
                       << outcome.error->message;
        }
        std::wcerr << L"\n";
        return 1;
    }
    const auto& options = *parsed.run;
    const auto elevated=require_elevation();
    if(!elevated.ok()){print_run_error(options.bundle,elevated.error());return 1;}
    const auto context = myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
    const auto mode = classify_bundle(options.bundle);
    if (!mode.ok()) {
        print_run_error(options.bundle, mode.error());
        return 1;
    }
    const bool recover = mode.value() == BundleMode::Recover;
    SetLastError(ERROR_SUCCESS); const DWORD drive_mask=GetLogicalDrives(); const DWORD drive_error=GetLastError();
    const bool drive_query_ok=drive_mask!=0 || drive_error==ERROR_SUCCESS;
    if (!recover && (!drive_query_ok || drive_mask &
        (1u << (options.drive_letter - L'A')))) {
        print_run_error(options.bundle, {myrd::ErrorDomain::usage, 1,
            L"preflight_drive_letter", !drive_query_ok ? L"GetLogicalDrives failed" : L"requested drive letter is occupied"});
        return 1;
    }
    auto image = recover
        ? myrd::persistence::recover_complete_image(
            context, options.bundle, "disk.rdm",
            {myrd::m3::production_persistence_limits(options.virtual_bytes)})
        : myrd::persistence::RecoveryOutcome{};
    std::optional<myrd::persistence::RecoveredImage> selected;
    std::optional<myrd::Error> selection_error;
    if (recover && image.ok()) selected = *image.image;
    else if (!recover) {
        auto created = create_image(context, options);
        if (created.ok()) selected = created.move_value();
        else selection_error = created.error();
    }
    if (recover && selected && selected->checkpoint_generation == 0) {
        std::wcerr << L"Error: incomplete generation-zero bundle at "
                   << options.bundle.wstring()
                   << L". Verify the bundle path, manually remove the incomplete bundle, then run the original myrd run command again.\n";
        return 1;
    }
    if (!selected) {
        if (recover && image.error.has_value()) {
            print_run_error(options.bundle, *image.error);
        } else if (selection_error.has_value()) {
            print_run_error(options.bundle, *selection_error);
        } else {
            print_run_error(options.bundle, {myrd::ErrorDomain::persistence,
                static_cast<std::uint32_t>(myrd::persistence::PersistenceErrorCode::VerificationFailed),
                L"create_product_image", L"generation-zero bundle creation failed"});
        }
        return 1;
    }
    if (recover && selected->geometry.virtual_bytes != options.virtual_bytes) {
        print_run_error(options.bundle, {myrd::ErrorDomain::usage, 1,
            L"validate_recovered_image", L"requested size does not match recovered bundle geometry"});
        return 1;
    }
    myrd::m3::M3AimProviderFactoryConfig factory{};
    factory.context = context;
    factory.provider.bundle_root = options.bundle;
    factory.provider.manifest_name = "disk.rdm";
    factory.provider.recovery = {
        myrd::m3::production_persistence_limits(options.virtual_bytes)};
    factory.provider.store = myrd::m3::production_store_options(
        options.ram_limit_bytes);
    factory.provider.request_timeout = std::chrono::seconds(30);
    factory.provider.journal_committed_end_ceiling = 2147483648ull;
    auto runtime = myrd::m3::M3AimRuntime::create(std::move(factory));
    if (!runtime.ok()) {
        print_run_error(options.bundle, runtime.error());
        return 1;
    }
    const auto started = runtime.value()->start();
    if (!started.ok()) {
        const auto cleanup = runtime.value()->stop();
        print_run_error(options.bundle, started.error());
        if (!cleanup.ok()) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    const auto stats = runtime.value()->maintenance_stats();
    if (!stats.ok()) {
        const auto cleanup = runtime.value()->stop();
        print_run_error(options.bundle, stats.error());
        if (!cleanup.ok()) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    if (runtime.value()->info().virtual_bytes != options.virtual_bytes ||
        (recover && stats.value().generation == 0)) {
        const auto cleanup = runtime.value()->stop();
        print_run_error(options.bundle, {myrd::ErrorDomain::persistence,
            static_cast<std::uint32_t>(myrd::persistence::PersistenceErrorCode::VerificationFailed),
            L"validate_started_runtime", L"runtime size or durable generation did not match the recovered bundle"});
        if (!cleanup.ok()) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    auto volume = recover
        ? myrd::find_recovered_volume(runtime.value()->info(), options.drive_letter,
                                      std::chrono::seconds(120))
        : myrd::provision_new_volume(runtime.value()->info(), options.drive_letter,
                                     std::chrono::seconds(120));
    auto stop_with_volume_cleanup = [&](const myrd::ProductVolume& held) {
        const auto removed = myrd::remove_drive_letter(held);
        const auto stopped = runtime.value()->stop();
        if (!removed.ok()) print_run_error(options.bundle, removed.error());
        if (!stopped.ok()) std::wcerr << L"Runtime cleanup failed\n";
        return removed.ok() && stopped.ok();
    };
    if (!volume.ok()) {
        const auto cleanup = runtime.value()->stop();
        print_run_error(options.bundle, volume.error());
        if (!cleanup.ok()) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    if (recover) {
        auto assigned = myrd::assign_drive_letter(volume.value());
        if (!assigned.ok()) {
            const auto cleanup = stop_with_volume_cleanup(volume.value());
            print_run_error(options.bundle, assigned.error());
            if (!cleanup) std::wcerr << L"Runtime cleanup failed\n";
            return 1;
        }
    } else {
        auto flushed = myrd::flush_volume(volume.value());
        if (!flushed.ok()) {
            const auto cleanup = stop_with_volume_cleanup(volume.value());
            print_run_error(options.bundle, flushed.error());
            if (!cleanup) std::wcerr << L"Runtime cleanup failed\n";
            return 1;
        }
        const auto checkpoint = runtime.value()->checkpoint(context,
            myrd::persistence::VolumeBarrierKind::WindowsVolumeFlushed);
        if (!checkpoint.ok() || checkpoint.generation != 1) {
            const auto cleanup = stop_with_volume_cleanup(volume.value());
            print_run_error(options.bundle, checkpoint.error.value_or(myrd::Error{
                myrd::ErrorDomain::persistence, 1, L"first_volume_checkpoint",
                L"first checkpoint did not commit generation one"}));
            if (!cleanup) std::wcerr << L"Runtime cleanup failed\n";
            return 1;
        }
    }
    const auto ready_stats = runtime.value()->maintenance_stats();
    if (!ready_stats.ok()) {
        const auto cleanup = stop_with_volume_cleanup(volume.value());
        print_run_error(options.bundle, ready_stats.error());
        if (!cleanup) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    std::wcout << L"Ready: " << volume.value().drive_letter << L": generation "
               << ready_stats.value().generation << L".\n" << std::flush;
    g_shutdown_backing.store(false, std::memory_order_relaxed);
    g_shutdown_requested = &g_shutdown_backing;
    if (!SetConsoleCtrlHandler(console_shutdown_handler, TRUE)) {
        const DWORD handler_error = GetLastError();
        const auto cleanup = stop_with_volume_cleanup(volume.value());
        print_run_error(options.bundle, {myrd::ErrorDomain::win32, handler_error,
            L"SetConsoleCtrlHandler", L"console handler registration failed"});
        if (!cleanup) std::wcerr << L"Runtime cleanup failed\n";
        return 1;
    }
    const int loop_exit = myrd::run_loop(*runtime.value(), volume.value(), options,
                                         *g_shutdown_requested);
    if (!SetConsoleCtrlHandler(console_shutdown_handler, FALSE)) {
        std::wcerr << L"Error: SetConsoleCtrlHandler unregister failed\n";
        return 1;
    }
    return loop_exit;
}
