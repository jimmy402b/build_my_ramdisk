#include "cli.h"
#include "lifecycle_runner.h"
#include "m0_provider.h"
#include "poc_report.h"
#include "report_publisher.h"
#include "src/common/authenticode.h"
#include "src/common/process_snapshot.h"
#include "src/common/sha256.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/driver/windows_srb_transport.h"
#include "src/poc/windows_raw_disk_io.h"
#include "src/proxy/object_names.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <securitybaseapi.h>
#include <winsvc.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace {

std::string w2s(std::wstring_view ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
        static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(),
        static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

bool publish_report(const std::string& json, const std::string& path) {
    auto published = myrd::poc::publish_report_atomic(
        json, std::filesystem::path(path));
    if (!published) {
        std::fprintf(stderr,
            "ERROR: report publication failed for %s (code=%u)\n",
            path.c_str(), published.error().code);
        return false;
    }
    std::printf("Report written to %s\n", path.c_str());
    return true;
}

std::string current_utc() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool equals_ci(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (towlower(left[i]) != towlower(right[i])) return false;
    }
    return true;
}

bool current_user_is_admin() {
    BOOL is_admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;
    if (AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    return is_admin != FALSE;
}

std::wstring current_executable_path() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::wstring(buffer.data(), length);
}

std::string sha256_file(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input) return {};
    }
    auto digest = myrd::sha256_hex(bytes);
    return digest ? digest.value() : std::string{};
}

std::string file_version(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(bytes.data(), L"\\",
            reinterpret_cast<void**>(&info), &info_size) ||
        info == nullptr || info_size < sizeof(*info)) {
        return {};
    }
    std::ostringstream out;
    out << HIWORD(info->dwFileVersionMS) << '.'
        << LOWORD(info->dwFileVersionMS) << '.'
        << HIWORD(info->dwFileVersionLS) << '.'
        << LOWORD(info->dwFileVersionLS);
    return out.str();
}

struct ServiceSnapshot {
    std::string state;
    std::wstring binary_path;
};

std::wstring normalize_service_binary_path(std::wstring path) {
    if (path.empty()) return {};
    if (path.front() == L'"') {
        const auto closing = path.find(L'"', 1);
        path = closing == std::wstring::npos
            ? path.substr(1)
            : path.substr(1, closing - 1);
    } else {
        const auto space = path.find(L' ');
        if (space != std::wstring::npos) path.resize(space);
    }

    constexpr std::wstring_view system_root_prefix = L"\\SystemRoot\\";
    if (path.size() >= system_root_prefix.size() &&
        equals_ci(std::wstring_view(path).substr(
                      0, system_root_prefix.size()),
                  system_root_prefix)) {
        wchar_t windows_dir[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            path = std::wstring(windows_dir, length) + L"\\" +
                   path.substr(system_root_prefix.size());
        }
    }

    const DWORD expanded_size = ExpandEnvironmentStringsW(
        path.c_str(), nullptr, 0);
    if (expanded_size > 0) {
        std::vector<wchar_t> expanded(expanded_size);
        if (ExpandEnvironmentStringsW(
                path.c_str(), expanded.data(), expanded_size) != 0) {
            path.assign(expanded.data());
        }
    }
    return path;
}

