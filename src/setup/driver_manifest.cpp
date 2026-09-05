#include "src/setup/driver_manifest.h"

#include "src/common/authenticode.h"
#include "src/common/sha256.h"
#include "src/common/sha256_stream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>
#include <winsvc.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>

namespace myrd::setup {

// ---------------------------------------------------------------------------
// immutable manifest singleton
// ---------------------------------------------------------------------------
const DriverManifest& driver_manifest() noexcept
{
    static constexpr DriverManifest manifest = {
        .upstream_commit = "cb7a2347e9595b12e2bd178ce803090a7a674037",
        .archive_url =
            "https://raw.githubusercontent.com/ArsenalRecon/"
            "Arsenal-Image-Mounter/"
            "cb7a2347e9595b12e2bd178ce803090a7a674037/"
            "DriverSetup/DriverFiles.zip",
        .archive_sha256 =
            "B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324",
        .x64_sys_sha256 =
            "3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9",
        .driver_version = "1.2.28.93",
        .architecture = DriverArchitecture::x64,
        .service_name = "phdskmnt",
    };
    return manifest;
}

// ---------------------------------------------------------------------------
// strict SHA-256 hex parser (case-insensitive, canonical uppercase)
// ---------------------------------------------------------------------------
Result<std::string> parse_sha256_hex(std::string_view input)
{
    if (input.size() != 64) {
        return Result<std::string>::failure(Error{
            ErrorDomain::usage, 1,
            L"parse_sha256_hex",
            L"SHA-256 hex must be exactly 64 characters"});
    }

    std::string canonical;
    canonical.reserve(64);

    for (char c : input) {
        if (c >= '0' && c <= '9') {
            canonical.push_back(c);
        } else if (c >= 'a' && c <= 'f') {
            canonical.push_back(static_cast<char>(std::toupper(c)));
        } else if (c >= 'A' && c <= 'F') {
            canonical.push_back(c);
        } else {
            return Result<std::string>::failure(Error{
                ErrorDomain::usage, 2,
                L"parse_sha256_hex",
                L"SHA-256 hex contains non-hex character"});
        }
    }

    return Result<std::string>::success(std::move(canonical));
}

// ---------------------------------------------------------------------------
// helper: convert narrow (UTF-8) string to wide string
// ---------------------------------------------------------------------------
static std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty()) return {};
    if (utf8.size() >
        static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), result.data(), len) != len) {
        return {};
    }
    return result;
}

