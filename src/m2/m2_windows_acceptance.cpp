#include "m2_windows_acceptance.h"

#include "m2_lifecycle_runner.h"
#include "m2_performance_probe.h"
#include "src/common/process_snapshot.h"
#include "src/common/sha256.h"
#include "src/common/unique_handle.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/driver/windows_srb_transport.h"
#include "src/m1/m1_provider.h"
#include "src/poc/report_publisher.h"
#include "src/proxy/object_names.h"
#include "src/storage/block_constants.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace myrd {
namespace {

using namespace std::chrono_literals;

std::string narrow_utf8(std::wstring_view value);

Error win32_error(
    const wchar_t* operation,
    const wchar_t* message,
    DWORD code = GetLastError()) {
    return {ErrorDomain::win32,
            static_cast<std::uint32_t>(code),
            operation,
            message};
}

Error acceptance_error(
    const wchar_t* operation,
    const wchar_t* message,
    std::uint32_t code = static_cast<std::uint32_t>(
        OperationErrorCode::internal_invariant)) {
    return {ErrorDomain::evidence, code, operation, message};
}

std::string narrow_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count,
            nullptr,
            nullptr) != count) {
        return {};
    }
    return result;
}

std::wstring widen_utf8(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count) != count) {
        return {};
    }
    return result;
}

bool equals_ci(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

Result<std::vector<std::byte>> read_file_bytes(
    const std::filesystem::path& path,
    const wchar_t* operation) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::vector<std::byte>>::failure(
            acceptance_error(operation, L"failed to open input file"));
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        return Result<std::vector<std::byte>>::failure(
            acceptance_error(operation, L"failed to size input file"));
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return Result<std::vector<std::byte>>::failure(
                acceptance_error(operation, L"failed to read input file"));
        }
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<std::string> hash_file(
    const std::filesystem::path& path,
    const wchar_t* operation) {
    auto bytes = read_file_bytes(path, operation);
    if (!bytes) return Result<std::string>::failure(bytes.error());
    return sha256_hex(bytes.value());
}

std::string hash_text(std::string_view text) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
    auto digest = sha256_hex(bytes);
    return digest ? digest.value() : std::string(64, '0');
}

std::wstring quote_process_argument(std::wstring_view value) {
    if (value.empty()) return L"\"\"";
    const bool needs_quotes = value.find_first_of(L" \t\n\v\"") !=
        std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(value);

    std::wstring result(1, L'\"');
    std::size_t backslashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

Result<void> run_process(
    const OperationContext& context,
    std::span<const std::wstring> arguments,
    const wchar_t* operation) {
    if (arguments.empty()) {
        return Result<void>::failure(
            acceptance_error(operation, L"empty child command"));
    }
    auto checked = check_operation_context(context);
    if (!checked) return checked;

    std::wstring command_line;
    for (const auto& argument : arguments) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += quote_process_argument(argument);
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            arguments.front().c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return Result<void>::failure(
            win32_error(operation, L"failed to start child process"));
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);

    for (;;) {
        const auto wait = WaitForSingleObject(process_handle.get(), 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) {
            const auto error = GetLastError();
            TerminateProcess(process_handle.get(), 0xE002u);
            WaitForSingleObject(process_handle.get(), 5000);
            return Result<void>::failure(
                win32_error(operation, L"failed while waiting for child", error));
        }
        checked = check_operation_context(context);
        if (!checked) {
            TerminateProcess(process_handle.get(), 0xE003u);
            WaitForSingleObject(process_handle.get(), 5000);
            return checked;
        }
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        return Result<void>::failure(
            win32_error(operation, L"failed to read child exit code"));
    }
    if (exit_code != 0) {
        return Result<void>::failure(acceptance_error(
            operation,
            L"child process returned a non-zero exit code",
            exit_code));
    }
    return check_operation_context(context);
}

Result<std::filesystem::path> locate_repository_root(
    const std::filesystem::path& executable_path) {
    std::error_code error;
    auto current = std::filesystem::canonical(executable_path, error);
    if (error) {
        return Result<std::filesystem::path>::failure(
            acceptance_error(
                L"locate_repository_root",
                L"executable path cannot be canonicalized"));
    }
    current = current.parent_path();
    for (int depth = 0; depth < 10 && !current.empty(); ++depth) {
        const auto workload =
            current / L"tests" / L"e2e" / L"Invoke-M2NtfsWorkload.ps1";
        const auto verifier =
            current / L"tools" / L"Verify-M1Json.ps1";
        if (std::filesystem::is_regular_file(workload, error) && !error &&
            std::filesystem::is_regular_file(verifier, error) && !error) {
            return Result<std::filesystem::path>::success(current);
        }
        current = current.parent_path();
    }
    return Result<std::filesystem::path>::failure(
        acceptance_error(
            L"locate_repository_root",
            L"could not locate checked-in M2 workload and M1 verifier"));
}

Result<std::string> read_utf8_text(
    const std::filesystem::path& path,
    const wchar_t* operation) {
    auto bytes = read_file_bytes(path, operation);
    if (!bytes) return Result<std::string>::failure(bytes.error());
    std::size_t offset = 0;
    if (bytes.value().size() >= 3 &&
        bytes.value()[0] == std::byte{0xEF} &&
        bytes.value()[1] == std::byte{0xBB} &&
        bytes.value()[2] == std::byte{0xBF}) {
        offset = 3;
    }
    return Result<std::string>::success(std::string(
        reinterpret_cast<const char*>(bytes.value().data() + offset),
        bytes.value().size() - offset));
}

std::optional<std::size_t> json_value_start(
    std::string_view json,
    std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto position = json.find(needle);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + needle.size());
    if (position == std::string_view::npos) return std::nullopt;
    do {
        ++position;
    } while (position < json.size() &&
             std::isspace(static_cast<unsigned char>(json[position])));
    return position;
}

std::optional<std::uint64_t> json_uint64(
    std::string_view json,
    std::string_view key) {
    const auto start = json_value_start(json, key);
    if (!start || *start >= json.size()) return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        json.data() + *start, json.data() + json.size(), value);
    if (parsed.ec != std::errc{}) return std::nullopt;
    return value;
}