ServiceSnapshot query_service(std::wstring_view name) {
    ServiceSnapshot snapshot;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return snapshot;
    const std::wstring service_name(name);
    SC_HANDLE service = OpenServiceW(
        manager, service_name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        return snapshot;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
        switch (status.dwCurrentState) {
        case SERVICE_RUNNING: snapshot.state = "Running"; break;
        case SERVICE_STOPPED: snapshot.state = "Stopped"; break;
        case SERVICE_START_PENDING: snapshot.state = "StartPending"; break;
        case SERVICE_STOP_PENDING: snapshot.state = "StopPending"; break;
        default: snapshot.state = std::to_string(status.dwCurrentState); break;
        }
    }

    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed > 0) {
        std::vector<std::byte> config_bytes(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(
            config_bytes.data());
        if (QueryServiceConfigW(service, config, needed, &needed) &&
            config->lpBinaryPathName != nullptr) {
            snapshot.binary_path = normalize_service_binary_path(
                config->lpBinaryPathName);
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return snapshot;
}

void populate_runtime_evidence(
    myrd::poc::PocReport& report, bool elevated, bool run_mode) {
    report.environment.elevated = elevated;
    report.environment.architecture = sizeof(void*) == 8 ? "x64" : "x86";
    const auto executable = current_executable_path();
    report.environment.executable_path = w2s(executable);
    report.environment.executable_sha256 = sha256_file(executable);

    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtl_get_version != nullptr) {
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtl_get_version(&version) == 0) {
            report.environment.windows_version =
                std::to_string(version.dwMajorVersion) + "." +
                std::to_string(version.dwMinorVersion);
            report.environment.windows_build =
                std::to_string(version.dwBuildNumber);
        }
    }

    constexpr std::array<std::wstring_view, 1> forbidden_modules = {L"aimapi.dll"};
    auto modules = myrd::snapshot_current_modules();
    if (modules) {
        for (const auto& module : modules.value()) {
            report.dependency_audit.loaded_modules.push_back(
                w2s(module.path));
            for (const auto forbidden : forbidden_modules) {
                if (equals_ci(module.basename, forbidden)) {
                    report.dependency_audit.forbidden_loaded_modules.push_back(
                        w2s(module.path));
                }
            }
        }
    }

    constexpr std::array<std::wstring_view, 8> forbidden_processes = {L"aim_ll.exe", L"RamDyn.exe", L"AIMtk.exe", L"AIMtk-Dlg.exe", L"AIMtk-svc.exe", L"aim_cli.exe", L"MountImg.exe", L"RamDiskUI.exe"};
    auto processes = myrd::snapshot_processes();
    if (processes) {
        for (const auto& process : processes.value()) {
            bool forbidden = false;
            for (const auto name : forbidden_processes) {
                if (equals_ci(process.name, name)) {
                    forbidden = true;
                    break;
                }
            }
            if (run_mode && equals_ci(process.name, L"deviosvc.exe")) {
                forbidden = true;
            }
            if (forbidden) {
                report.dependency_audit.forbidden_processes.push_back(
                    w2s(process.name));
            }
        }
    }
    report.dependency_audit.deviosvc_state_during_run =
        query_service(L"deviosvc").state;
}

void populate_driver_evidence(myrd::poc::PocReport& report) {
    report.driver.service_name = "phdskmnt";
    const auto service = query_service(L"phdskmnt");
    report.driver.service_state = service.state;
    report.driver.file_path = w2s(service.binary_path);
    report.driver.file_version = file_version(service.binary_path);
    report.driver.sha256 = sha256_file(service.binary_path);
    auto signature = myrd::verify_authenticode(service.binary_path);
    if (signature) {
        std::ostringstream status;
        status << "0x" << std::hex << std::uppercase
               << signature.value().trust_status;
        report.driver.authenticode_status = status.str();
        report.driver.signer_subject =
            w2s(signature.value().signer_subject);
    }
}

} // anonymous namespace