// ---------------------------------------------------------------------------
// helper: query service configuration and state from SCM (read-only)
// ---------------------------------------------------------------------------
class ScHandle final {
public:
    explicit ScHandle(SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScHandle() { if (handle_ != nullptr) CloseServiceHandle(handle_); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    ScHandle(ScHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    ScHandle& operator=(ScHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) CloseServiceHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
private:
    SC_HANDLE handle_{};
};

struct ServiceSnapshot final {
    DriverServiceState state{DriverServiceState::query_failed};
    std::optional<std::wstring> binary_path;
};

static ServiceSnapshot query_service_snapshot(const std::wstring& service_name) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return {};

    ScHandle service(OpenServiceW(
        scm.get(), service_name.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service) {
        const auto error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST ||
            error == ERROR_FILE_NOT_FOUND) {
            return {DriverServiceState::absent, std::nullopt};
        }
        return {};
    }

    DWORD needed = 0;
    static_cast<void>(QueryServiceConfigW(service.get(), nullptr, 0, &needed));
    constexpr DWORD kMaximumConfigBytes = 1U << 20;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0 ||
        needed > kMaximumConfigBytes) {
        return {};
    }
    auto buffer = std::make_unique<std::byte[]>(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.get());
    if (!QueryServiceConfigW(service.get(), config, needed, &needed) ||
        config->lpBinaryPathName == nullptr ||
        config->lpBinaryPathName[0] == L'\0') {
        return {};
    }

    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusEx(
            service.get(), SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
        return {};
    }
    DriverServiceState state = DriverServiceState::transitional;
    if (status.dwCurrentState == SERVICE_RUNNING) {
        state = DriverServiceState::running;
    } else if (status.dwCurrentState == SERVICE_STOPPED) {
        state = DriverServiceState::stopped;
    }
    return {state, std::wstring(config->lpBinaryPathName)};
}

// ---------------------------------------------------------------------------
// helper: resolve a potentially-relative kernel driver path to a full path
// ---------------------------------------------------------------------------
static std::wstring resolve_driver_path(const std::wstring& raw)
{
    std::wstring path = raw;
    const auto first = path.find_first_not_of(L" \t");
    const auto last = path.find_last_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    path = path.substr(first, last - first + 1);
    if (path.front() == L'"') {
        const auto closing = path.find(L'"', 1);
        if (closing == std::wstring::npos ||
            path.find_first_not_of(L" \t", closing + 1) != std::wstring::npos) {
            return {};
        }
        path = path.substr(1, closing - 1);
    }

    if (path.rfind(L"\\??\\", 0) == 0) {
        path.erase(0, 4);
    }

    if (path.find(L'%') != std::wstring::npos) {
        const DWORD required = ExpandEnvironmentStringsW(
            path.c_str(), nullptr, 0);
        if (required == 0 || required > 32'768) return {};
        std::wstring expanded(required, L'\0');
        if (ExpandEnvironmentStringsW(
                path.c_str(), expanded.data(), required) != required) {
            return {};
        }
        expanded.pop_back();
        path = std::move(expanded);
    }

    // If it starts with "\SystemRoot\", replace with actual system root
    if (path.size() >= 12 &&
        _wcsnicmp(path.c_str(), L"\\SystemRoot\\", 12) == 0) {
        wchar_t sys_root[MAX_PATH];
        UINT len = GetSystemWindowsDirectoryW(sys_root, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            path = std::wstring(sys_root) + path.substr(11);
        }
    } else if (path.size() >= 11 &&
               _wcsnicmp(path.c_str(), L"SystemRoot\\", 11) == 0) {
        wchar_t sys_root[MAX_PATH];
        const UINT len = GetSystemWindowsDirectoryW(sys_root, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            path = std::wstring(sys_root) + L"\\" + path.substr(11);
        }
    }

    // If the path is relative (no drive letter or UNC), try prepending system root
    if (path.size() >= 2 && path[1] != L':' && path[0] != L'\\') {
        wchar_t sys_root[MAX_PATH];
        UINT len = GetSystemWindowsDirectoryW(sys_root, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            path = std::wstring(sys_root) + L"\\" + path;
        }
    }

    return path;
}

static std::optional<std::string> to_utf8(std::wstring_view text) {
    if (text.empty() || text.size() >
            static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), output.data(), required,
            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return output;
}

// ---------------------------------------------------------------------------
// helper: extract PE machine type from file header (read-only)
// ---------------------------------------------------------------------------
static std::optional<DriverArchitecture> pe_architecture(
    const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER file_size{};
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!GetFileSizeEx(h, &file_size) || file_size.QuadPart <
            static_cast<LONGLONG>(sizeof(dos)) ||
        !ReadFile(h, &dos, sizeof(dos), &read, nullptr) ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
        CloseHandle(h);
        return std::nullopt;
    }
    constexpr LONGLONG kNtHeaderPrefixBytes =
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (static_cast<LONGLONG>(dos.e_lfanew) >
        file_size.QuadPart - kNtHeaderPrefixBytes) {
        CloseHandle(h);
        return std::nullopt;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!SetFilePointerEx(h, offset, nullptr, FILE_BEGIN) ||
        !ReadFile(h, &signature, sizeof(signature), &read, nullptr) ||
        read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadFile(h, &header, sizeof(header), &read, nullptr) ||
        read != sizeof(header)) {
        CloseHandle(h);
        return std::nullopt;
    }
    CloseHandle(h);
    switch (header.Machine) {
    case IMAGE_FILE_MACHINE_AMD64: return DriverArchitecture::x64;
    case IMAGE_FILE_MACHINE_I386: return DriverArchitecture::x86;
    case IMAGE_FILE_MACHINE_ARM64: return DriverArchitecture::arm64;
    default: return DriverArchitecture::unknown;
    }
}

// ---------------------------------------------------------------------------
// helper: get file version string (read-only)
// ---------------------------------------------------------------------------
static std::optional<std::string> query_file_version(
    const std::wstring& path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::nullopt;

    auto buf = std::make_unique<std::byte[]>(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.get())) {
        return std::nullopt;
    }

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_bytes = 0;
    if (!VerQueryValueW(buf.get(), L"\\",
            reinterpret_cast<LPVOID*>(&fixed), &fixed_bytes) ||
        fixed == nullptr || fixed_bytes < sizeof(VS_FIXEDFILEINFO) ||
        fixed->dwSignature != 0xFEEF04BD) {
        return std::nullopt;
    }
    return std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(fixed->dwFileVersionLS));
}