std::optional<std::string> json_string(
    std::string_view json,
    std::string_view key) {
    const auto start = json_value_start(json, key);
    if (!start || *start >= json.size() || json[*start] != '"') {
        return std::nullopt;
    }
    std::string result;
    for (std::size_t index = *start + 1; index < json.size(); ++index) {
        const auto ch = json[index];
        if (ch == '"') return result;
        if (ch != '\\') {
            result.push_back(ch);
            continue;
        }
        if (++index >= json.size()) return std::nullopt;
        switch (json[index]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> json_bool(
    std::string_view json,
    std::string_view key) {
    const auto start = json_value_start(json, key);
    if (!start || *start >= json.size()) return std::nullopt;
    const auto is_token_end = [json](std::size_t position) {
        return position >= json.size() ||
            std::isspace(static_cast<unsigned char>(json[position])) ||
            json[position] == ',' || json[position] == '}';
    };
    if (json.substr(*start, 4) == "true" && is_token_end(*start + 4)) {
        return true;
    }
    if (json.substr(*start, 5) == "false" && is_token_end(*start + 5)) {
        return false;
    }
    return std::nullopt;
}

Result<M2ProviderConfig> provider_config_error(std::wstring message) {
    return Result<M2ProviderConfig>::failure({
        ErrorDomain::preflight,
        static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
        L"make_m2_real_provider_config",
        std::move(message)});
}

} // namespace

Result<std::filesystem::path> m2_system_powershell_path() {
    std::wstring system_directory(32768, L'\0');
    const auto length = GetSystemDirectoryW(
        system_directory.data(),
        static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return Result<std::filesystem::path>::failure(win32_error(
            L"GetSystemDirectoryW(m2)",
            L"failed to resolve the trusted system PowerShell directory"));
    }
    system_directory.resize(length);
    auto executable = std::filesystem::path(system_directory) /
        L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    std::error_code error;
    if (!executable.is_absolute() ||
        !std::filesystem::is_regular_file(executable, error) || error) {
        return Result<std::filesystem::path>::failure(acceptance_error(
            L"m2_system_powershell_path",
            L"trusted system PowerShell executable is missing",
            ERROR_FILE_NOT_FOUND));
    }
    return Result<std::filesystem::path>::success(std::move(executable));
}

bool m2_service_restoration_required(
    std::uint32_t current_state,
    std::uint32_t current_start_type,
    std::uint32_t baseline_state,
    std::uint32_t baseline_start_type) noexcept {
    return current_state != baseline_state ||
        current_start_type != baseline_start_type;
}

Result<std::uint64_t> validate_m2_delete_retrim_state(
    std::string_view json) {
    const auto phase = json_string(json, "phase");
    const auto deleted = json_uint64(json, "bytes_deleted");
    const auto delete_notify = json_bool(json, "delete_notify_enabled");
    const auto retrim = json_bool(json, "retrim_succeeded");
    if (!phase || *phase != "retrim_complete" ||
        !deleted || *deleted < (1ull << 30) ||
        !delete_notify || !*delete_notify || !retrim || !*retrim) {
        return Result<std::uint64_t>::failure(acceptance_error(
            L"validate_m2_delete_retrim_state",
            L"NTFS state lacks explicit DeleteNotify, ReTrim, or 1 GiB delete evidence"));
    }
    return Result<std::uint64_t>::success(*deleted);
}

Result<std::string> normalize_m2_aim_descriptor_bus_type(
    std::uint32_t raw_bus_type,
    std::string_view vendor_id,
    std::string_view product_id) {
    // The accepted phdskmnt.sys build reports BusTypeFibre (6) through
    // STORAGE_DEVICE_DESCRIPTOR, and Windows Get-Disk renders that enum as
    // "Fibre Channel". Keep the display-name mapping scoped to the pinned AIM
    // descriptor identity; never reinterpret an arbitrary Fibre-channel disk
    // as an M2 disk.
    constexpr std::uint32_t kBusTypeFibre = 6u;
    if (raw_bus_type != kBusTypeFibre ||
        !equals_ci(vendor_id, "Arsenal") ||
        !equals_ci(product_id, "Virtual")) {
        return Result<std::string>::failure(acceptance_error(
            L"normalize_m2_aim_descriptor_bus_type",
            L"raw storage descriptor does not match the pinned AIM identity"));
    }
    return Result<std::string>::success("Fibre Channel");
}

Result<M2ProviderConfig> make_m2_real_provider_config(
    const M2GeometryOptions& geometry,
    const M2BackingState& backing) {
    constexpr std::uint64_t kM2DiskSizeBytes = 64ull << 30;
    if (geometry.disk_size_bytes != kM2DiskSizeBytes ||
        geometry.sector_size_bytes != 512 ||
        geometry.block_size_bytes != kBlockSizeBytes ||
        geometry.backing_kind != "zero") {
        return provider_config_error(
            L"real M2 requires exact 64 GiB/512-byte/64-KiB zero geometry");
    }
    if (backing.initial_state != M2InitialState::empty ||
        backing.initial_backing_generation != 0 ||
        backing.current_backing_generation != 0 ||
        backing.durable_successor_installed ||
        backing.durable_successor_generation_present ||
        backing.durable_successor_generation != 0) {
        return provider_config_error(
            L"real M2 requires empty generation-zero non-durable backing state");
    }
    if (geometry.max_snapshot_batch_records >
            (std::numeric_limits<std::size_t>::max)() ||
        geometry.lock_stripe_count >
            (std::numeric_limits<std::size_t>::max)()) {
        return provider_config_error(L"store count does not fit size_t");
    }

    M2ProviderConfig config;
    config.store.logical_size_bytes = geometry.disk_size_bytes;
    config.store.sector_size_bytes =
        static_cast<std::uint32_t>(geometry.sector_size_bytes);
    config.store.options.soft_limit_bytes = geometry.soft_limit_bytes;
    config.store.options.hard_limit_bytes = geometry.hard_limit_bytes;
    config.store.options.eviction_low_watermark_bytes =
        geometry.eviction_low_watermark_bytes;
    config.store.options.max_snapshot_batch_bytes =
        geometry.max_snapshot_batch_bytes;
    config.store.options.max_snapshot_batch_records =
        static_cast<std::size_t>(geometry.max_snapshot_batch_records);
    config.store.options.lock_stripe_count =
        static_cast<std::size_t>(geometry.lock_stripe_count);
    config.store.initial_state = InitialBlockState::CleanZero;
    config.store.initial_backing_generation = 0;
    config.store.backing_store.reset();

    if (!validate_dynamic_block_store_create_args(config.store).ok()) {
        return provider_config_error(L"real M2 store configuration is invalid");
    }
    return Result<M2ProviderConfig>::success(std::move(config));
}

Result<void> warm_m2_provider_runtime_for_acceptance(
    const M2GeometryOptions& geometry,
    const M2BackingState& backing) {
    DWORD previous_handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &previous_handles)) {
        return Result<void>::failure(win32_error(
            L"GetProcessHandleCount(m2-provider-warmup)",
            L"failed to sample provider runtime handles"));
    }
    constexpr std::size_t kMaxWarmupCycles = 8;
    for (std::size_t cycle = 0; cycle < kMaxWarmupCycles; ++cycle) {
        auto config = make_m2_real_provider_config(geometry, backing);
        if (!config) return Result<void>::failure(config.error());
        auto provider = M2Provider::create(config.move_value());
        if (!provider) return Result<void>::failure(provider.error());
        auto started = provider.value()->start();
        if (!started) return started;
        auto stopped = provider.value()->stop();
        provider.value().reset();
        if (!stopped) return stopped;

        DWORD current_handles = 0;
        if (!GetProcessHandleCount(GetCurrentProcess(), &current_handles)) {
            return Result<void>::failure(win32_error(
                L"GetProcessHandleCount(m2-provider-warmup)",
                L"failed to verify provider runtime handle stability"));
        }
        if (current_handles == previous_handles) {
            return Result<void>::success();
        }
        previous_handles = current_handles;
    }
    return Result<void>::failure(acceptance_error(
        L"warm_m2_provider_runtime_for_acceptance",
        L"provider runtime handle count did not stabilize"));
}

Result<M2RealRun> finalize_m2_windows_acceptance(
    M2Report report,
    std::filesystem::path report_path) {
    std::error_code file_error;
    const bool report_exists =
        std::filesystem::is_regular_file(report_path, file_error) &&
        !file_error;
    if (!report_exists && !report.failures.empty()) {
        const auto& original = report.failures.front();
        return Result<M2RealRun>::failure({
            ErrorDomain::evidence,
            static_cast<std::uint32_t>(original.code),
            widen_utf8(original.phase),
            widen_utf8(original.message)});
    }
    auto readback = validate_published_m2_report(report_path);
    if (!readback) {
        return Result<M2RealRun>::failure(readback.error());
    }
    return Result<M2RealRun>::success(
        {std::move(report), std::move(report_path)});
}

namespace {

struct M2RealSharedState {
    std::vector<std::uint32_t> physical_disk_baseline;
    std::optional<aim::DeviceId> owned_device;
    std::optional<aim::LocatedDisk> located_disk;
    std::string provider_session_id;
};

class WindowsAimSession final {
public:
    Result<void> ensure(const OperationContext& context) {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        if (adapter_) return Result<void>::success();
        auto discovered = aim::discover_aim_adapter();
        if (!discovered) return Result<void>::failure(discovered.error());
        handle_ = discovered.move_value();
        transport_ = std::make_unique<aim::WindowsSrbTransport>(
            handle_->handle.get());
        adapter_ = std::make_unique<aim::DirectAimAdapter>(*transport_);
        auto version = adapter_->query_version();
        if (!version) {
            adapter_.reset();
            transport_.reset();
            handle_.reset();
            return Result<void>::failure(version.error());
        }
        return check_operation_context(context);
    }

    aim::DirectAimAdapter& adapter() { return *adapter_; }
    std::uint32_t dev_inst() const { return handle_->dev_inst; }

private:
    std::optional<aim::AdapterHandle> handle_;
    std::unique_ptr<aim::WindowsSrbTransport> transport_;
    std::unique_ptr<aim::DirectAimAdapter> adapter_;
};

Result<std::string> session_id_from_names(
    const proxy::ProxyObjectNames& names) {
    constexpr std::wstring_view prefix = L"Global\\myrd-m1-";
    if (!std::wstring_view(names.mapping).starts_with(prefix)) {
        return Result<std::string>::failure(acceptance_error(
            L"session_id_from_names",
            L"provider mapping does not use the accepted kernel namespace"));
    }
    const auto session = std::wstring_view(names.mapping).substr(prefix.size());
    auto valid = proxy::validate_session_id(session);
    if (!valid) return Result<std::string>::failure(valid.error());
    const auto utf8 = narrow_utf8(session);
    if (utf8.empty()) {
        return Result<std::string>::failure(acceptance_error(
            L"session_id_from_names", L"failed to encode provider session id"));
    }
    return Result<std::string>::success(utf8);
}

class WindowsM2LifecycleProvider final : public IM2LifecycleProvider {
public:
    WindowsM2LifecycleProvider(
        std::unique_ptr<M2Provider> provider,
        std::string session_id)
        : provider_(std::move(provider)),
          session_id_(std::move(session_id)) {}

    Result<void> start(const OperationContext& context) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto started = provider_->start();
        if (!started) return started;
        return check_operation_context(context);
    }

    Result<void> stop(const OperationContext& context) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        auto stopped = provider_->stop();
        if (!stopped) return stopped;
        return check_operation_context(context);
    }

    Result<M2ProviderSample> sample(
        const OperationContext& context,
        M2StoreSamplePoint) const override {
        auto checked = check_operation_context(context);
        if (!checked) {
            return Result<M2ProviderSample>::failure(checked.error());
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                sizeof(memory))) {
            return Result<M2ProviderSample>::failure(win32_error(
                L"GetProcessMemoryInfo(m2-provider)",
                L"failed to sample provider process memory"));
        }
        M2ProviderSample result;
        result.store_stats = provider_->storage_stats();
        result.provider_process_id = GetCurrentProcessId();
        result.working_set_bytes = memory.WorkingSetSize;
        result.private_bytes = memory.PrivateUsage;
        result.commit_bytes = memory.PagefileUsage;
        return Result<M2ProviderSample>::success(std::move(result));
    }

    std::string session_id() const override { return session_id_; }
    std::wstring native_object_path() const override {
        return provider_->native_object_path();
    }

private:
    std::unique_ptr<M2Provider> provider_;
    std::string session_id_;
};

class WindowsM2ProviderFactory final : public IM2LifecycleProviderFactory {
public:
    explicit WindowsM2ProviderFactory(M2RealSharedState& shared)
        : shared_(shared) {}