int wmain(int argc, wchar_t* argv[]) {
    myrd::poc::PocReport report;
    std::string requested_report_path;

    try {
        report.started_utc = current_utc();

        // Parse arguments
        std::vector<std::wstring_view> av;
        for (int i = 1; i < argc; ++i)
            av.push_back(std::wstring_view(argv[i]));

#ifdef _DEBUG
        bool debug = true;
#else
        bool debug = false;
#endif

        auto parsed = myrd::poc::parse_command_line(av, debug);
        if (!parsed) {
            report.success = false;
            report.exit_code = 2;
            report.last_stage = "parse_error";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            std::fprintf(stderr, "Usage: myrd-m0-poc query-driver --report <path>\n");
            std::fprintf(stderr, "       myrd-m0-poc run --report <path>\n");
            return 2;
        }

        auto cmd = parsed.value();
        requested_report_path = cmd.report_path.string();
        report.mode = (cmd.mode == myrd::poc::CommandMode::query_driver)
                          ? "query-driver" : "run";
        auto report_id = myrd::proxy::generate_session_id();
        if (!report_id) {
            report.success = false;
            report.exit_code = 12;
            report.last_stage = "report_written";
            report.finished_utc = current_utc();
            if (!publish_report(myrd::poc::serialize_report(report),
                    cmd.report_path.string())) return 11;
            return report.exit_code;
        }
        report.run_id = w2s(report_id.value());
        const bool elevated = current_user_is_admin();
        populate_runtime_evidence(
            report, elevated,
            cmd.mode == myrd::poc::CommandMode::run);
        populate_driver_evidence(report);

        auto finish_with_error = [&](int exit_code,
                                     const myrd::Error& error) {
            report.success = false;
            report.exit_code = exit_code;
            report.primary_error = myrd::poc::ErrorEvidence{
                std::to_string(static_cast<int>(error.domain)),
                error.code,
                w2s(error.operation),
                w2s(error.message)};
            report.last_stage = "report_written";
            report.finished_utc = current_utc();
            if (!publish_report(myrd::poc::serialize_report(report),
                    cmd.report_path.string())) {
                return 11;
            }
            return exit_code;
        };

        // ── query-driver mode ─────────────────────────────────
        if (cmd.mode == myrd::poc::CommandMode::query_driver) {
            report.last_stage = "query_started";

            auto adapter = myrd::aim::discover_aim_adapter();
            if (!adapter) {
                return finish_with_error(5, adapter.error());
            }

            report.driver.adapter_interface_path =
                w2s(adapter.value().interface_path);

            myrd::aim::WindowsSrbTransport transport(
                adapter.value().handle.get());
            myrd::aim::DirectAimAdapter aim(transport);

            auto ver = aim.query_version();
            if (!ver) return finish_with_error(5, ver.error());
            report.driver.query_version_raw =
                std::to_string(ver.value().api_version) +
                " (sub_version=0x" +
                [](std::uint32_t sv) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%08X", sv);
                    return std::string(buf);
                }(ver.value().sub_version) + ")";

            auto devices = aim.list_devices();
            if (!devices) return finish_with_error(5, devices.error());
            for (auto& d : devices.value())
                report.baseline.aim_device_ids_before.push_back(
                    std::to_string(d.value));

            auto pd = myrd::aim::snapshot_physical_drive_numbers();
            if (!pd) return finish_with_error(5, pd.error());
            for (auto n : pd.value())
                report.baseline.physical_drive_numbers_before.push_back(
                    std::to_string(n));

            // Post-snapshot (same as before for query mode)
            report.baseline.aim_device_ids_after =
                report.baseline.aim_device_ids_before;
            report.baseline.physical_drive_numbers_after =
                report.baseline.physical_drive_numbers_before;

            report.success = true;
            report.exit_code = 0;
            report.last_stage = "report_written";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            if (!publish_report(json, cmd.report_path.string())) return 11;
            return 0;
        }

        // ── run mode ──────────────────────────────────────────
        // Check admin elevation
        if (!elevated) {
            report.exit_code = 3;
            report.success = false;
            report.last_stage = "preflight_failed";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            if (!publish_report(json, cmd.report_path.string())) return 11;
            return 3;
        }

        // Global serial mutex
        myrd::UniqueHandle run_mutex(
            CreateMutexW(nullptr, FALSE, L"Global\\myrd-m0-run-mutex"));
        if (!run_mutex ||
            WaitForSingleObject(run_mutex.get(), 0) != WAIT_OBJECT_0) {
            report.exit_code = 3;
            report.success = false;
            report.last_stage = "preflight_failed";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            if (!publish_report(json, cmd.report_path.string())) return 11;
            ReleaseMutex(run_mutex.get());
            return 3;
        }

        // Build concrete components
        auto adapter = myrd::aim::discover_aim_adapter();
        if (!adapter) {
            ReleaseMutex(run_mutex.get());
            return finish_with_error(5, adapter.error());
        }

        myrd::aim::WindowsSrbTransport transport(
            adapter.value().handle.get());
        myrd::aim::DirectAimAdapter aim(transport);
        report.driver.adapter_interface_path =
            w2s(adapter.value().interface_path);
        auto run_version = aim.query_version();
        if (!run_version) {
            ReleaseMutex(run_mutex.get());
            return finish_with_error(5, run_version.error());
        }
        report.driver.query_version_raw =
            std::to_string(run_version.value().api_version) +
            " (sub_version=0x" +
            [](std::uint32_t sv) {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%08X", sv);
                return std::string(buffer);
            }(run_version.value().sub_version) + ")";

        // Kernel adapter wrapper (for lifecycle runner)
        struct KernelAdapter : myrd::poc::IKernelDiskAdapter {
            myrd::aim::DirectAimAdapter& aim;
            myrd::aim::WindowsSrbTransport& transport;
            std::uint32_t dev_inst;
            KernelAdapter(myrd::aim::DirectAimAdapter& a,
                          myrd::aim::WindowsSrbTransport& t,
                          std::uint32_t di)
                : aim(a), transport(t), dev_inst(di) {}
            myrd::Result<std::vector<myrd::aim::DeviceId>> list_devices() override { return aim.list_devices(); }
            myrd::Result<myrd::aim::DeviceId> create_proxy_disk(const myrd::aim::ProxyDiskSpec& s) override { return aim.create_proxy_disk(s); }
            myrd::Result<void> rescan_adapter() override { return myrd::aim::rescan_aim_adapter(dev_inst); }
            myrd::Result<myrd::aim::DiskInfo> query_device(myrd::aim::DeviceId id) override { return aim.query_device(id); }
            myrd::Result<void> remove_device(myrd::aim::DeviceId id) override { return aim.remove_device(id); }
            myrd::Result<void> wait_until_absent(myrd::aim::DeviceId id, std::chrono::milliseconds to) override {
                auto deadline = std::chrono::steady_clock::now() + to;
                while (std::chrono::steady_clock::now() < deadline) {
                    auto list = aim.list_devices();
                    if (!list) return myrd::Result<void>::failure(list.error());
                    bool found = false;
                    for (auto& d : list.value())
                        if (d.value == id.value) { found = true; break; }
                    if (!found) return myrd::Result<void>::success();
                    Sleep(250);
                }
                return myrd::Result<void>::failure(
                    {myrd::ErrorDomain::evidence, WAIT_TIMEOUT,
                     L"wait_until_absent",
                     L"created AIM device remained present after timeout"});
            }
        };

        // Disk locator wrapper
        struct DiskLocator : myrd::poc::IDiskLocator {
            myrd::Result<std::vector<std::uint32_t>> snapshot_numbers() override { return myrd::aim::snapshot_physical_drive_numbers(); }
            myrd::Result<myrd::aim::LocatedDisk> wait_for(myrd::aim::DeviceId id, std::span<const std::uint32_t> bl) override { return myrd::aim::wait_for_created_physical_drive(id, bl, std::chrono::seconds(15)); }
        };

        KernelAdapter kAdapter(aim, transport, adapter.value().dev_inst);
        DiskLocator kLocator;

        // M0Provider for lifecycle
        myrd::poc::M0Provider provider;

        myrd::poc::RunConfig config;
        config.run_id = report.run_id;
        config.started_utc = report.started_utc;
        config.debug_build = debug;
        config.injected_failure = cmd.injected_failure;
        config.report_path = cmd.report_path.string();

        myrd::poc::WindowsRawDiskIo diskIo;
        myrd::poc::RawRoundTrip realRt(diskIo);
        auto result = myrd::poc::run_lifecycle(
            provider, kAdapter, kLocator, realRt, config);

        result.environment = report.environment;
        result.dependency_audit = report.dependency_audit;
        result.driver = report.driver;
        result.finished_utc = current_utc();
        result.last_stage = "report_written";
        auto json = myrd::poc::serialize_report(result);
        if (!publish_report(json, cmd.report_path.string())) {
            ReleaseMutex(run_mutex.get());
            return 11;
        }

        ReleaseMutex(run_mutex.get());
        return result.exit_code;

    } catch (const std::exception& e) {
        report.exit_code = 12;
        report.success = false;
        report.primary_error = myrd::poc::ErrorEvidence{
            "internal", 0, "exception", e.what()};
        report.last_stage = "report_written";
        report.finished_utc = current_utc();
        auto json = myrd::poc::serialize_report(report);
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        if (!requested_report_path.empty() &&
            !publish_report(json, requested_report_path)) return 11;
        return 12;
    } catch (...) {
        report.exit_code = 12;
        report.success = false;
        report.primary_error = myrd::poc::ErrorEvidence{
            "internal", 0, "unknown_exception", "unknown exception"};
        report.last_stage = "report_written";
        report.finished_utc = current_utc();
        auto json = myrd::poc::serialize_report(report);
        std::fprintf(stderr, "FATAL: unknown exception\n");
        if (!requested_report_path.empty() &&
            !publish_report(json, requested_report_path)) return 11;
        return 12;
    }
}
