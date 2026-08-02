#include "m1_cli.h"
#include "m1_lifecycle_runner.h"
#include "m1_provider.h"
#include "m1_raw_protocol_io.h"
#include "m1_report.h"

#include "src/common/authenticode.h"
#include "src/common/process_snapshot.h"
#include "src/common/sha256.h"
#include "src/common/unique_handle.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/driver/windows_srb_transport.h"
#include "src/poc/report_publisher.h"
#include "src/proxy/object_names.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::string current_utc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer),
        "%04u-%02u-%02uT%02u:%02u:%02uZ",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

bool equals_ci(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (towlower(left[index]) != towlower(right[index])) return false;
    }
    return true;
}

bool current_user_is_admin() {
    BOOL is_admin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (AllocateAndInitializeSid(&authority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &is_admin);
        FreeSid(group);
    }
    return is_admin != FALSE;
}

myrd::Error win32_error(const wchar_t* operation, const wchar_t* message) {
    return {myrd::ErrorDomain::win32,
            static_cast<std::uint32_t>(GetLastError()),
            operation, message};
}

class WindowsExecutionGuard final : public myrd::IM1ExecutionGuard {
public:
    ~WindowsExecutionGuard() override {
        if (owns_mutex_) ReleaseMutex(mutex_.get());
    }

    myrd::Result<void> require_elevated_and_acquire() override {
        if (!current_user_is_admin()) {
            return myrd::Result<void>::failure({
                myrd::ErrorDomain::preflight, ERROR_ELEVATION_REQUIRED,
                L"current_user_is_admin", L"M1 lifecycle requires elevation"});
        }
        mutex_ = myrd::UniqueHandle(CreateMutexW(
            nullptr, FALSE, L"Global\\myrd-m1-e2e-mutex"));
        if (!mutex_) {
            return myrd::Result<void>::failure(
                win32_error(L"CreateMutexW(m1-e2e)",
                            L"failed to create lifecycle mutex"));
        }
        const DWORD wait = WaitForSingleObject(mutex_.get(), 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            return myrd::Result<void>::failure({
                myrd::ErrorDomain::preflight,
                wait == WAIT_TIMEOUT ? WAIT_TIMEOUT
                                     : static_cast<std::uint32_t>(GetLastError()),
                L"WaitForSingleObject(m1-e2e)",
                L"another M1 lifecycle owns the global mutex"});
        }
        owns_mutex_ = true;
        return myrd::Result<void>::success();
    }

private:
    myrd::UniqueHandle mutex_;
    bool owns_mutex_ = false;
};

bool named_objects_absent(const myrd::proxy::ProxyObjectNames& names,
                          myrd::Error& error) {
    struct ObjectProbe {
        HANDLE handle;
        const wchar_t* operation;
    };
    std::array<ObjectProbe, 5> probes{{
        {OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str()), L"OpenMutexW"},
        {OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str()),
         L"OpenFileMappingW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.request.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.response.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.cancel.c_str()), L"OpenEventW"},
    }};
    for (auto& probe : probes) {
        if (probe.handle != nullptr) {
            CloseHandle(probe.handle);
            return false;
        }
        const auto code = static_cast<std::uint32_t>(GetLastError());
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_INVALID_NAME) {
            error = {myrd::ErrorDomain::cleanup, code,
                     probe.operation, L"failed to inspect named object"};
            return false;
        }
    }
    return true;
}

class WindowsProviderLifecycle final : public myrd::IM1ProviderLifecycle {
public:
    explicit WindowsProviderLifecycle(std::unique_ptr<myrd::M1Provider> provider)
        : provider_(std::move(provider)), names_(provider_->names()) {}

    myrd::Result<void> start() override { return provider_->start(); }

    myrd::Result<void> stop_and_release() override {
        auto result = provider_->stop();
        final_snapshot_ = provider_->snapshot();
        return result;
    }

    std::wstring native_object_path() const override {
        return provider_->native_object_path();
    }

    myrd::proxy::SessionSnapshot snapshot() const override {
        return final_snapshot_.has_value()
            ? *final_snapshot_ : provider_->snapshot();
    }

    myrd::Result<bool> named_objects_released() const override {
        myrd::Error error{};
        const bool absent = named_objects_absent(names_, error);
        if (!absent && error.operation.size() != 0) {
            return myrd::Result<bool>::failure(std::move(error));
        }
        return myrd::Result<bool>::success(absent);
    }

private:
    std::unique_ptr<myrd::M1Provider> provider_;
    myrd::proxy::ProxyObjectNames names_;
    std::optional<myrd::proxy::SessionSnapshot> final_snapshot_;
};