    Result<std::unique_ptr<IM2LifecycleProvider>> create(
        const OperationContext& context,
        const M2GeometryOptions& geometry,
        const M2BackingState& backing) override {
        auto checked = check_operation_context(context);
        if (!checked) {
            return Result<std::unique_ptr<IM2LifecycleProvider>>::failure(
                checked.error());
        }
        auto config = make_m2_real_provider_config(geometry, backing);
        if (!config) {
            return Result<std::unique_ptr<IM2LifecycleProvider>>::failure(
                config.error());
        }
        auto provider = M2Provider::create(config.move_value());
        if (!provider) {
            return Result<std::unique_ptr<IM2LifecycleProvider>>::failure(
                provider.error());
        }
        auto session = session_id_from_names(provider.value()->names());
        if (!session) {
            return Result<std::unique_ptr<IM2LifecycleProvider>>::failure(
                session.error());
        }
        shared_.provider_session_id = session.value();
        std::unique_ptr<IM2LifecycleProvider> lifecycle =
            std::make_unique<WindowsM2LifecycleProvider>(
                provider.move_value(), session.move_value());
        return Result<std::unique_ptr<IM2LifecycleProvider>>::success(
            std::move(lifecycle));
    }

private:
    M2RealSharedState& shared_;
};

struct PhysicalDiskDescriptor {
    std::uint32_t raw_bus_type{};
    std::string raw_bus_type_name;
    std::string vendor_id;
    std::string product_id;
    std::string serial_number;
};

std::string descriptor_string(
    std::span<const std::byte> bytes,
    DWORD offset) {
    if (offset == 0 || offset >= bytes.size()) return {};
    const auto* start = reinterpret_cast<const char*>(bytes.data() + offset);
    const auto available = bytes.size() - offset;
    std::size_t length = 0;
    while (length < available && start[length] != '\0') ++length;
    std::string value(start, length);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    value.erase(value.begin(), first);
    return value;
}

std::string bus_type_name(STORAGE_BUS_TYPE type) {
    switch (type) {
    case BusTypeScsi: return "SCSI";
    case BusTypeFibre: return "Fibre Channel";
    case BusTypeSas: return "SAS";
    case BusTypeSata: return "SATA";
    case BusTypeNvme: return "NVMe";
    case BusTypeVirtual: return "Virtual";
    default: return std::to_string(static_cast<unsigned>(type));
    }
}

Result<PhysicalDiskDescriptor> query_physical_disk_descriptor(
    const aim::LocatedDisk& disk) {
    UniqueHandle handle(CreateFileW(
        disk.path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
        return Result<PhysicalDiskDescriptor>::failure(win32_error(
            L"CreateFileW(PhysicalDrive descriptor)",
            L"failed to open exact created PhysicalDrive"));
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD returned = 0;
    if (!DeviceIoControl(
            handle.get(),
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            &header,
            sizeof(header),
            &returned,
            nullptr) ||
        header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
        header.Size > (1u << 20)) {
        return Result<PhysicalDiskDescriptor>::failure(win32_error(
            L"IOCTL_STORAGE_QUERY_PROPERTY(size)",
            L"failed to size exact disk descriptor"));
    }
    std::vector<std::byte> bytes(header.Size);
    if (!DeviceIoControl(
            handle.get(),
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &returned,
            nullptr) ||
        returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return Result<PhysicalDiskDescriptor>::failure(win32_error(
            L"IOCTL_STORAGE_QUERY_PROPERTY(data)",
            L"failed to read exact disk descriptor"));
    }
    bytes.resize(returned);
    const auto* descriptor =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(bytes.data());
    PhysicalDiskDescriptor result;
    result.raw_bus_type = static_cast<std::uint32_t>(descriptor->BusType);
    result.raw_bus_type_name = bus_type_name(descriptor->BusType);
    result.vendor_id = descriptor_string(bytes, descriptor->VendorIdOffset);
    result.product_id = descriptor_string(bytes, descriptor->ProductIdOffset);
    result.serial_number =
        descriptor_string(bytes, descriptor->SerialNumberOffset);
    if (result.raw_bus_type_name.empty() || result.vendor_id.empty() ||
        result.product_id.empty()) {
        return Result<PhysicalDiskDescriptor>::failure(acceptance_error(
            L"query_physical_disk_descriptor",
            L"created disk descriptor lacks bus/vendor/product identity"));
    }
    return Result<PhysicalDiskDescriptor>::success(std::move(result));
}

class WindowsM2DeviceControl final : public IM2DeviceControl {
public:
    WindowsM2DeviceControl(
        WindowsAimSession& aim_session,
        M2RealSharedState& shared,
        std::string expected_bus_type,
        std::string expected_vendor_id,
        std::string expected_product_id)
        : aim_(aim_session),
          shared_(shared),
          expected_bus_type_(std::move(expected_bus_type)),
          expected_vendor_id_(std::move(expected_vendor_id)),
          expected_product_id_(std::move(expected_product_id)) {}

    Result<M2OwnedDevice> create(
        const OperationContext& context,
        std::wstring_view native_object_path,
        const M2GeometryOptions& geometry) override {
        auto initialized = aim_.ensure(context);
        if (!initialized) {
            return Result<M2OwnedDevice>::failure(initialized.error());
        }
        auto baseline = aim::snapshot_physical_drive_numbers();
        if (!baseline) {
            return Result<M2OwnedDevice>::failure(baseline.error());
        }
        shared_.physical_disk_baseline = baseline.move_value();

        aim::ProxyDiskSpec spec{
            geometry.disk_size_bytes,
            static_cast<std::uint32_t>(geometry.sector_size_bytes),
            std::wstring(native_object_path)};
        auto created = aim_.adapter().create_proxy_disk(spec);
        if (!created) {
            return Result<M2OwnedDevice>::failure(created.error());
        }

        try {
            const auto address = aim::decode_device_address(created.value());
            M2DeviceIdentity identity;
            identity.provider_session_id = shared_.provider_session_id;
            identity.native_object_path = narrow_utf8(native_object_path);
            identity.device_id_raw = std::to_string(created.value().value);
            identity.path_id = address.path_id;
            identity.target_id = address.target_id;
            identity.lun = address.lun;
            identity.create_flags = aim::kM0CreateFlags;
            shared_.owned_device = created.value();
            return Result<M2OwnedDevice>::success(
                {created.value(), std::move(identity)});
        } catch (...) {
            (void)aim_.adapter().remove_device(created.value());
            return Result<M2OwnedDevice>::failure(acceptance_error(
                L"WindowsM2DeviceControl::create",
                L"failed to retain exact created-device ownership"));
        }
    }

    Result<M2DeviceIdentity> wait_for_disk(
        const OperationContext& context,
        const M2OwnedDevice& owned) override {
        if (!shared_.owned_device ||
            *shared_.owned_device != owned.id) {
            return Result<M2DeviceIdentity>::failure(acceptance_error(
                L"WindowsM2DeviceControl::wait_for_disk",
                L"owned DeviceId changed before discovery"));
        }
        auto checked = check_operation_context(context);
        if (!checked) {
            return Result<M2DeviceIdentity>::failure(checked.error());
        }
        auto rescanned = aim::rescan_aim_adapter(aim_.dev_inst());
        if (!rescanned) {
            return Result<M2DeviceIdentity>::failure(rescanned.error());
        }
        auto query = aim_.adapter().query_device(owned.id);
        if (!query) {
            return Result<M2DeviceIdentity>::failure(query.error());
        }
        if (query.value().size_bytes != (64ull << 30) ||
            query.value().sector_size != 512 ||
            query.value().flags != aim::kM1QueryFlags ||
            query.value().native_object_path !=
                widen_utf8(owned.identity.native_object_path)) {
            return Result<M2DeviceIdentity>::failure(acceptance_error(
                L"QUERY_DEVICE(m2)",
                L"created device does not match exact M2 proxy contract"));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= context.deadline) {
            return Result<M2DeviceIdentity>::failure(
                check_operation_context(context).error());
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(context.deadline - now);
        auto located = aim::wait_for_created_physical_drive(
            owned.id,
            shared_.physical_disk_baseline,
            64ull << 30,
            remaining);
        if (!located) {
            return Result<M2DeviceIdentity>::failure(located.error());
        }
        auto descriptor = query_physical_disk_descriptor(located.value());
        if (!descriptor) {
            return Result<M2DeviceIdentity>::failure(descriptor.error());
        }
        auto normalized_bus = normalize_m2_aim_descriptor_bus_type(
            descriptor.value().raw_bus_type,
            descriptor.value().vendor_id,
            descriptor.value().product_id);
        if (!normalized_bus ||
            !equals_ci(normalized_bus.value(), expected_bus_type_) ||
            !equals_ci(descriptor.value().vendor_id, expected_vendor_id_) ||
            !equals_ci(descriptor.value().product_id, expected_product_id_)) {
            std::wostringstream details;
            details << L"expected bus='" << widen_utf8(expected_bus_type_)
                    << L"' vendor='" << widen_utf8(expected_vendor_id_)
                    << L"' product='" << widen_utf8(expected_product_id_)
                    << L"'; actual raw bus='"
                    << widen_utf8(descriptor.value().raw_bus_type_name)
                    << L"(" << descriptor.value().raw_bus_type << L")'"
                    << L" normalized bus='"
                    << (normalized_bus
                            ? widen_utf8(normalized_bus.value())
                            : std::wstring(L"<rejected>"))
                    << L"' vendor='"
                    << widen_utf8(descriptor.value().vendor_id)
                    << L"' product='"
                    << widen_utf8(descriptor.value().product_id) << L"'";
            return Result<M2DeviceIdentity>::failure(acceptance_error(
                L"WindowsM2DeviceControl::wait_for_disk",
                details.str().c_str()));
        }

        shared_.located_disk = located.value();
        auto identity = owned.identity;
        identity.query_flags = query.value().flags;
        identity.confirmed_by_query_device = true;
        identity.physical_drive_number = located.value().physical_drive_number;
        identity.physical_drive_path = narrow_utf8(located.value().path);
        identity.disk_length_bytes = located.value().length_bytes;
        identity.bus_type = normalized_bus.move_value();
        identity.vendor_id = descriptor.value().vendor_id;
        identity.product_id = descriptor.value().product_id;
        identity.serial_number = descriptor.value().serial_number;
        return Result<M2DeviceIdentity>::success(std::move(identity));
    }

    Result<void> remove(
        const OperationContext& context,
        aim::DeviceId id) override {
        if (!shared_.owned_device || *shared_.owned_device != id ||
            id.value == aim::kAllDevices) {
            return Result<void>::failure(acceptance_error(
                L"WindowsM2DeviceControl::remove",
                L"refusing removal without exact owned DeviceId"));
        }
        auto removed = aim_.adapter().remove_device(id);
        if (!removed) return removed;
        (void)aim::rescan_aim_adapter(aim_.dev_inst());
        const auto physical_number = shared_.located_disk
            ? std::optional<std::uint32_t>(
                  shared_.located_disk->physical_drive_number)
            : std::nullopt;
        for (;;) {
            auto devices = aim_.adapter().list_devices();
            if (!devices) return Result<void>::failure(devices.error());
            const auto found = std::find(
                devices.value().begin(), devices.value().end(), id);
            auto physical = aim::snapshot_physical_drive_numbers();
            if (!physical) return Result<void>::failure(physical.error());
            const bool physical_absent = !physical_number ||
                std::find(
                    physical.value().begin(),
                    physical.value().end(),
                    *physical_number) == physical.value().end();
            if (found == devices.value().end() && physical_absent) {
                shared_.owned_device.reset();
                shared_.located_disk.reset();
                return Result<void>::success();
            }
            auto checked = check_operation_context(context);
            if (!checked) return checked;
            std::this_thread::sleep_for(100ms);
        }
    }

private:
    WindowsAimSession& aim_;
    M2RealSharedState& shared_;
    std::string expected_bus_type_;
    std::string expected_vendor_id_;
    std::string expected_product_id_;
};

bool current_user_is_admin() {
    BOOL is_admin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &administrators)) {
        (void)CheckTokenMembership(nullptr, administrators, &is_admin);
        FreeSid(administrators);
    }
    return is_admin != FALSE;
}

bool equals_ci(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (towlower(left[index]) != towlower(right[index])) return false;
    }
    return true;
}