// ---------------------------------------------------------------------------
// helper: streaming SHA-256 of a file (read-only, bounded reads)
// ---------------------------------------------------------------------------
static std::optional<std::string> compute_file_sha256(
    const std::wstring& path)
{
    // Use Windows API for reliable wide-char path opening
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    Sha256Stream stream;
    std::array<std::byte, 65536> buf{};
    DWORD bytes_read = 0;

    for (;;) {
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()),
                      &bytes_read, nullptr)) {
            CloseHandle(h);
            return std::nullopt;
        }
        if (bytes_read == 0) break;
        auto update = stream.update(
            std::span<const std::byte>(buf.data(), bytes_read));
        if (!update.ok()) {
            CloseHandle(h);
            return std::nullopt;
        }
    }

    CloseHandle(h);
    auto finalized = stream.finalize();
    if (!finalized.ok()) return std::nullopt;
    return finalized.move_value();
}

// ---------------------------------------------------------------------------
// read-only driver status classifier with explicit precedence
//
// Precedence (first match wins, highest priority first):
//   1. inspection_failure   — SCM inaccessible or file inspection error
//   2. absent               — phdskmnt service not installed
//   3. architecture_mismatch — installed SYS is not x64
//   4. signature_mismatch   — Authenticode verification failed
//   5. sys_hash_mismatch    — SYS file SHA-256 does not match pin
//   6. version_mismatch     — file version != 1.2.28.93
//   7. busy                 — service in transitional state (pending, etc.)
//   8. stopped              — service installed, not running, all checks pass
//   9. running_compatible   — service running, all checks pass
// ---------------------------------------------------------------------------
static std::string_view architecture_name(DriverArchitecture architecture) {
    switch (architecture) {
    case DriverArchitecture::x64: return "x64";
    case DriverArchitecture::x86: return "x86";
    case DriverArchitecture::arm64: return "arm64";
    case DriverArchitecture::unknown: return "unknown";
    }
    return "unknown";
}

DriverStatusResult classify_driver_status(
    const DriverManifest& manifest, const DriverObservation& observation) {
    DriverStatusResult result{};
    result.installed_version = observation.installed_version;
    result.sys_path = observation.sys_path;
    result.sys_hash = observation.sys_hash;
    result.signature_valid = observation.signature_valid;
    result.signature_trust_status = observation.signature_trust_status;
    if (observation.architecture) {
        result.architecture = std::string(
            architecture_name(*observation.architecture));
    }

    if (observation.service_state == DriverServiceState::absent) {
        result.status = DriverStatus::absent;
        return result;
    }
    if (observation.service_state == DriverServiceState::query_failed ||
        !observation.installed_version || !observation.sys_hash ||
        !observation.signature_valid || !observation.signature_trust_status ||
        !observation.architecture) {
        result.status = DriverStatus::inspection_failure;
        return result;
    }
    if (*observation.architecture != manifest.architecture) {
        result.status = DriverStatus::architecture_mismatch;
        return result;
    }
    if (!*observation.signature_valid) {
        result.status = DriverStatus::signature_mismatch;
        return result;
    }
    auto actual_hash = parse_sha256_hex(*observation.sys_hash);
    auto expected_hash = parse_sha256_hex(manifest.x64_sys_sha256);
    if (!actual_hash.ok() || !expected_hash.ok()) {
        result.status = DriverStatus::inspection_failure;
        return result;
    }
    result.sys_hash = actual_hash.value();
    if (actual_hash.value() != expected_hash.value()) {
        result.status = DriverStatus::sys_hash_mismatch;
        return result;
    }
    if (*observation.installed_version != manifest.driver_version) {
        result.status = DriverStatus::version_mismatch;
        return result;
    }
    switch (observation.service_state) {
    case DriverServiceState::running:
        result.status = DriverStatus::running_compatible;
        break;
    case DriverServiceState::stopped:
        result.status = DriverStatus::stopped;
        break;
    case DriverServiceState::transitional:
        result.status = DriverStatus::busy;
        break;
    case DriverServiceState::absent:
    case DriverServiceState::query_failed:
        result.status = DriverStatus::inspection_failure;
        break;
    }
    return result;
}