class WindowsProviderFactory final : public myrd::IM1ProviderFactory {
public:
    myrd::Result<std::unique_ptr<myrd::IM1ProviderLifecycle>> create() override {
        auto provider = myrd::M1Provider::create();
        if (!provider) {
            return myrd::Result<
                std::unique_ptr<myrd::IM1ProviderLifecycle>>::failure(
                    provider.error());
        }
        std::unique_ptr<myrd::IM1ProviderLifecycle> lifecycle =
            std::make_unique<WindowsProviderLifecycle>(
                std::move(provider.value()));
        return myrd::Result<
            std::unique_ptr<myrd::IM1ProviderLifecycle>>::success(
                std::move(lifecycle));
    }
};

struct ServiceInfo {
    std::string state;
    std::wstring path;
};

ServiceInfo query_service(std::wstring_view service_name) {
    ServiceInfo result;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return result;
    const std::wstring name(service_name);
    SC_HANDLE service = OpenServiceW(
        manager, name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        return result;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
        result.state = status.dwCurrentState == SERVICE_RUNNING
            ? "Running" : status.dwCurrentState == SERVICE_STOPPED
                ? "Stopped" : std::to_string(status.dwCurrentState);
    }
    DWORD required = 0;
    QueryServiceConfigW(service, nullptr, 0, &required);
    if (required != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<std::byte> storage(required);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
        if (QueryServiceConfigW(service, config, required, &required) &&
            config->lpBinaryPathName != nullptr) {
            result.path = config->lpBinaryPathName;
            if (!result.path.empty() && result.path.front() == L'"') {
                const auto quote = result.path.find(L'"', 1);
                result.path = result.path.substr(1, quote - 1);
            }
            constexpr std::wstring_view prefix = L"\\SystemRoot\\";
            if (result.path.starts_with(prefix)) {
                wchar_t windows[MAX_PATH]{};
                const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
                if (length != 0 && length < MAX_PATH) {
                    result.path = std::wstring(windows, length) + L"\\" +
                        result.path.substr(prefix.size());
                }
            }
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}

std::string hash_file(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
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
    std::ostringstream output;
    output << HIWORD(info->dwFileVersionMS) << '.'
           << LOWORD(info->dwFileVersionMS) << '.'
           << HIWORD(info->dwFileVersionLS) << '.'
           << LOWORD(info->dwFileVersionLS);
    return output.str();
}

class WindowsKernelAdapter final : public myrd::IM1KernelAdapter {
public:
    myrd::Result<myrd::M1DriverEvidence> query_driver() override {
        auto initialized = ensure_initialized();
        if (!initialized) {
            return myrd::Result<myrd::M1DriverEvidence>::failure(
                initialized.error());
        }
        auto version = adapter_->query_version();
        if (!version) {
            return myrd::Result<myrd::M1DriverEvidence>::failure(
                version.error());
        }
        myrd::M1DriverEvidence evidence;
        evidence.service_name = "phdskmnt";
        const auto service = query_service(L"phdskmnt");
        evidence.service_state = service.state;
        evidence.file_path = narrow(service.path);
        evidence.file_version = file_version(service.path);
        evidence.sha256 = hash_file(service.path);
        const auto signature = myrd::verify_authenticode(service.path);
        if (signature) {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase
                   << signature.value().trust_status;
            evidence.authenticode_status = stream.str();
            evidence.signer_subject = narrow(signature.value().signer_subject);
        }
        evidence.adapter_interface_path = narrow(handle_->interface_path);
        evidence.api_version = version.value().api_version;
        evidence.sub_version = version.value().sub_version;
        evidence.query_version_raw = std::to_string(version.value().api_version) +
            ":" + std::to_string(version.value().sub_version);

        constexpr std::array<std::wstring_view, 2> forbidden_modules{
            L"aimapi.dll", L"devio.dll"};
        auto modules = myrd::snapshot_current_modules();
        if (modules) {
            for (const auto& module : modules.value()) {
                evidence.loaded_modules.push_back(narrow(module.path));
                for (const auto forbidden : forbidden_modules) {
                    if (equals_ci(module.basename, forbidden)) {
                        evidence.forbidden_loaded_modules.push_back(
                            narrow(module.path));
                    }
                }
            }
        }
        constexpr std::array<std::wstring_view, 9> forbidden_processes{
            L"aim_ll.exe", L"RamDyn.exe", L"AIMtk.exe", L"AIMtk-Dlg.exe",
            L"AIMtk-svc.exe", L"aim_cli.exe", L"MountImg.exe",
            L"RamDiskUI.exe", L"deviosvc.exe"};
        auto processes = myrd::snapshot_processes();
        if (processes) {
            for (const auto& process : processes.value()) {
                for (const auto forbidden : forbidden_processes) {
                    if (equals_ci(process.name, forbidden)) {
                        evidence.forbidden_processes.push_back(
                            narrow(process.name));
                    }
                }
            }
        }
        return myrd::Result<myrd::M1DriverEvidence>::success(
            std::move(evidence));
    }

    myrd::Result<std::vector<myrd::aim::DeviceId>> list_devices() override {
        auto initialized = ensure_initialized();
        if (!initialized) {
            return myrd::Result<std::vector<myrd::aim::DeviceId>>::failure(
                initialized.error());
        }
        return adapter_->list_devices();
    }

    myrd::Result<myrd::aim::DeviceId> create_proxy_disk(
        const myrd::aim::ProxyDiskSpec& spec) override {
        return adapter_->create_proxy_disk(spec);
    }

    myrd::Result<void> rescan_adapter() override {
        return myrd::aim::rescan_aim_adapter(handle_->dev_inst);
    }

    myrd::Result<myrd::aim::DiskInfo> query_device(
        myrd::aim::DeviceId id) override {
        return adapter_->query_device(id);
    }

    myrd::Result<void> remove_device(myrd::aim::DeviceId id) override {
        if (id.value == myrd::aim::kAllDevices ||
            id.value == myrd::aim::kAutoDeviceNumber) {
            return myrd::Result<void>::failure({
                myrd::ErrorDomain::cleanup, ERROR_INVALID_PARAMETER,
                L"remove_device", L"refusing sentinel/all-device removal"});
        }
        return adapter_->remove_device(id);
    }

    myrd::Result<void> wait_until_absent(
        myrd::aim::DeviceId id,
        std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            auto devices = adapter_->list_devices();
            if (!devices) return myrd::Result<void>::failure(devices.error());
            const bool found = std::any_of(
                devices.value().begin(), devices.value().end(),
                [id](myrd::aim::DeviceId value) { return value == id; });
            if (!found) return myrd::Result<void>::success();
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        } while (true);
        return myrd::Result<void>::failure({
            myrd::ErrorDomain::evidence, WAIT_TIMEOUT,
            L"wait_until_absent", L"created device remained present"});
    }

private:
    myrd::Result<void> ensure_initialized() {
        if (adapter_) return myrd::Result<void>::success();
        auto discovered = myrd::aim::discover_aim_adapter();
        if (!discovered) {
            return myrd::Result<void>::failure(discovered.error());
        }
        handle_ = std::move(discovered.value());
        transport_ = std::make_unique<myrd::aim::WindowsSrbTransport>(
            handle_->handle.get());
        adapter_ = std::make_unique<myrd::aim::DirectAimAdapter>(*transport_);
        return myrd::Result<void>::success();
    }

    std::optional<myrd::aim::AdapterHandle> handle_;
    std::unique_ptr<myrd::aim::WindowsSrbTransport> transport_;
    std::unique_ptr<myrd::aim::DirectAimAdapter> adapter_;
};

class WindowsDiskLocator final : public myrd::IM1DiskLocator {
public:
    myrd::Result<std::vector<std::uint32_t>> snapshot_numbers() override {
        return myrd::aim::snapshot_physical_drive_numbers();
    }
    myrd::Result<myrd::aim::LocatedDisk> wait_for(
        myrd::aim::DeviceId id,
        std::span<const std::uint32_t> baseline) override {
        return myrd::aim::wait_for_created_physical_drive(
            id, baseline, std::chrono::seconds(15));
    }
};

class WindowsProtocolVerifier final : public myrd::IM1ProtocolVerifier {
public:
    myrd::Result<myrd::M1RawProtocolEvidence> verify(
        const myrd::aim::LocatedDisk& disk,
        const myrd::proxy::RequestCounters& before,
        const std::function<myrd::proxy::RequestCounters()>& counters) override {
        return myrd::verify_m1_raw_protocol(api_, disk, before, counters);
    }
private:
    myrd::WindowsM1RawDiskApi api_;
};

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#ifdef _DEBUG
    constexpr bool debug_build = true;
#else
    constexpr bool debug_build = false;
#endif

    std::vector<std::wstring_view> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    auto parsed = myrd::parse_m1_cli(arguments, debug_build);
    if (!parsed) {
        std::fwprintf(stderr,
            L"Usage: myrd-m1-e2e run --report reports\\<new-file>.json "
            L"[--inject-failure-after <stage>]\n");
        return 2;
    }

    auto run_id = myrd::proxy::generate_session_id();
    if (!run_id) return 12;

    WindowsExecutionGuard guard;
    WindowsProviderFactory providers;
    WindowsKernelAdapter kernel;
    WindowsDiskLocator locator;
    WindowsProtocolVerifier protocol;

    myrd::M1RunConfig config;
    config.run_id = narrow(run_id.value());
    config.started_utc = current_utc();
    config.failure_point = parsed.value().failure_point;
    auto report = myrd::run_m1_lifecycle(
        guard, providers, kernel, locator, protocol, config);
    report.finished_utc = current_utc();

    const auto json = myrd::serialize_m1_report(report);
    auto published = myrd::poc::publish_report_atomic(
        json, parsed.value().report_path);
    if (!published) {
        std::fprintf(stderr, "M1 report publication failed (code=%u)\n",
                     published.error().code);
        return 11;
    }
    std::printf("M1 report written: %s\n",
                parsed.value().report_path.string().c_str());
    return report.exit_code;
}