std::string service_state_name(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_START_PENDING: return "start_pending";
    case SERVICE_STOP_PENDING: return "stop_pending";
    case SERVICE_RUNNING: return "running";
    case SERVICE_CONTINUE_PENDING: return "continue_pending";
    case SERVICE_PAUSE_PENDING: return "pause_pending";
    case SERVICE_PAUSED: return "paused";
    default: return std::to_string(state);
    }
}

std::string service_start_name(DWORD start_type) {
    switch (start_type) {
    case SERVICE_BOOT_START: return "boot";
    case SERVICE_SYSTEM_START: return "system";
    case SERVICE_AUTO_START: return "automatic";
    case SERVICE_DEMAND_START: return "manual";
    case SERVICE_DISABLED: return "disabled";
    default: return std::to_string(start_type);
    }
}

std::filesystem::path normalize_service_binary_path(std::wstring path) {
    while (!path.empty() && std::iswspace(path.front())) path.erase(path.begin());
    if (!path.empty() && path.front() == L'"') {
        const auto closing = path.find(L'"', 1);
        if (closing != std::wstring::npos) {
            path = path.substr(1, closing - 1);
        }
    } else {
        const auto separator = path.find(L".sys ");
        if (separator != std::wstring::npos) {
            path.resize(separator + 4);
        }
    }
    constexpr std::wstring_view system_root = L"\\SystemRoot\\";
    if (std::wstring_view(path).starts_with(system_root)) {
        wchar_t windows[MAX_PATH]{};
        const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            path = std::wstring(windows, length) + L"\\" +
                path.substr(system_root.size());
        }
    }
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path(path) : canonical;
}

std::string file_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(
            bytes.data(),
            L"\\",
            reinterpret_cast<void**>(&info),
            &info_size) ||
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

struct NativeServiceState {
    M2DriverServiceState report;
    DWORD current_state{};
    DWORD start_type{};
    std::filesystem::path binary_path;
};

Result<NativeServiceState> query_driver_service(
    std::wstring_view service_name) {
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return Result<NativeServiceState>::failure(win32_error(
            L"OpenSCManagerW(m2)", L"failed to open service manager"));
    }
    struct ServiceHandleCloser {
        void operator()(SC_HANDLE value) const noexcept {
            if (value != nullptr) CloseServiceHandle(value);
        }
    };
    std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleCloser>
        manager_guard(manager);
    const std::wstring name(service_name);
    const auto service = OpenServiceW(
        manager,
        name.c_str(),
        SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG |
            SERVICE_START | SERVICE_STOP);
    if (service == nullptr) {
        return Result<NativeServiceState>::failure(win32_error(
            L"OpenServiceW(m2)", L"failed to open expected driver service"));
    }
    std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleCloser>
        service_guard(service);

    SERVICE_STATUS_PROCESS status{};
    DWORD required = 0;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &required)) {
        return Result<NativeServiceState>::failure(win32_error(
            L"QueryServiceStatusEx(m2)",
            L"failed to query expected driver service state"));
    }
    QueryServiceConfigW(service, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return Result<NativeServiceState>::failure(win32_error(
            L"QueryServiceConfigW(size,m2)",
            L"failed to size driver service configuration"));
    }
    std::vector<std::byte> bytes(required);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    if (!QueryServiceConfigW(service, config, required, &required) ||
        config->lpBinaryPathName == nullptr) {
        return Result<NativeServiceState>::failure(win32_error(
            L"QueryServiceConfigW(data,m2)",
            L"failed to read driver service configuration"));
    }
    const auto binary = normalize_service_binary_path(config->lpBinaryPathName);
    auto hash = hash_file(binary, L"hash_driver_service_binary");
    if (!hash) {
        return Result<NativeServiceState>::failure(hash.error());
    }

    NativeServiceState result;
    result.current_state = status.dwCurrentState;
    result.start_type = config->dwStartType;
    result.binary_path = binary;
    result.report.driver_state =
        status.dwCurrentState == SERVICE_RUNNING ? "loaded" : "not_loaded";
    result.report.service_name = narrow_utf8(name);
    result.report.service_state = service_state_name(status.dwCurrentState);
    result.report.service_start_type = service_start_name(config->dwStartType);
    result.report.driver_file_path = narrow_utf8(binary.wstring());
    result.report.driver_file_version = file_version(binary);
    result.report.driver_sha256 = hash.move_value();
    if (result.report.service_name.empty() ||
        result.report.driver_file_path.empty() ||
        result.report.driver_file_version.empty()) {
        return Result<NativeServiceState>::failure(acceptance_error(
            L"query_driver_service",
            L"driver service identity evidence is incomplete"));
    }
    return Result<NativeServiceState>::success(std::move(result));
}

Result<std::vector<std::string>> enumerate_volume_names() {
    std::vector<std::string> result;
    std::array<wchar_t, 1024> buffer{};
    const auto first = FindFirstVolumeW(
        buffer.data(), static_cast<DWORD>(buffer.size()));
    if (first == INVALID_HANDLE_VALUE) {
        return Result<std::vector<std::string>>::failure(win32_error(
            L"FindFirstVolumeW(m2)", L"failed to enumerate volumes"));
    }
    struct VolumeFindCloser {
        void operator()(void* value) const noexcept {
            if (value != nullptr && value != INVALID_HANDLE_VALUE) {
                FindVolumeClose(value);
            }
        }
    };
    std::unique_ptr<void, VolumeFindCloser> find_handle(first);
    for (;;) {
        const auto value = narrow_utf8(buffer.data());
        if (value.empty()) {
            return Result<std::vector<std::string>>::failure(acceptance_error(
                L"enumerate_volume_names",
                L"failed to encode a volume identity"));
        }
        result.push_back(value);
        if (FindNextVolumeW(
                find_handle.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()))) {
            continue;
        }
        const auto error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            return Result<std::vector<std::string>>::failure(win32_error(
                L"FindNextVolumeW(m2)",
                L"volume enumeration failed",
                error));
        }
        break;
    }
    std::sort(result.begin(), result.end());
    return Result<std::vector<std::string>>::success(std::move(result));
}