DriverStatusResult inspect_installed_driver(const DriverManifest& manifest) {
    DriverObservation observation{};
    try {
        const auto service_name = to_wide(manifest.service_name);
        if (service_name.empty()) {
            return classify_driver_status(manifest, observation);
        }
        const auto service = query_service_snapshot(service_name);
        observation.service_state = service.state;
        if (service.state == DriverServiceState::absent ||
            service.state == DriverServiceState::query_failed ||
            !service.binary_path) {
            return classify_driver_status(manifest, observation);
        }

        const auto path = resolve_driver_path(*service.binary_path);
        observation.sys_path = to_utf8(path);
        if (path.empty() || !observation.sys_path) {
            return classify_driver_status(manifest, observation);
        }
        observation.architecture = pe_architecture(path);
        const auto signature = verify_authenticode(path);
        if (signature.ok()) {
            observation.signature_trust_status =
                signature.value().trust_status;
            observation.signature_valid =
                signature.value().trust_status == ERROR_SUCCESS;
        }
        observation.sys_hash = compute_file_sha256(path);
        observation.installed_version = query_file_version(path);
        return classify_driver_status(manifest, observation);
    } catch (...) {
        observation.service_state = DriverServiceState::query_failed;
        return classify_driver_status(manifest, observation);
    }
}

// ---------------------------------------------------------------------------
// status enum helpers
// ---------------------------------------------------------------------------
std::string_view to_string(DriverStatus status) noexcept
{
    switch (status) {
    case DriverStatus::absent:               return "absent";
    case DriverStatus::stopped:              return "stopped";
    case DriverStatus::running_compatible:   return "running_compatible";
    case DriverStatus::version_mismatch:     return "version_mismatch";
    case DriverStatus::sys_hash_mismatch:    return "sys_hash_mismatch";
    case DriverStatus::signature_mismatch:   return "signature_mismatch";
    case DriverStatus::architecture_mismatch: return "architecture_mismatch";
    case DriverStatus::inspection_failure:   return "inspection_failure";
    case DriverStatus::busy:                 return "busy";
    }
    return "";
}

bool is_valid(DriverStatus status) noexcept
{
    switch (status) {
    case DriverStatus::absent:
    case DriverStatus::stopped:
    case DriverStatus::running_compatible:
    case DriverStatus::version_mismatch:
    case DriverStatus::sys_hash_mismatch:
    case DriverStatus::signature_mismatch:
    case DriverStatus::architecture_mismatch:
    case DriverStatus::inspection_failure:
    case DriverStatus::busy:
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON serialization with path redaction
// ---------------------------------------------------------------------------
product::JsonObject driver_status_to_json(
    const DriverStatusResult& result,
    bool include_paths)
{
    using product::JsonField;
    using product::JsonScalar;

    product::JsonObject obj;
    obj.reserve(7);

    obj.push_back(JsonField{"status", std::string(to_string(result.status))});

    if (result.installed_version) {
        obj.push_back(JsonField{"installed_version", *result.installed_version});
    }
    if (result.architecture) {
        obj.push_back(JsonField{"architecture", *result.architecture});
    }
    if (result.sys_hash) {
        obj.push_back(JsonField{"sys_hash", *result.sys_hash});
    }
    if (result.signature_valid) {
        obj.push_back(JsonField{"signature_valid", *result.signature_valid});
    }
    if (result.signature_trust_status) {
        obj.push_back(JsonField{
            "signature_trust_status",
            static_cast<std::uint64_t>(*result.signature_trust_status)});
    }
    if (include_paths && result.sys_path) {
        obj.push_back(JsonField{"sys_path", *result.sys_path});
    }

    return obj;
}

} // namespace myrd::setup