Result<std::vector<std::string>> enumerate_mount_points(
    std::span<const std::string> volumes) {
    std::vector<std::string> result;
    for (const auto& volume : volumes) {
        const auto wide_volume = widen_utf8(volume);
        if (wide_volume.empty()) {
            return Result<std::vector<std::string>>::failure(acceptance_error(
                L"enumerate_mount_points", L"invalid volume identity"));
        }
        DWORD required = 0;
        SetLastError(ERROR_SUCCESS);
        if (GetVolumePathNamesForVolumeNameW(
                wide_volume.c_str(), nullptr, 0, &required) ||
            GetLastError() != ERROR_MORE_DATA || required == 0) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            return Result<std::vector<std::string>>::failure(win32_error(
                L"GetVolumePathNamesForVolumeNameW(size,m2)",
                L"failed to size volume mount paths",
                error));
        }
        std::vector<wchar_t> paths(required);
        if (!GetVolumePathNamesForVolumeNameW(
                wide_volume.c_str(), paths.data(), required, &required)) {
            return Result<std::vector<std::string>>::failure(win32_error(
                L"GetVolumePathNamesForVolumeNameW(data,m2)",
                L"failed to read volume mount paths"));
        }
        for (const wchar_t* current = paths.data(); *current != L'\0';) {
            auto encoded = narrow_utf8(current);
            if (encoded.empty()) {
                return Result<std::vector<std::string>>::failure(
                    acceptance_error(
                        L"enumerate_mount_points",
                        L"failed to encode volume mount path"));
            }
            result.push_back(std::move(encoded));
            current += wcslen(current) + 1;
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return Result<std::vector<std::string>>::success(std::move(result));
}

Result<std::uint64_t> current_process_thread_count() {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) {
        return Result<std::uint64_t>::failure(win32_error(
            L"CreateToolhelp32Snapshot(threads,m2)",
            L"failed to enumerate process threads"));
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint64_t count = 0;
    if (!Thread32First(snapshot.get(), &entry)) {
        return Result<std::uint64_t>::failure(win32_error(
            L"Thread32First(m2)", L"failed to start thread enumeration"));
    }
    do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
    } while (Thread32Next(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        return Result<std::uint64_t>::failure(win32_error(
            L"Thread32Next(m2)", L"thread enumeration failed"));
    }
    return Result<std::uint64_t>::success(count);
}

Result<M2InventorySnapshot> capture_inventory(
    WindowsAimSession& aim_session,
    const OperationContext& context,
    const NativeServiceState& service) {
    auto checked = aim_session.ensure(context);
    if (!checked) {
        return Result<M2InventorySnapshot>::failure(checked.error());
    }
    auto physical = aim::snapshot_physical_drive_numbers();
    if (!physical) {
        return Result<M2InventorySnapshot>::failure(physical.error());
    }
    auto volumes = enumerate_volume_names();
    if (!volumes) {
        return Result<M2InventorySnapshot>::failure(volumes.error());
    }
    auto mounts = enumerate_mount_points(volumes.value());
    if (!mounts) {
        return Result<M2InventorySnapshot>::failure(mounts.error());
    }
    auto devices = aim_session.adapter().list_devices();
    if (!devices) {
        return Result<M2InventorySnapshot>::failure(devices.error());
    }
    DWORD handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        return Result<M2InventorySnapshot>::failure(win32_error(
            L"GetProcessHandleCount(m2)",
            L"failed to sample process handles"));
    }
    auto threads = current_process_thread_count();
    if (!threads) {
        return Result<M2InventorySnapshot>::failure(threads.error());
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        return Result<M2InventorySnapshot>::failure(win32_error(
            L"GetProcessMemoryInfo(inventory,m2)",
            L"failed to sample process private bytes"));
    }

    M2InventorySnapshot result;
    for (const auto number : physical.value()) {
        result.physical_disks.push_back(std::to_string(number));
    }
    std::sort(result.physical_disks.begin(), result.physical_disks.end());
    result.volumes = volumes.move_value();
    result.mount_points = mounts.move_value();
    for (const auto id : devices.value()) {
        result.m2_device_ids.push_back(std::to_string(id.value));
    }
    std::sort(result.m2_device_ids.begin(), result.m2_device_ids.end());
    result.service_state = service.report.service_state;
    result.service_start_type = service.report.service_start_type;
    result.process_handle_count = handles;
    result.process_thread_count = threads.value();
    result.process_private_bytes = memory.PrivateUsage;
    return Result<M2InventorySnapshot>::success(std::move(result));
}

std::string inventory_fingerprint(const M2InventorySnapshot& value) {
    std::ostringstream stream;
    const auto append = [&stream](
        std::string_view name,
        const std::vector<std::string>& values) {
        stream << name << '=';
        for (const auto& item : values) stream << item << '\0';
        stream << '\n';
    };
    append("physical", value.physical_disks);
    append("volumes", value.volumes);
    append("mounts", value.mount_points);
    append("devices", value.m2_device_ids);
    stream << "service=" << value.service_state << '\0'
           << value.service_start_type;
    return hash_text(stream.str());
}

bool same_structural_inventory(
    const M2InventorySnapshot& left,
    const M2InventorySnapshot& right) {
    return left.physical_disks == right.physical_disks &&
        left.volumes == right.volumes &&
        left.mount_points == right.mount_points &&
        left.m2_device_ids == right.m2_device_ids &&
        left.service_state == right.service_state &&
        left.service_start_type == right.service_start_type;
}

void append_set_diff(
    std::string_view label,
    std::span<const std::string> before,
    std::span<const std::string> after,
    M2InventoryDiff& diff) {
    for (const auto& value : after) {
        if (std::find(before.begin(), before.end(), value) == before.end()) {
            diff.added.push_back(
                std::string(label) + ":" + value);
        }
    }
    for (const auto& value : before) {
        if (std::find(after.begin(), after.end(), value) == after.end()) {
            diff.removed.push_back(
                std::string(label) + ":" + value);
        }
    }
}

M2InventoryDiff inventory_diff(
    const M2InventorySnapshot& before,
    const M2InventorySnapshot& after) {
    M2InventoryDiff diff;
    append_set_diff("physical", before.physical_disks,
                    after.physical_disks, diff);
    append_set_diff("volume", before.volumes, after.volumes, diff);
    append_set_diff("mount", before.mount_points,
                    after.mount_points, diff);
    append_set_diff("device", before.m2_device_ids,
                    after.m2_device_ids, diff);
    if (before.service_state != after.service_state) {
        diff.changed.push_back("service_state");
    }
    if (before.service_start_type != after.service_start_type) {
        diff.changed.push_back("service_start_type");
    }
    return diff;
}

Result<void> require_no_forbidden_aim_dependencies() {
    constexpr std::array<std::wstring_view, 2> modules{
        L"aimapi.dll", L"devio.dll"};
    auto loaded = snapshot_current_modules();
    if (!loaded) return Result<void>::failure(loaded.error());
    for (const auto& module : loaded.value()) {
        for (const auto forbidden : modules) {
            if (equals_ci(module.basename, forbidden)) {
                return Result<void>::failure(acceptance_error(
                    L"require_no_forbidden_aim_dependencies",
                    L"forbidden AIM user-mode module is loaded"));
            }
        }
    }
    constexpr std::array<std::wstring_view, 9> process_names{
        L"aim_ll.exe", L"RamDyn.exe", L"AIMtk.exe", L"AIMtk-Dlg.exe",
        L"AIMtk-svc.exe", L"aim_cli.exe", L"MountImg.exe",
        L"RamDiskUI.exe", L"deviosvc.exe"};
    auto processes = snapshot_processes();
    if (!processes) return Result<void>::failure(processes.error());
    for (const auto& process : processes.value()) {
        for (const auto forbidden : process_names) {
            if (equals_ci(process.name, forbidden)) {
                return Result<void>::failure(acceptance_error(
                    L"require_no_forbidden_aim_dependencies",
                    L"forbidden AIM helper process is running"));
            }
        }
    }
    return Result<void>::success();
}

class WindowsM2BaselineControl final : public IM2BaselineControl {
public:
    WindowsM2BaselineControl(
        WindowsAimSession& aim_session,
        std::string expected_service,
        std::string expected_sha256,
        M2GeometryOptions geometry,
        M2BackingState backing)
        : aim_(aim_session),
          expected_service_(std::move(expected_service)),
          expected_sha256_(std::move(expected_sha256)),
          geometry_(std::move(geometry)),
          backing_(std::move(backing)) {}

    ~WindowsM2BaselineControl() override {
        if (owns_mutex_ && mutex_) ReleaseMutex(mutex_.get());
    }

    Result<M2BaselineSnapshot> capture(
        const OperationContext& context) override {
        if (!current_user_is_admin()) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WindowsM2BaselineControl::capture",
                L"real M2 acceptance requires an elevated administrator",
                ERROR_ELEVATION_REQUIRED));
        }
        mutex_ = UniqueHandle(CreateMutexW(
            nullptr, FALSE, L"Global\\myrd-m2-e2e-mutex"));
        if (!mutex_) {
            return Result<M2BaselineSnapshot>::failure(win32_error(
                L"CreateMutexW(m2-e2e)",
                L"failed to create global M2 lifecycle mutex"));
        }
        const auto wait = WaitForSingleObject(mutex_.get(), 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WaitForSingleObject(m2-e2e)",
                L"another M2 real lifecycle owns the global mutex",
                wait));
        }
        owns_mutex_ = true;
        auto dependency = require_no_forbidden_aim_dependencies();
        if (!dependency) {
            return Result<M2BaselineSnapshot>::failure(dependency.error());
        }
        const auto service_name = widen_utf8(expected_service_);
        if (service_name.empty()) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WindowsM2BaselineControl::capture",
                L"expected service name is not valid UTF-8"));
        }
        auto service = query_driver_service(service_name);
        if (!service) {
            return Result<M2BaselineSnapshot>::failure(service.error());
        }
        if (service.value().current_state != SERVICE_RUNNING ||
            !equals_ci(service.value().report.driver_sha256,
                       expected_sha256_) ||
            !equals_ci(service.value().report.service_name,
                       expected_service_)) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WindowsM2BaselineControl::capture",
                L"driver service state/name/hash differs from explicit expectation"));
        }
        auto stabilized_service = query_driver_service(service_name);
        if (!stabilized_service) {
            return Result<M2BaselineSnapshot>::failure(
                stabilized_service.error());
        }
        if (stabilized_service.value().current_state !=
                service.value().current_state ||
            stabilized_service.value().start_type !=
                service.value().start_type ||
            stabilized_service.value().report.driver_sha256 !=
                service.value().report.driver_sha256 ||
            stabilized_service.value().report.service_name !=
                service.value().report.service_name) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WindowsM2BaselineControl::capture",
                L"driver service identity changed while stabilizing baseline collection"));
        }
        auto warmed = warm_m2_provider_runtime_for_acceptance(
            geometry_, backing_);
        if (!warmed) {
            return Result<M2BaselineSnapshot>::failure(warmed.error());
        }
        auto ensured = aim_.ensure(context);
        if (!ensured) {
            return Result<M2BaselineSnapshot>::failure(ensured.error());
        }
        auto rescanned = aim::rescan_aim_adapter(aim_.dev_inst());
        if (!rescanned) {
            return Result<M2BaselineSnapshot>::failure(rescanned.error());
        }
        auto inventory = capture_inventory(aim_, context, service.value());
        if (!inventory) {
            return Result<M2BaselineSnapshot>::failure(inventory.error());
        }
        if (!inventory.value().m2_device_ids.empty()) {
            return Result<M2BaselineSnapshot>::failure(acceptance_error(
                L"WindowsM2BaselineControl::capture",
                L"pre-existing AIM devices make the real M2 baseline ambiguous"));
        }
        native_before_ = service.value();
        M2BaselineSnapshot result;
        result.driver_service = service.value().report;
        result.inventory = inventory.move_value();
        result.fingerprint = inventory_fingerprint(result.inventory);
        return Result<M2BaselineSnapshot>::success(std::move(result));
    }

    Result<void> restore_service(
        const OperationContext& context,
        const M2BaselineSnapshot&) override {
        if (!native_before_) {
            return Result<void>::failure(acceptance_error(
                L"WindowsM2BaselineControl::restore_service",
                L"native service baseline is unavailable"));
        }
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        const auto service_name = widen_utf8(expected_service_);
        auto current = query_driver_service(service_name);
        if (!current) return Result<void>::failure(current.error());
        if (!m2_service_restoration_required(
                current.value().current_state,
                current.value().start_type,
                native_before_->current_state,
                native_before_->start_type)) {
            return Result<void>::success();
        }
        const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr) {
            return Result<void>::failure(win32_error(
                L"OpenSCManagerW(restore,m2)",
                L"failed to open service manager for restoration"));
        }
        const auto service = OpenServiceW(
            manager,
            service_name.c_str(),
            SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG |
                SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP);
        if (service == nullptr) {
            const auto error = win32_error(
                L"OpenServiceW(restore,m2)",
                L"failed to open exact service for restoration");
            CloseServiceHandle(manager);
            return Result<void>::failure(error);
        }
        auto close_handles = [&]() {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
        };
        if (current.value().start_type != native_before_->start_type &&
            !ChangeServiceConfigW(
                service,
                SERVICE_NO_CHANGE,
                native_before_->start_type,
                SERVICE_NO_CHANGE,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr)) {
            const auto error = win32_error(
                L"ChangeServiceConfigW(restore,m2)",
                L"failed to restore driver service start type");
            close_handles();
            return Result<void>::failure(error);
        }
        if (native_before_->current_state == SERVICE_RUNNING &&
            current.value().current_state != SERVICE_RUNNING) {
            if (!StartServiceW(service, 0, nullptr) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                const auto error = win32_error(
                    L"StartServiceW(restore,m2)",
                    L"failed to restore running driver service");
                close_handles();
                return Result<void>::failure(error);
            }
        } else if (native_before_->current_state == SERVICE_STOPPED &&
                   current.value().current_state != SERVICE_STOPPED) {
            SERVICE_STATUS status{};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &status) &&
                GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                const auto error = win32_error(
                    L"ControlService(restore,m2)",
                    L"failed to restore stopped driver service");
                close_handles();
                return Result<void>::failure(error);
            }
        }
        close_handles();
        for (;;) {
            auto restored = query_driver_service(service_name);
            if (!restored) return Result<void>::failure(restored.error());
            if (restored.value().current_state == native_before_->current_state &&
                restored.value().start_type == native_before_->start_type) {
                return Result<void>::success();
            }
            checked = check_operation_context(context);
            if (!checked) return checked;
            std::this_thread::sleep_for(100ms);
        }
    }

    Result<M2BaselineComparisonEvidence> compare(
        const OperationContext& context,
        const M2BaselineSnapshot& before) override {
        const auto service_name = widen_utf8(expected_service_);
        auto service = query_driver_service(service_name);
        if (!service) {
            return Result<M2BaselineComparisonEvidence>::failure(
                service.error());
        }
        auto after = capture_inventory(aim_, context, service.value());
        if (!after) {
            return Result<M2BaselineComparisonEvidence>::failure(
                after.error());
        }
        M2FinalBaselineComparison comparison;
        comparison.before = before.inventory;
        comparison.after = after.value();
        comparison.before_fingerprint = before.fingerprint;
        comparison.after_fingerprint = inventory_fingerprint(after.value());
        comparison.diff = inventory_diff(before.inventory, after.value());
        comparison.exact_match = same_structural_inventory(
            before.inventory, after.value());
        constexpr std::uint64_t private_tolerance = 1ull << 20;
        comparison.private_bytes_tolerance = private_tolerance;
        comparison.resources_within_tolerance =
            m2_process_resources_within_tolerance(
                before.inventory, after.value(), private_tolerance);
        comparison.comparison =
            comparison.exact_match && comparison.resources_within_tolerance &&
                comparison.before_fingerprint == comparison.after_fingerprint
                ? M2BaselineComparison::match
                : M2BaselineComparison::mismatch;
        return Result<M2BaselineComparisonEvidence>::success(
            {service.value().report, std::move(comparison)});
    }

private:
    WindowsAimSession& aim_;
    std::string expected_service_;
    std::string expected_sha256_;
    M2GeometryOptions geometry_;
    M2BackingState backing_;
    UniqueHandle mutex_;
    bool owns_mutex_{};
    std::optional<NativeServiceState> native_before_;
};

class WindowsRawReadIo final : public IM2RawIo {
public:
    explicit WindowsRawReadIo(const aim::LocatedDisk& disk)
        : size_bytes_(disk.length_bytes) {
        handle_ = UniqueHandle(CreateFileW(
            disk.path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                FILE_FLAG_WRITE_THROUGH,
            nullptr));
        scratch_ = static_cast<std::byte*>(
            VirtualAlloc(nullptr, 1ull << 20, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE));
    }

    ~WindowsRawReadIo() override {
        if (scratch_ != nullptr) VirtualFree(scratch_, 0, MEM_RELEASE);
    }

    bool valid() const noexcept {
        return handle_ && scratch_ != nullptr;
    }

    std::uint64_t size_bytes() const noexcept override { return size_bytes_; }

    Result<std::size_t> transfer(
        const OperationContext& context,
        M2RawOperation operation,
        std::uint64_t offset,
        std::span<std::byte> buffer) override {
        if (operation != M2RawOperation::Read) {
            return Result<std::size_t>::failure(acceptance_error(
                L"WindowsRawReadIo::transfer",
                L"performance workload is read-only while NTFS is mounted"));
        }
        if (buffer.empty() || buffer.size() > (1ull << 20) ||
            (offset % 4096) != 0 || (buffer.size() % 4096) != 0 ||
            offset > size_bytes_ || buffer.size() > size_bytes_ - offset) {
            return Result<std::size_t>::failure(acceptance_error(
                L"WindowsRawReadIo::transfer",
                L"unaligned or out-of-range raw read"));
        }
        auto checked = check_operation_context(context);
        if (!checked) {
            return Result<std::size_t>::failure(checked.error());
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN)) {
            return Result<std::size_t>::failure(win32_error(
                L"SetFilePointerEx(performance)",
                L"failed to position exact raw disk"));
        }
        DWORD transferred = 0;
        if (!ReadFile(
                handle_.get(),
                scratch_,
                static_cast<DWORD>(buffer.size()),
                &transferred,
                nullptr) ||
            transferred != buffer.size()) {
            return Result<std::size_t>::failure(win32_error(
                L"ReadFile(performance)",
                L"exact raw disk read did not complete"));
        }
        std::memcpy(buffer.data(), scratch_, buffer.size());
        return Result<std::size_t>::success(transferred);
    }

private:
    UniqueHandle handle_;
    std::byte* scratch_{};
    std::uint64_t size_bytes_{};
};

class WindowsPerformanceClock final : public IM2PerformanceClock {
public:
    std::uint64_t frequency_hz() const noexcept override {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value)
            ? static_cast<std::uint64_t>(value.QuadPart) : 0;
    }

    Result<std::uint64_t> now_ticks() override {
        LARGE_INTEGER value{};
        if (!QueryPerformanceCounter(&value)) {
            return Result<std::uint64_t>::failure(win32_error(
                L"QueryPerformanceCounter(m2)",
                L"failed to sample performance clock"));
        }
        return Result<std::uint64_t>::success(
            static_cast<std::uint64_t>(value.QuadPart));
    }
};

class WindowsM2VolumeWorkload final : public IM2VolumeWorkload {
public:
    WindowsM2VolumeWorkload(
        std::filesystem::path repository_root,
        std::filesystem::path evidence_directory,
        std::string expected_bus_type,
        std::string expected_vendor_id,
        std::string expected_product_id,
        M2RealSharedState& shared,
        WindowsAimSession& aim_session,
        std::string machine_fingerprint,
        std::string build_fingerprint)
        : repository_root_(std::move(repository_root)),
          evidence_directory_(std::move(evidence_directory)),
          expected_bus_type_(std::move(expected_bus_type)),
          expected_vendor_id_(std::move(expected_vendor_id)),
          expected_product_id_(std::move(expected_product_id)),
          shared_(shared),
          aim_(aim_session),
          machine_fingerprint_(std::move(machine_fingerprint)),
          build_fingerprint_(std::move(build_fingerprint)) {}

    Result<M2IoEvidence> setup_and_write(
        const OperationContext& context,
        M2DeviceIdentity& identity,
        const M2MountsAcquired& mounts_acquired) override {
        auto preflight = invoke_script(
            context, "Preflight", identity, false);
        if (!preflight) return Result<M2IoEvidence>::failure(preflight.error());
        // The PowerShell child cannot call back into this process at the exact
        // first mount mutation. Register ownership after the independent
        // preflight and before any destructive SetupWrite command; cleanup is
        // idempotent for every state-file phase.
        mounts_acquired();
        auto setup = invoke_script(context, "SetupWrite", identity, true);
        if (!setup) return Result<M2IoEvidence>::failure(setup.error());
        auto state = load_state();
        if (!state) return Result<M2IoEvidence>::failure(state.error());
        const auto bytes_written = json_uint64(state.value(), "bytes_written");
        const auto bytes_read = json_uint64(state.value(), "bytes_read");
        const auto write_hash = json_string(state.value(), "write_sha256");
        const auto read_hash = json_string(state.value(), "read_sha256");
        const auto volume = json_string(state.value(), "volume_unique_id");
        const auto mount = json_string(state.value(), "mount_directory");
        const auto bus = json_string(state.value(), "bus_type");
        const auto vendor = json_string(state.value(), "vendor_id");
        const auto product = json_string(state.value(), "product_id");
        const auto serial = json_string(state.value(), "serial_number");
        const auto unique = json_string(state.value(), "disk_unique_id");
        if (!bytes_written || !bytes_read || !write_hash || !read_hash ||
            !volume || !mount || !bus || !vendor || !product || !serial ||
            !unique || write_hash->empty() || *write_hash != *read_hash ||
            *bytes_written < (4ull << 30) || *bytes_read < (4ull << 30)) {
            return Result<M2IoEvidence>::failure(acceptance_error(
                L"WindowsM2VolumeWorkload::setup_and_write",
                L"NTFS workload state is incomplete or hash verification failed"));
        }
        identity.volume_guids = {*volume};
        identity.mount_points = {*mount};
        identity.bus_type = *bus;
        identity.vendor_id = *vendor;
        identity.product_id = *product;
        identity.serial_number = *serial;
        identity.disk_unique_id = *unique;
        return Result<M2IoEvidence>::success({
            *write_hash, *read_hash, *bytes_written, *bytes_read, 0});
    }

    Result<std::uint64_t> delete_and_retrim(
        const OperationContext& context,
        M2DeviceIdentity& identity) override {
        auto result = invoke_script(context, "DeleteRetrim", identity, true);
        if (!result) return Result<std::uint64_t>::failure(result.error());
        auto state = load_state();
        if (!state) return Result<std::uint64_t>::failure(state.error());
        return validate_m2_delete_retrim_state(state.value());
    }

    Result<M2PerformanceEvidence> run_performance(
        const OperationContext& context,
        const M2DeviceIdentity& identity) override {
        if (!shared_.located_disk ||
            shared_.located_disk->physical_drive_number !=
                identity.physical_drive_number ||
            shared_.located_disk->length_bytes != identity.disk_length_bytes) {
            return Result<M2PerformanceEvidence>::failure(acceptance_error(
                L"WindowsM2VolumeWorkload::run_performance",
                L"exact dynamic PhysicalDrive is unavailable"));
        }
        auto checked = aim_.ensure(context);
        if (!checked) {
            return Result<M2PerformanceEvidence>::failure(checked.error());
        }
        auto fixed_provider = M1Provider::create({
            64ull << 20, 512, 30s});
        if (!fixed_provider) {
            return Result<M2PerformanceEvidence>::failure(
                fixed_provider.error());
        }
        auto fixed_started = fixed_provider.value()->start();
        if (!fixed_started) {
            return Result<M2PerformanceEvidence>::failure(fixed_started.error());
        }

        std::optional<aim::DeviceId> fixed_owned;
        std::optional<aim::LocatedDisk> fixed_location;
        const auto cleanup_fixed = [&]() -> std::optional<Error> {
            std::optional<Error> first_error;
            if (fixed_owned) {
                auto removed = aim_.adapter().remove_device(*fixed_owned);
                if (!removed) first_error = removed.error();
                (void)aim::rescan_aim_adapter(aim_.dev_inst());
                const auto deadline =
                    std::chrono::steady_clock::now() + 30s;
                while (!first_error &&
                       std::chrono::steady_clock::now() < deadline) {
                    auto devices = aim_.adapter().list_devices();
                    if (!devices) {
                        first_error = devices.error();
                        break;
                    }
                    const bool device_absent = std::find(
                            devices.value().begin(),
                            devices.value().end(),
                            *fixed_owned) == devices.value().end();
                    auto physical = aim::snapshot_physical_drive_numbers();
                    if (!physical) {
                        first_error = physical.error();
                        break;
                    }
                    const bool physical_absent = !fixed_location ||
                        std::find(
                            physical.value().begin(),
                            physical.value().end(),
                            fixed_location->physical_drive_number) ==
                            physical.value().end();
                    if (device_absent && physical_absent) {
                        break;
                    }
                    std::this_thread::sleep_for(100ms);
                }
                if (!first_error &&
                    std::chrono::steady_clock::now() >= deadline) {
                    first_error = acceptance_error(
                        L"cleanup_fixed_performance_disk",
                        L"fixed baseline AIM device remained present");
                }
            }
            auto stopped = fixed_provider.value()->stop();
            if (!stopped && !first_error) first_error = stopped.error();
            return first_error;
        };
        const auto fail_after_cleanup = [&](const Error& original) {
            auto cleanup_error = cleanup_fixed();
            return Result<M2PerformanceEvidence>::failure(
                cleanup_error ? std::move(*cleanup_error) : original);
        };

        std::vector<std::uint32_t> fixed_baseline;
        auto baseline = aim::snapshot_physical_drive_numbers();
        if (!baseline) {
            return fail_after_cleanup(baseline.error());
        }
        fixed_baseline = baseline.move_value();
        auto fixed_id = aim_.adapter().create_proxy_disk({
            64ull << 20, 512, fixed_provider.value()->native_object_path()});
        if (!fixed_id) {
            return fail_after_cleanup(fixed_id.error());
        }
        fixed_owned = fixed_id.value();
        auto fixed_rescan = aim::rescan_aim_adapter(aim_.dev_inst());
        if (!fixed_rescan) {
            return fail_after_cleanup(fixed_rescan.error());
        }
        auto fixed_query = aim_.adapter().query_device(fixed_id.value());
        if (!fixed_query) {
            return fail_after_cleanup(fixed_query.error());
        }
        if (fixed_query.value().size_bytes != (64ull << 20) ||
            fixed_query.value().sector_size != 512 ||
            fixed_query.value().flags != aim::kM1QueryFlags ||
            fixed_query.value().native_object_path !=
                fixed_provider.value()->native_object_path()) {
            return fail_after_cleanup(acceptance_error(
                L"QUERY_DEVICE(fixed performance baseline)",
                L"fixed baseline device differs from exact M1 contract"));
        }
        auto fixed_disk = aim::wait_for_created_physical_drive(
            fixed_id.value(), fixed_baseline, 64ull << 20, 15s);
        if (!fixed_disk) {
            return fail_after_cleanup(fixed_disk.error());
        }
        fixed_location = fixed_disk.value();

        std::optional<M2PerformanceProbeResult> fixed_sample;
        std::optional<M2PerformanceProbeResult> dynamic_sample;
        std::optional<Error> measurement_error;
        {
            WindowsRawReadIo fixed_io(*fixed_location);
            WindowsRawReadIo dynamic_io(*shared_.located_disk);
            if (!fixed_io.valid() || !dynamic_io.valid()) {
                measurement_error = acceptance_error(
                    L"WindowsM2VolumeWorkload::run_performance",
                    L"failed to open exact fixed or dynamic raw disk");
            } else {
                WindowsPerformanceClock clock;
                M2ProbeConfig probe_config;
                probe_config.region_offset = 16ull << 20;
                probe_config.operation = M2RawOperation::Read;
                probe_config.policy = {
                    machine_fingerprint_,
                    build_fingerprint_,
                    "single-thread-normal-priority",
                    "FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH"};
                auto fixed = run_m2_performance_probe(
                    fixed_io, clock, context, probe_config);
                if (!fixed) {
                    measurement_error = fixed.error();
                } else {
                    fixed_sample = fixed.move_value();
                    auto dynamic = run_m2_performance_probe(
                        dynamic_io, clock, context, probe_config);
                    if (!dynamic) {
                        measurement_error = dynamic.error();
                    } else {
                        dynamic_sample = dynamic.move_value();
                    }
                }
            }
        }
        auto cleanup_error = cleanup_fixed();
        if (cleanup_error) {
            return Result<M2PerformanceEvidence>::failure(
                std::move(*cleanup_error));
        }
        if (measurement_error) {
            return Result<M2PerformanceEvidence>::failure(
                std::move(*measurement_error));
        }
        auto comparison = compare_m2_performance(
            *fixed_sample, *dynamic_sample);
        if (!comparison) {
            return Result<M2PerformanceEvidence>::failure(comparison.error());
        }
        M2PerformanceEvidence evidence;
        evidence.fixed = std::move(*fixed_sample);
        evidence.dynamic = std::move(*dynamic_sample);
        evidence.comparison = comparison.move_value();
        evidence.gate_status = evidence.comparison.overall_pass
            ? M2PerformanceGateStatus::passed
            : M2PerformanceGateStatus::failed;
        return Result<M2PerformanceEvidence>::success(std::move(evidence));
    }

    Result<void> remove_mounts(
        const OperationContext& context,
        const M2DeviceIdentity& identity) override {
        auto removed = invoke_script(context, "RemoveMounts", identity, true);
        return removed;
    }

private:
    std::filesystem::path state_path() const {
        return evidence_directory_ / L"workload-state.json";
    }

    Result<std::string> load_state() const {
        return read_utf8_text(state_path(), L"read_m2_workload_state");
    }

    Result<void> invoke_script(
        const OperationContext& context,
        std::string_view phase,
        const M2DeviceIdentity& identity,
        bool safety_acknowledged) const {
        const auto script = repository_root_ / L"tests" / L"e2e" /
            L"Invoke-M2NtfsWorkload.ps1";
        auto powershell = m2_system_powershell_path();
        if (!powershell) {
            return Result<void>::failure(powershell.error());
        }
        std::vector<std::wstring> arguments{
            powershell.value().wstring(), L"-NoProfile", L"-ExecutionPolicy", L"Bypass",
            L"-File", script.wstring(), L"-Phase", widen_utf8(phase),
            L"-DiskNumber", std::to_wstring(identity.physical_drive_number),
            L"-DeviceIdRaw", widen_utf8(identity.device_id_raw),
            L"-ProviderSessionId", widen_utf8(identity.provider_session_id),
            L"-ExpectedBusType", widen_utf8(expected_bus_type_),
            L"-ExpectedVendorId", widen_utf8(expected_vendor_id_),
            L"-ExpectedProductId", widen_utf8(expected_product_id_),
            L"-MountDirectory", (evidence_directory_ / L"mount").wstring(),
            L"-StatePath", state_path().wstring(),
            L"-BeforeDiskNumbersCsv", join_numbers(shared_.physical_disk_baseline)};
        if (safety_acknowledged) arguments.push_back(L"-SafetyAcknowledged");
        return run_process(context, arguments, L"Invoke-M2NtfsWorkload.ps1");
    }

    static std::wstring join_numbers(std::span<const std::uint32_t> numbers) {
        std::wstring result;
        for (const auto value : numbers) {
            if (!result.empty()) result.push_back(L',');
            result += std::to_wstring(value);
        }
        return result;
    }

    std::filesystem::path repository_root_;
    std::filesystem::path evidence_directory_;
    std::string expected_bus_type_;
    std::string expected_vendor_id_;
    std::string expected_product_id_;
    M2RealSharedState& shared_;
    WindowsAimSession& aim_;
    std::string machine_fingerprint_;
    std::string build_fingerprint_;
};

class WindowsM2ReportPublisher final : public IM2ReportPublisher {
public:
    explicit WindowsM2ReportPublisher(std::filesystem::path final_path)
        : final_path_(std::move(final_path)) {}

    Result<void> publish(
        const OperationContext& context,
        const M2Report& report) override {
        auto checked = check_operation_context(context);
        if (!checked) return checked;
        return publish_m2_report(report, final_path_);
    }

private:
    std::filesystem::path final_path_;
};

std::string computer_name() {
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> value{};
    DWORD length = static_cast<DWORD>(value.size());
    if (!GetComputerNameW(value.data(), &length)) return {};
    return narrow_utf8(std::wstring_view(value.data(), length));
}

std::string processor_name() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0,
            KEY_QUERY_VALUE,
            &key) != ERROR_SUCCESS) {
        return {};
    }
    std::array<wchar_t, 512> value{};
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const auto status = RegQueryValueExW(
        key,
        L"ProcessorNameString",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(value.data()),
        &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    const auto length = wcsnlen(value.data(), value.size());
    return narrow_utf8(std::wstring_view(value.data(), length));
}

M2MachineOs machine_evidence() {
    M2MachineOs result;
    result.machine_name = computer_name();
    result.os_name = "Windows";
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<RtlGetVersionFn>(
              GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
        result.os_version = std::to_string(version.dwMajorVersion) + "." +
            std::to_string(version.dwMinorVersion) + "." +
            std::to_string(version.dwBuildNumber);
        result.windows_build = std::to_string(version.dwBuildNumber);
    } else {
        result.os_version = "unavailable";
        result.windows_build = "unavailable";
    }
#if defined(_M_X64)
    result.architecture = "x64";
#elif defined(_M_ARM64)
    result.architecture = "arm64";
#else
    result.architecture = "unsupported";
#endif
    result.cpu_model = processor_name();
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    result.logical_processor_count = system.dwNumberOfProcessors;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.physical_memory_bytes = memory.ullTotalPhys;
    }
    std::ostringstream fingerprint;
    fingerprint << result.machine_name << '\0' << result.os_version << '\0'
                << result.architecture << '\0' << result.cpu_model << '\0'
                << result.logical_processor_count << '\0'
                << result.physical_memory_bytes;
    result.machine_fingerprint = hash_text(fingerprint.str());
    return result;
}

Result<M2Report> make_real_report_template(
    const M2CliConfig& config,
    const std::filesystem::path& executable_path,
    std::string_view run_id) {
    auto executable_hash = hash_file(
        executable_path, L"hash_m2_executable");
    if (!executable_hash) {
        return Result<M2Report>::failure(executable_hash.error());
    }
    auto baseline_hash = hash_file(
        config.accepted_m1_report, L"hash_accepted_m1_report");
    if (!baseline_hash) {
        return Result<M2Report>::failure(baseline_hash.error());
    }
    M2Report report;
    report.run.run_id = std::string(run_id);
    report.run.mode = M2RunMode::real;
#ifdef _DEBUG
    const std::string configuration = "Debug";
#else
    const std::string configuration = "Release";
#endif
    const auto executable_utf8 = narrow_utf8(executable_path.wstring());
    const auto compiler_version = std::to_string(_MSC_FULL_VER);
    const auto build_fingerprint = hash_text(
        executable_hash.value() + "|" + configuration + "|" +
        compiler_version);
    report.build_toolchain = {
        "2.0.0",
        "runtime-worktree",
        configuration,
        "MSVC",
        compiler_version,
        "CMake-configured",
        executable_utf8,
        executable_hash.move_value(),
        build_fingerprint};
    report.accepted_m1_baseline = {
        narrow_utf8(config.accepted_m1_report.wstring()),
        baseline_hash.move_value(),
        1,
        "GO",
        true};
    report.machine_os = machine_evidence();
    report.geometry_options = config.geometry;
    report.geometry_options.backing_kind = "zero";
    report.state.initial_state = M2InitialState::empty;
    report.state.initial_backing_generation = 0;
    report.state.current_backing_generation = 0;
    if (report.run.run_id.empty() || executable_utf8.empty() ||
        report.machine_os.machine_name.empty() ||
        report.machine_os.cpu_model.empty() ||
        report.machine_os.logical_processor_count == 0 ||
        report.machine_os.physical_memory_bytes == 0) {
        return Result<M2Report>::failure(acceptance_error(
            L"make_real_report_template",
            L"build or machine evidence is incomplete"));
    }
    return Result<M2Report>::success(std::move(report));
}

Result<void> validate_accepted_m1_report(
    const OperationContext& context,
    const std::filesystem::path& repository_root,
    const std::filesystem::path& report_path) {
    auto powershell = m2_system_powershell_path();
    if (!powershell) return Result<void>::failure(powershell.error());
    const std::vector<std::wstring> arguments{
        powershell.value().wstring(),
        L"-NoProfile",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-File",
        (repository_root / L"tools" / L"Verify-M1Json.ps1").wstring(),
        L"-ReportPath",
        report_path.wstring()};
    return run_process(context, arguments, L"Verify-M1Json.ps1");
}

} // namespace

Result<M2RealRun> run_m2_windows_acceptance(
    const M2CliConfig& config,
    const std::filesystem::path& executable_path) {
    if (config.mode != M2CliMode::real || !config.safety_acknowledged) {
        return Result<M2RealRun>::failure(acceptance_error(
            L"run_m2_windows_acceptance",
            L"real mode and destructive safety acknowledgement are required"));
    }
    auto repository_root = locate_repository_root(executable_path);
    if (!repository_root) {
        return Result<M2RealRun>::failure(repository_root.error());
    }
    OperationContext verifier_context;
    verifier_context.deadline =
        std::chrono::steady_clock::now() + 30s;
    auto accepted = validate_accepted_m1_report(
        verifier_context,
        repository_root.value(),
        config.accepted_m1_report);
    if (!accepted) return Result<M2RealRun>::failure(accepted.error());

    auto generated_id = proxy::generate_session_id();
    if (!generated_id) {
        return Result<M2RealRun>::failure(generated_id.error());
    }
    const auto run_id = narrow_utf8(generated_id.value());
    if (run_id.empty()) {
        return Result<M2RealRun>::failure(acceptance_error(
            L"run_m2_windows_acceptance",
            L"failed to encode generated run id"));
    }
    auto report = make_real_report_template(config, executable_path, run_id);
    if (!report) return Result<M2RealRun>::failure(report.error());
    auto report_path = make_unique_m2_report_path(
        config.evidence_directory, run_id);
    if (!report_path) {
        return Result<M2RealRun>::failure(report_path.error());
    }

    M2RealSharedState shared;
    WindowsAimSession aim_session;
    WindowsM2BaselineControl baseline(
        aim_session,
        config.expected_driver_service,
        config.expected_driver_sha256,
        report.value().geometry_options,
        report.value().state);
    WindowsM2ProviderFactory providers(shared);
    WindowsM2DeviceControl devices(
        aim_session,
        shared,
        config.expected_bus_type,
        config.expected_vendor_id,
        config.expected_product_id);
    WindowsM2VolumeWorkload workload(
        repository_root.move_value(),
        config.evidence_directory,
        config.expected_bus_type,
        config.expected_vendor_id,
        config.expected_product_id,
        shared,
        aim_session,
        report.value().machine_os.machine_fingerprint,
        report.value().build_toolchain.build_fingerprint);
    WindowsM2ReportPublisher publisher(report_path.value());
    M2SystemLifecycleClock clock;
    M2LifecycleConfig lifecycle;
    lifecycle.report = report.move_value();
    lifecycle.phase_timeout = 30min;
    lifecycle.injected_failure_after = config.injected_failure_after;
    auto completed = run_m2_lifecycle(
        baseline,
        providers,
        devices,
        workload,
        publisher,
        clock,
        std::move(lifecycle));
    return finalize_m2_windows_acceptance(
        std::move(completed), report_path.move_value());
}

} // namespace myrd
