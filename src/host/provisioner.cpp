#include "src/host/provisioner.h"
#include "src/common/sha256.h"
#include "src/common/unique_handle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace myrd::host {
namespace {

template <typename T>
Result<T> policy_failure(
    ProvisioningPolicyError code,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::evidence,
        static_cast<std::uint32_t>(code),
        L"assess_new_disk_provisioning",
        message});
}

bool is_nil(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{}; });
}

std::wstring physical_drive_path(std::uint32_t number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

bool valid_expectation(
    const NewDiskProvisioningExpectation& expected) noexcept {
    if (is_nil(expected.owner.profile_uuid)
        || is_nil(expected.owner.image_uuid)
        || is_nil(expected.operation_id)
        || expected.owner.profile_revision == 0
        || expected.owner.session_generation == 0
        || expected.physical_disk.length_bytes == 0
        || expected.physical_disk.path != physical_drive_path(
            expected.physical_disk.physical_drive_number)) {
        return false;
    }
    const auto address = aim::decode_device_address(
        expected.owner.aim_device_id);
    return address.path_id == expected.physical_disk.path_id
        && address.target_id == expected.physical_disk.target_id
        && address.lun == expected.physical_disk.lun;
}

bool shares_authoritative_anchor(
    const PhysicalDiskIdentity& expected,
    const PhysicalDiskIdentity& observed) noexcept {
    return observed.physical_drive_number == expected.physical_drive_number
        || observed.port_number == expected.port_number
            && observed.path_id == expected.path_id
            && observed.target_id == expected.target_id
            && observed.lun == expected.lun;
}

bool protected_disk(const DiskProtectionFlags& flags) noexcept {
    return flags.is_system || flags.is_boot || flags.boots_from_disk
        || flags.has_pagefile || flags.has_hibernation_file
        || flags.has_crash_dump || flags.is_clustered
        || flags.is_highly_available || flags.is_shadow_copy;
}

bool valid_windows_directory(std::wstring_view path) noexcept {
    if (path.size() < 3 || path.size() > 200
        || !std::iswalpha(path[0]) || path[1] != L':'
        || path[2] != L'\\' || path.back() == L'\\'
        || path.find(L'/') != std::wstring_view::npos
        || path.find(L'\0') != std::wstring_view::npos) {
        return false;
    }
    std::size_t segment_start = 3;
    while (segment_start <= path.size()) {
        const auto separator = path.find(L'\\', segment_start);
        const auto segment = path.substr(
            segment_start,
            separator == std::wstring_view::npos
                ? path.size() - segment_start
                : separator - segment_start);
        if (segment.empty() || segment == L"." || segment == L"..") {
            return false;
        }
        if (separator == std::wstring_view::npos) break;
        segment_start = separator + 1;
    }
    return true;
}

std::wstring encode_powershell_command(std::wstring_view script) {
    static_assert(sizeof(wchar_t) == 2);
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    bytes.reserve(script.size() * 2);
    for (const wchar_t code_unit : script) {
        const auto value = static_cast<unsigned>(code_unit);
        bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
        bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xFFU));
    }

    std::wstring encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const unsigned first = bytes[offset];
        const unsigned second = offset + 1 < bytes.size()
            ? bytes[offset + 1] : 0;
        const unsigned third = offset + 2 < bytes.size()
            ? bytes[offset + 2] : 0;
        const unsigned combined = (first << 16) | (second << 8) | third;
        encoded.push_back(static_cast<wchar_t>(
            alphabet[(combined >> 18) & 0x3FU]));
        encoded.push_back(static_cast<wchar_t>(
            alphabet[(combined >> 12) & 0x3FU]));
        encoded.push_back(offset + 1 < bytes.size()
            ? static_cast<wchar_t>(alphabet[(combined >> 6) & 0x3FU])
            : L'=');
        encoded.push_back(offset + 2 < bytes.size()
            ? static_cast<wchar_t>(alphabet[combined & 0x3FU])
            : L'=');
    }
    return encoded;
}

struct PlannedGptIds final {
    product::Uuid disk_guid;
    product::Uuid partition_guid;
};

constexpr GUID kBasicDataPartitionGuid{
    0xEBD0A0A2L, 0xB9E5, 0x4433,
    {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

Result<product::Uuid> derive_operation_guid(
    const product::Uuid& operation_id,
    std::string_view purpose) {
    std::string material = "myrd.m4.provisioning.v1:";
    material.append(purpose);
    material.push_back(':');
    material.append(operation_id.to_string());
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(material.data()),
        material.size());
    auto digest = sha256_digest(bytes);
    if (!digest.ok()) {
        return Result<product::Uuid>::failure(digest.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string canonical;
    canonical.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            canonical.push_back('-');
        }
        const auto value = std::to_integer<unsigned>(
            digest.value()[index]);
        canonical.push_back(kHex[(value >> 4) & 0x0FU]);
        canonical.push_back(kHex[value & 0x0FU]);
    }
    canonical[14] = '4';
    canonical[19] = kHex[
        8U | (static_cast<unsigned>(canonical[19] <= '9'
            ? canonical[19] - '0'
            : canonical[19] - 'a' + 10) & 0x03U)];
    return product::Uuid::parse(canonical);
}

Result<PlannedGptIds> derive_gpt_ids(
    const product::Uuid& operation_id) {
    auto disk = derive_operation_guid(operation_id, "disk");
    if (!disk.ok()) return Result<PlannedGptIds>::failure(disk.error());
    auto partition = derive_operation_guid(operation_id, "partition");
    if (!partition.ok()) {
        return Result<PlannedGptIds>::failure(partition.error());
    }
    if (disk.value() == partition.value()) {
        return policy_failure<PlannedGptIds>(
            ProvisioningPolicyError::invalid_expectation,
            L"The operation-derived GPT identifiers collided");
    }
    return Result<PlannedGptIds>::success({
        disk.move_value(), partition.move_value()});
}

struct NativeGptLayoutPlan final {
    PlannedGptIds ids;
    std::uint64_t partition_starting_offset{};
    std::uint64_t partition_length_bytes{};
};

Result<NativeGptLayoutPlan> plan_gpt_layout(
    const product::Uuid& operation_id,
    std::uint64_t disk_length_bytes,
    std::uint64_t starting_usable_offset,
    std::uint64_t usable_length_bytes,
    std::uint32_t bytes_per_sector) {
    constexpr std::uint64_t kAlignment = 1ULL << 20;
    const bool valid_sector = bytes_per_sector >= 512
        && bytes_per_sector <= 65536
        && (bytes_per_sector & (bytes_per_sector - 1)) == 0;
    if (!valid_sector || disk_length_bytes == 0
        || usable_length_bytes == 0
        || starting_usable_offset > disk_length_bytes
        || usable_length_bytes
            > disk_length_bytes - starting_usable_offset) {
        return policy_failure<NativeGptLayoutPlan>(
            ProvisioningPolicyError::invalid_environment,
            L"Windows returned invalid GPT usable geometry");
    }
    const auto base = (std::max)(starting_usable_offset, kAlignment);
    if (base > (std::numeric_limits<std::uint64_t>::max)()
            - (kAlignment - 1)) {
        return policy_failure<NativeGptLayoutPlan>(
            ProvisioningPolicyError::invalid_environment,
            L"GPT alignment overflowed");
    }
    const auto start = (base + kAlignment - 1) & ~(kAlignment - 1);
    const auto usable_end = starting_usable_offset + usable_length_bytes;
    const auto end = usable_end
        & ~(static_cast<std::uint64_t>(bytes_per_sector) - 1);
    if (end <= start || end - start < kAlignment) {
        return policy_failure<NativeGptLayoutPlan>(
            ProvisioningPolicyError::invalid_environment,
            L"The disk is too small for the aligned GPT data partition");
    }
    auto ids = derive_gpt_ids(operation_id);
    if (!ids.ok()) {
        return Result<NativeGptLayoutPlan>::failure(ids.error());
    }
    return Result<NativeGptLayoutPlan>::success({
        ids.move_value(), start, end - start});
}

std::wstring widen_ascii(std::string_view text) {
    std::wstring value;
    value.reserve(text.size());
    for (const char character : text) {
        value.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    }
    return value;
}

std::wstring provisioning_script(
    const VerifiedNewDisk& verified,
    const PlannedGptIds& planned) {
    constexpr std::wstring_view kBody = LR"PS(
$partitionDeadline=[DateTime]::UtcNow.AddSeconds(30)
do {
  $partitions=@(Get-Partition -ErrorAction SilentlyContinue | Where-Object { [guid]$_.Guid -eq $expectedPartitionGuid })
  if (@($partitions).Count -eq 1) { break }
  Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $partitionDeadline)
if (@($partitions).Count -ne 1) { throw 'MYRD:partition-guid-count' }
$partition=$partitions[0]
if ([uint32]$partition.DiskNumber -ne $diskNumber) { throw 'MYRD:partition-disk-number' }
$disk=@(Get-Disk -Number ([uint32]$partition.DiskNumber) -ErrorAction Stop)
if (@($disk).Count -ne 1) { throw 'MYRD:disk-count' }
$disk=$disk[0]
if ($disk.Number -ne $diskNumber) { throw 'MYRD:disk-number' }
if ([uint64]$disk.Size -ne $expectedSize) { throw 'MYRD:disk-size' }
if ([string]$disk.PartitionStyle -ne 'GPT') { throw 'MYRD:not-gpt' }
if ([guid]$disk.Guid -ne $expectedDiskGuid) { throw 'MYRD:disk-guid' }
if ([uint32]$disk.NumberOfPartitions -ne 1) { throw 'MYRD:partition-count' }
if ([guid]$partition.Guid -ne $expectedPartitionGuid) { throw 'MYRD:partition-guid' }
if ($disk.IsSystem -or $disk.IsBoot -or $disk.BootFromDisk -or $disk.IsClustered -or $disk.IsHighlyAvailable) { throw 'MYRD:protected' }
if ($disk.IsReadOnly) { throw 'MYRD:read-only' }
if ($disk.IsOffline) { throw 'MYRD:offline' }
$volume=Format-Volume -Partition $partition -FileSystem NTFS -Force -Confirm:$false -ErrorAction Stop
$disk=Get-Disk -Number $diskNumber -ErrorAction Stop
$partitions=@(Get-Partition -ErrorAction Stop | Where-Object { [guid]$_.Guid -eq $expectedPartitionGuid })
if (@($partitions).Count -ne 1) { throw 'MYRD:post-partition-guid-count' }
$partition=$partitions[0]
$volume=Get-Volume -Partition $partition -ErrorAction Stop
if ([string]$volume.FileSystem -ne 'NTFS') { throw 'MYRD:filesystem' }
$volumeId=[string]$volume.UniqueId
if ($volumeId -notmatch '^\\\\\?\\Volume\{(?<id>[0-9A-Fa-f-]{36})\}\\$') { throw 'MYRD:volume-guid' }
Write-Output 'MYRD_PROVISIONING_V1'
Write-Output ('disk_guid=' + ([guid]$disk.Guid).ToString('D').ToLowerInvariant())
Write-Output ('partition_guid=' + ([guid]$partition.Guid).ToString('D').ToLowerInvariant())
Write-Output ('volume_guid=' + ([guid]$Matches['id']).ToString('D').ToLowerInvariant())
)PS";
    std::wstring script = L"$ErrorActionPreference='Stop'\n";
    script += L"$ProgressPreference='SilentlyContinue'\n";
    script += L"[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)\n";
    script += L"$diskNumber=[uint32]";
    script += std::to_wstring(
        verified.physical_disk().physical_drive_number);
    script += L"\n$expectedSize=[uint64]";
    script += std::to_wstring(verified.physical_disk().length_bytes);
    script += L"\n$expectedDiskGuid=[guid]'";
    script += widen_ascii(planned.disk_guid.to_string());
    script += L"'\n$expectedPartitionGuid=[guid]'";
    script += widen_ascii(planned.partition_guid.to_string());
    script += L"'";
    script += kBody;
    return script;
}

Error execution_failure(
    ProvisioningExecutionError code,
    const wchar_t* operation,
    const wchar_t* message) {
    return {
        ErrorDomain::internal,
        static_cast<std::uint32_t>(code),
        operation,
        message};
}

Error windows_execution_failure(
    ProvisioningExecutionError code,
    const wchar_t* operation,
    const wchar_t* message,
    DWORD native_code = GetLastError()) {
    return {
        ErrorDomain::win32,
        static_cast<std::uint32_t>(code),
        operation,
        message,
        native_code};
}

struct PendingDeviceIo final {
    UniqueHandle handle;
    UniqueHandle event;
    OVERLAPPED overlapped{};
    std::vector<std::byte> input;
    std::vector<std::byte> output;
};

Result<std::vector<std::byte>> bounded_device_io_control(
    HANDLE source_handle,
    DWORD control_code,
    std::span<const std::byte> input,
    std::size_t output_bytes,
    ProvisioningExecutionError semantic_code,
    const wchar_t* operation,
    std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    if (source_handle == nullptr || source_handle == INVALID_HANDLE_VALUE
        || input.size() > (std::numeric_limits<DWORD>::max)()
        || output_bytes > (std::numeric_limits<DWORD>::max)()
        || timeout <= std::chrono::milliseconds::zero()) {
        return Result<std::vector<std::byte>>::failure(execution_failure(
            semantic_code,
            operation,
            L"The bounded device-control request is invalid"));
    }
    auto state = std::make_unique<PendingDeviceIo>();
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            source_handle,
            GetCurrentProcess(),
            &duplicate,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                semantic_code,
                operation,
                L"Failed to isolate a bounded device-control handle"));
    }
    state->handle.reset(duplicate);
    state->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!state->event) {
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                semantic_code,
                operation,
                L"Failed to create the bounded device-control event"));
    }
    state->overlapped.hEvent = state->event.get();
    state->input.assign(input.begin(), input.end());
    state->output.resize(output_bytes);
    DWORD returned = 0;
    const BOOL issued = DeviceIoControl(
        state->handle.get(),
        control_code,
        state->input.empty() ? nullptr : state->input.data(),
        static_cast<DWORD>(state->input.size()),
        state->output.empty() ? nullptr : state->output.data(),
        static_cast<DWORD>(state->output.size()),
        &returned,
        &state->overlapped);
    if (issued) {
        state->output.resize(returned);
        return Result<std::vector<std::byte>>::success(
            std::move(state->output));
    }
    const DWORD issue_error = GetLastError();
    if (issue_error != ERROR_IO_PENDING) {
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                semantic_code,
                operation,
                L"The bounded device-control request failed",
                issue_error));
    }
    const auto timeout_count = timeout.count();
    const DWORD wait_timeout = timeout_count
            > static_cast<long long>((std::numeric_limits<DWORD>::max)())
        ? (std::numeric_limits<DWORD>::max)()
        : static_cast<DWORD>(timeout_count);
    const DWORD wait = WaitForSingleObject(state->event.get(), wait_timeout);
    if (wait == WAIT_OBJECT_0) {
        if (!GetOverlappedResult(
                state->handle.get(),
                &state->overlapped,
                &returned,
                FALSE)) {
            return Result<std::vector<std::byte>>::failure(
                windows_execution_failure(
                    semantic_code,
                    operation,
                    L"The bounded device-control request completed with failure"));
        }
        if (returned > state->output.size()) {
            return Result<std::vector<std::byte>>::failure(
                windows_execution_failure(
                    semantic_code,
                    operation,
                    L"The device-control byte count exceeded its buffer",
                    ERROR_INVALID_DATA));
        }
        state->output.resize(returned);
        return Result<std::vector<std::byte>>::success(
            std::move(state->output));
    }
    if (wait != WAIT_TIMEOUT) {
        const DWORD wait_error = GetLastError();
        static_cast<void>(CancelIoEx(
            state->handle.get(), &state->overlapped));
        const bool quarantined = WaitForSingleObject(
            state->event.get(), 1000) != WAIT_OBJECT_0;
        if (quarantined) {
            static_cast<void>(state.release());
        }
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                quarantined
                    ? ProvisioningExecutionError::io_indeterminate
                    : semantic_code,
                operation,
                L"Failed while waiting for bounded device control",
                wait_error));
    }

    static_cast<void>(CancelIoEx(
        state->handle.get(), &state->overlapped));
    const DWORD cancel_wait = WaitForSingleObject(
        state->event.get(), 1000);
    const bool quarantined = cancel_wait != WAIT_OBJECT_0;
    if (quarantined) {
        // A broken kernel/filter driver may ignore cancellation. Keeping the
        // duplicated handle, buffers, event and OVERLAPPED alive forever is
        // deliberate quarantine: it bounds the service call without a UAF or
        // accidental reuse of an in-flight destructive request. Provisioning
        // is one-shot, so the profile transitions to NeedsAttention instead
        // of retrying and accumulating quarantined requests.
        static_cast<void>(state.release());
    }
    return Result<std::vector<std::byte>>::failure(
        windows_execution_failure(
            quarantined
                ? ProvisioningExecutionError::io_indeterminate
                : ProvisioningExecutionError::timed_out,
            operation,
            L"The device-control request exceeded its bounded deadline",
            WAIT_TIMEOUT));
}

std::wstring quote_process_argument(std::wstring_view value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(value);
    }
    std::wstring result(1, L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

Result<void> make_inherited_pipe(
    UniqueHandle& read_handle,
    UniqueHandle& write_handle,
    const wchar_t* operation) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &security, 0)) {
        return Result<void>::failure(windows_execution_failure(
            ProvisioningExecutionError::process_failed,
            operation,
            L"Failed to create a bounded child-process pipe"));
    }
    read_handle.reset(raw_read);
    write_handle.reset(raw_write);
    if (!SetHandleInformation(
            read_handle.get(), HANDLE_FLAG_INHERIT, 0)) {
        return Result<void>::failure(windows_execution_failure(
            ProvisioningExecutionError::process_failed,
            operation,
            L"Failed to make the parent pipe endpoint private"));
    }
    return Result<void>::success();
}

struct BoundedCapture final {
    std::string text;
    std::uint64_t limit{};
    bool truncated{};
    bool closed{};
};

Result<void> drain_pipe(
    HANDLE pipe,
    BoundedCapture& capture,
    const wchar_t* operation) {
    std::array<char, 4096> bytes{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                capture.closed = true;
                return Result<void>::success();
            }
            return Result<void>::failure(windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                operation,
                L"Failed while inspecting child-process output",
                error));
        }
        if (available == 0) return Result<void>::success();
        const DWORD requested = (std::min)(
            available, static_cast<DWORD>(bytes.size()));
        DWORD read = 0;
        if (!ReadFile(pipe, bytes.data(), requested, &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                capture.closed = true;
                return Result<void>::success();
            }
            return Result<void>::failure(windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                operation,
                L"Failed while reading child-process output",
                error));
        }
        const auto retained = static_cast<std::uint64_t>(capture.text.size());
        const auto remaining = capture.limit > retained
            ? capture.limit - retained : 0;
        const auto append = static_cast<std::size_t>((std::min)(
            remaining, static_cast<std::uint64_t>(read)));
        capture.text.append(bytes.data(), append);
        if (append != read) capture.truncated = true;
    }
}

class ProcessAttributeList final {
public:
    Result<void> initialize(std::span<HANDLE> inherited_handles) {
        SIZE_T required = 0;
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr, 1, 0, &required));
        if (required == 0) {
            return Result<void>::failure(windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"InitializeProcThreadAttributeList",
                L"Windows did not report an attribute-list size"));
        }
        storage_.resize(required);
        list_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &required)) {
            list_ = nullptr;
            return Result<void>::failure(windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"InitializeProcThreadAttributeList",
                L"Failed to initialize the child handle allowlist"));
        }
        if (!UpdateProcThreadAttribute(
                list_,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles.data(),
                inherited_handles.size_bytes(),
                nullptr,
                nullptr)) {
            return Result<void>::failure(windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"UpdateProcThreadAttribute",
                L"Failed to restrict inherited child handles"));
        }
        return Result<void>::success();
    }

    ~ProcessAttributeList() {
        if (list_ != nullptr) DeleteProcThreadAttributeList(list_);
    }

    PPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    std::vector<std::byte> storage_;
    PPROC_THREAD_ATTRIBUTE_LIST list_{};
};

Result<ProvisioningProcessResult> run_windows_process(
    const ProvisioningCommand& command,
    const OperationContext& context) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<ProvisioningProcessResult>::failure(live.error());
    }
    constexpr std::uint64_t kMaximumCaptureBytes = 1024ULL * 1024ULL;
    if (command.executable.empty() || command.arguments.empty()
        || command.timeout <= std::chrono::milliseconds::zero()
        || command.maximum_stdout_bytes > kMaximumCaptureBytes
        || command.maximum_stderr_bytes > kMaximumCaptureBytes) {
        return Result<ProvisioningProcessResult>::failure(
            execution_failure(
                ProvisioningExecutionError::process_failed,
                L"provision.run_windows_process",
                L"The provisioning child command is invalid"));
    }

    UniqueHandle stdout_read;
    UniqueHandle stdout_write;
    auto pipe = make_inherited_pipe(
        stdout_read, stdout_write, L"CreatePipe(stdout)");
    if (!pipe.ok()) {
        return Result<ProvisioningProcessResult>::failure(pipe.error());
    }
    UniqueHandle stderr_read;
    UniqueHandle stderr_write;
    pipe = make_inherited_pipe(
        stderr_read, stderr_write, L"CreatePipe(stderr)");
    if (!pipe.ok()) {
        return Result<ProvisioningProcessResult>::failure(pipe.error());
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueHandle null_input(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!null_input) {
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"CreateFileW(NUL)",
                L"Failed to open a deterministic child input handle"));
    }

    std::array inherited{
        null_input.get(), stdout_write.get(), stderr_write.get()};
    ProcessAttributeList attributes;
    auto initialized = attributes.initialize(inherited);
    if (!initialized.ok()) {
        return Result<ProvisioningProcessResult>::failure(
            initialized.error());
    }

    std::wstring command_line = quote_process_argument(command.executable);
    for (const auto& argument : command.arguments) {
        command_line.push_back(L' ');
        command_line += quote_process_argument(argument);
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION raw_process{};
    constexpr DWORD kCreationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED
        | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(
            command.executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            kCreationFlags,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &raw_process)) {
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"CreateProcessW(provisioning)",
                L"Failed to start the bounded provisioning child"));
    }
    UniqueHandle process(raw_process.hProcess);
    UniqueHandle primary_thread(raw_process.hThread);

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        TerminateProcess(process.get(), 0xE510U);
        WaitForSingleObject(process.get(), 5000);
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"CreateJobObjectW(provisioning)",
                L"Failed to create a bounded provisioning job"));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))
        || !AssignProcessToJobObject(job.get(), process.get())) {
        const DWORD error = GetLastError();
        TerminateProcess(process.get(), 0xE511U);
        WaitForSingleObject(process.get(), 5000);
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"AssignProcessToJobObject(provisioning)",
                L"Failed to constrain the provisioning process tree",
                error));
    }
    if (ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job.get(), 0xE512U);
        WaitForSingleObject(process.get(), 5000);
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"ResumeThread(provisioning)",
                L"Failed to resume the bounded provisioning child",
                error));
    }
    stdout_write.reset();
    stderr_write.reset();
    null_input.reset();

    BoundedCapture stdout_capture{
        {}, command.maximum_stdout_bytes, false, false};
    BoundedCapture stderr_capture{
        {}, command.maximum_stderr_bytes, false, false};
    const auto started = std::chrono::steady_clock::now();
    bool timed_out = false;
    bool completed = false;
    for (;;) {
        auto drained = drain_pipe(
            stdout_read.get(), stdout_capture, L"ReadFile(stdout)");
        if (!drained.ok()) {
            TerminateJobObject(job.get(), 0xE513U);
            WaitForSingleObject(process.get(), 5000);
            return Result<ProvisioningProcessResult>::failure(
                drained.error());
        }
        drained = drain_pipe(
            stderr_read.get(), stderr_capture, L"ReadFile(stderr)");
        if (!drained.ok()) {
            TerminateJobObject(job.get(), 0xE514U);
            WaitForSingleObject(process.get(), 5000);
            return Result<ProvisioningProcessResult>::failure(
                drained.error());
        }
        const DWORD wait = WaitForSingleObject(process.get(), 10);
        if (wait == WAIT_OBJECT_0) {
            completed = true;
            break;
        }
        if (wait != WAIT_TIMEOUT) {
            const DWORD error = GetLastError();
            TerminateJobObject(job.get(), 0xE515U);
            WaitForSingleObject(process.get(), 5000);
            return Result<ProvisioningProcessResult>::failure(
                windows_execution_failure(
                    ProvisioningExecutionError::process_failed,
                    L"WaitForSingleObject(provisioning)",
                    L"Failed while waiting for the provisioning child",
                    error));
        }
        live = check_operation_context(context);
        if (!live.ok()) {
            TerminateJobObject(job.get(), 0xE516U);
            WaitForSingleObject(process.get(), 5000);
            return Result<ProvisioningProcessResult>::failure(live.error());
        }
        if (std::chrono::steady_clock::now() - started
            >= command.timeout) {
            timed_out = true;
            TerminateJobObject(job.get(), 0xE517U);
            static_cast<void>(WaitForSingleObject(process.get(), 5000));
            break;
        }
    }

    // The strict inherited-handle allowlist guarantees EOF once the process
    // has exited or the job has been terminated.
    for (unsigned attempt = 0; attempt < 100
         && (!stdout_capture.closed || !stderr_capture.closed); ++attempt) {
        auto drained = drain_pipe(
            stdout_read.get(), stdout_capture, L"ReadFile(stdout-final)");
        if (!drained.ok()) {
            return Result<ProvisioningProcessResult>::failure(
                drained.error());
        }
        drained = drain_pipe(
            stderr_read.get(), stderr_capture, L"ReadFile(stderr-final)");
        if (!drained.ok()) {
            return Result<ProvisioningProcessResult>::failure(
                drained.error());
        }
        if (!stdout_capture.closed || !stderr_capture.closed) Sleep(1);
    }
    if (!stdout_capture.closed || !stderr_capture.closed) {
        return Result<ProvisioningProcessResult>::failure(
            execution_failure(
                ProvisioningExecutionError::process_failed,
                L"provision.capture",
                L"The provisioning child retained an output pipe"));
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) {
        return Result<ProvisioningProcessResult>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::process_failed,
                L"GetExitCodeProcess(provisioning)",
                L"Failed to obtain the provisioning child exit code"));
    }
    if (!completed && !timed_out) {
        return Result<ProvisioningProcessResult>::failure(
            execution_failure(
                ProvisioningExecutionError::process_failed,
                L"provision.run_windows_process",
                L"The provisioning process ended without a known result"));
    }
    return Result<ProvisioningProcessResult>::success({
        exit_code,
        timed_out,
        stdout_capture.truncated,
        stderr_capture.truncated,
        std::move(stdout_capture.text),
        std::move(stderr_capture.text)});
}

std::wstring disk_observation_script(
    const PhysicalDiskIdentity& expected) {
    constexpr std::wstring_view kBody = LR"PS(
$disk=@(Get-Disk -Number $diskNumber -ErrorAction Stop)
if (@($disk).Count -ne 1) { throw 'MYRD:disk-count' }
$disk=$disk[0]
if ([uint32]$disk.Number -ne $diskNumber) { throw 'MYRD:disk-number' }
if ([uint64]$disk.Size -ne $expectedSize) { throw 'MYRD:disk-size' }
Write-Output 'MYRD_DISK_OBSERVATION_V1'
Write-Output ('number=' + ([uint32]$disk.Number).ToString([Globalization.CultureInfo]::InvariantCulture))
Write-Output ('size=' + ([uint64]$disk.Size).ToString([Globalization.CultureInfo]::InvariantCulture))
Write-Output ('partition_style=' + ([string]$disk.PartitionStyle).ToUpperInvariant())
Write-Output ('partition_count=' + ([uint32]$disk.NumberOfPartitions).ToString([Globalization.CultureInfo]::InvariantCulture))
Write-Output ('is_system=' + ([int][bool]$disk.IsSystem))
Write-Output ('is_boot=' + ([int][bool]$disk.IsBoot))
Write-Output ('boots_from_disk=' + ([int][bool]$disk.BootFromDisk))
Write-Output ('is_clustered=' + ([int][bool]$disk.IsClustered))
Write-Output ('is_highly_available=' + ([int][bool]$disk.IsHighlyAvailable))
Write-Output ('is_read_only=' + ([int][bool]$disk.IsReadOnly))
Write-Output ('is_offline=' + ([int][bool]$disk.IsOffline))
)PS";
    std::wstring script = L"$ErrorActionPreference='Stop'\n";
    script += L"$ProgressPreference='SilentlyContinue'\n";
    script += L"[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)\n";
    script += L"$diskNumber=[uint32]";
    script += std::to_wstring(expected.physical_drive_number);
    script += L"\n$expectedSize=[uint64]";
    script += std::to_wstring(expected.length_bytes);
    script += kBody;
    return script;
}

Result<ProvisioningCommand> build_disk_observation_command(
    const PhysicalDiskIdentity& expected,
    std::wstring_view windows_directory) {
    if (expected.length_bytes == 0
        || expected.path != physical_drive_path(
            expected.physical_drive_number)
        || !valid_windows_directory(windows_directory)) {
        return policy_failure<ProvisioningCommand>(
            ProvisioningPolicyError::invalid_environment,
            L"The disk observer received an invalid trusted identity");
    }
    std::wstring executable(windows_directory);
    executable +=
        L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    return Result<ProvisioningCommand>::success({
        std::move(executable),
        {L"-NoLogo", L"-NoProfile", L"-NonInteractive",
         L"-ExecutionPolicy", L"Bypass", L"-EncodedCommand",
         encode_powershell_command(disk_observation_script(expected))},
        std::chrono::seconds(30),
        4096,
        4096,
        "observe.new_disk"});
}

template <typename Integer>
bool parse_decimal(std::string_view text, Integer& value) noexcept {
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return false;
    }
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

bool parse_boolean(std::string_view text, bool& value) noexcept {
    if (text == "0") {
        value = false;
        return true;
    }
    if (text == "1") {
        value = true;
        return true;
    }
    return false;
}

Result<NewDiskObservation> parse_disk_observation(
    const PhysicalDiskIdentity& physical_disk,
    ObservedDiskLayout handle_layout,
    std::uint32_t handle_partition_count,
    std::uint32_t handle_volume_count,
    std::uint32_t handle_extent_count,
    bool handle_read_only,
    bool handle_offline,
    std::string_view output) {
    auto malformed = [] {
        return policy_failure<NewDiskObservation>(
            ProvisioningPolicyError::malformed_result,
            L"The read-only disk observer returned malformed output");
    };
    if (output.empty() || output.size() > 4096
        || output.find('\0') != std::string_view::npos) {
        return malformed();
    }
    std::array<std::string_view, 12> lines{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (start < output.size()) {
        if (count == lines.size()) return malformed();
        const auto newline = output.find('\n', start);
        auto line = output.substr(
            start,
            newline == std::string_view::npos
                ? output.size() - start
                : newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return malformed();
        lines[count++] = line;
        start = newline == std::string_view::npos
            ? output.size() : newline + 1;
    }
    if (count != lines.size()
        || lines[0] != "MYRD_DISK_OBSERVATION_V1") {
        return malformed();
    }
    constexpr std::array<std::string_view, 11> prefixes{
        "number=", "size=", "partition_style=", "partition_count=",
        "is_system=", "is_boot=", "boots_from_disk=", "is_clustered=",
        "is_highly_available=", "is_read_only=", "is_offline="};
    std::array<std::string_view, 11> values{};
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        const auto line = lines[index + 1];
        if (!line.starts_with(prefixes[index])) return malformed();
        values[index] = line.substr(prefixes[index].size());
    }

    std::uint32_t number = 0;
    std::uint64_t size = 0;
    std::uint32_t partition_count = 0;
    if (!parse_decimal(values[0], number)
        || !parse_decimal(values[1], size)
        || !parse_decimal(values[3], partition_count)) {
        return malformed();
    }
    ObservedDiskLayout script_layout = ObservedDiskLayout::unknown;
    if (values[2] == "RAW") script_layout = ObservedDiskLayout::raw;
    else if (values[2] == "MBR") script_layout = ObservedDiskLayout::mbr;
    else if (values[2] == "GPT") script_layout = ObservedDiskLayout::gpt;
    else if (values[2] != "UNKNOWN") return malformed();

    DiskProtectionFlags protection{};
    std::array<bool*, 7> flags{
        &protection.is_system,
        &protection.is_boot,
        &protection.boots_from_disk,
        &protection.is_clustered,
        &protection.is_highly_available,
        &protection.is_read_only,
        &protection.is_offline};
    for (std::size_t index = 0; index < flags.size(); ++index) {
        if (!parse_boolean(values[index + 4], *flags[index])) {
            return malformed();
        }
    }
    if (number != physical_disk.physical_drive_number
        || size != physical_disk.length_bytes
        || script_layout != handle_layout
        || partition_count != handle_partition_count
        || protection.is_read_only != handle_read_only
        || protection.is_offline != handle_offline) {
        return policy_failure<NewDiskObservation>(
            ProvisioningPolicyError::identity_conflict,
            L"PowerShell and the retained PhysicalDrive handle disagree");
    }
    const bool file_role_absence_proven = handle_layout
            == ObservedDiskLayout::raw
        && handle_partition_count == 0
        && handle_volume_count == 0
        && handle_extent_count == 0;
    protection.has_pagefile = !file_role_absence_proven;
    protection.has_hibernation_file = !file_role_absence_proven;
    protection.has_crash_dump = !file_role_absence_proven;
    protection.is_shadow_copy = !file_role_absence_proven;
    return Result<NewDiskObservation>::success({
        physical_disk,
        handle_layout,
        partition_count,
        handle_volume_count,
        handle_extent_count,
        protection});
}

template <typename Value>
Result<Value> query_disk_ioctl(
    HANDLE handle,
    DWORD control_code,
    const wchar_t* operation) {
    auto bytes = bounded_device_io_control(
        handle,
        control_code,
        {},
        sizeof(Value),
        ProvisioningExecutionError::open_failed,
        operation);
    if (!bytes.ok()) return Result<Value>::failure(bytes.error());
    if (bytes.value().size() < sizeof(Value)) {
        return Result<Value>::failure(windows_execution_failure(
            ProvisioningExecutionError::open_failed,
            operation,
            L"Windows returned truncated PhysicalDrive evidence",
            ERROR_INVALID_DATA));
    }
    Value value{};
    std::memcpy(&value, bytes.value().data(), sizeof(value));
    return Result<Value>::success(value);
}

struct RetainedDiskSnapshot final {
    PhysicalDiskIdentity physical_disk;
    ObservedDiskLayout layout{ObservedDiskLayout::unknown};
    std::uint32_t partition_count{};
    std::uint32_t volume_count{};
    std::uint32_t extent_count{};
    GUID gpt_disk_guid{};
    GUID first_partition_guid{};
    GUID first_partition_type{};
    bool has_gpt_disk_guid{};
    bool has_first_partition_guid{};
    std::uint64_t first_partition_starting_offset{};
    std::uint64_t first_partition_length_bytes{};
    std::uint64_t first_partition_attributes{};
    bool has_single_volume_extent{};
    std::uint64_t single_volume_starting_offset{};
    std::uint64_t single_volume_length_bytes{};
    bool is_read_only{};
    bool is_offline{};
};

Result<std::vector<std::byte>> query_drive_layout(HANDLE handle) {
    auto queried = bounded_device_io_control(
        handle,
        IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        {},
        64ULL * 1024ULL,
        ProvisioningExecutionError::open_failed,
        L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX(provisioning)");
    if (!queried.ok()) return queried;
    auto layout_bytes = queried.move_value();
    const auto layout_size = layout_bytes.size();
    constexpr std::size_t kPartitionEntriesOffset =
        offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    if (layout_size < kPartitionEntriesOffset) {
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::open_failed,
                L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX(provisioning)",
                L"Windows returned a truncated disk layout",
                ERROR_INVALID_DATA));
    }
    const auto* layout =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            layout_bytes.data());
    if (layout->PartitionCount
        > (layout_size - kPartitionEntriesOffset)
            / sizeof(PARTITION_INFORMATION_EX)) {
        return Result<std::vector<std::byte>>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::open_failed,
                L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX(provisioning)",
                L"The disk layout partition count exceeds its buffer",
                ERROR_INVALID_DATA));
    }
    return Result<std::vector<std::byte>>::success(
        std::move(layout_bytes));
}

class FindVolumeGuard final {
public:
    explicit FindVolumeGuard(HANDLE handle) noexcept : handle_(handle) {}
    ~FindVolumeGuard() {
        if (handle_ != INVALID_HANDLE_VALUE) FindVolumeClose(handle_);
    }
    FindVolumeGuard(const FindVolumeGuard&) = delete;
    FindVolumeGuard& operator=(const FindVolumeGuard&) = delete;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct TargetVolumeExtents final {
    std::uint32_t volume_count{};
    std::uint32_t extent_count{};
    bool has_single_extent{};
    std::uint64_t single_starting_offset{};
    std::uint64_t single_length_bytes{};
};

bool volume_requires_disk_extents(std::uint32_t device_type) noexcept {
    // Optical volumes are enumerated by FindFirstVolumeW but cannot have a
    // disk extent on a PhysicalDrive. All other device types remain
    // fail-closed by requiring the extent query below.
    return device_type != FILE_DEVICE_CD_ROM;
}

ObservedDiskLayout classify_drive_layout(
    std::uint32_t partition_style,
    std::uint32_t partition_count,
    std::uint32_t mbr_signature,
    std::uint32_t mbr_checksum) noexcept {
    if (partition_style == PARTITION_STYLE_RAW) {
        return ObservedDiskLayout::raw;
    }
    if (partition_style == PARTITION_STYLE_GPT) {
        return ObservedDiskLayout::gpt;
    }
    if (partition_style == PARTITION_STYLE_MBR) {
        // IOCTL_DISK_GET_DRIVE_LAYOUT_EX reports a newly zeroed Windows disk
        // as an empty, zero-signature MBR while Get-Disk reports RAW. Only
        // this exact zeroed representation is normalized to RAW.
        if (partition_count == 0 && mbr_signature == 0
            && mbr_checksum == 0) {
            return ObservedDiskLayout::raw;
        }
        return ObservedDiskLayout::mbr;
    }
    return ObservedDiskLayout::unknown;
}

Result<TargetVolumeExtents>
count_volume_extents_for_disk(std::uint32_t disk_number) {
    std::array<wchar_t, 32768> volume_path{};
    const HANDLE enumeration = FindFirstVolumeW(
        volume_path.data(), static_cast<DWORD>(volume_path.size()));
    if (enumeration == INVALID_HANDLE_VALUE) {
        return Result<TargetVolumeExtents>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::open_failed,
                L"FindFirstVolumeW(provisioning)",
                L"Failed to enumerate existing volume extents"));
    }
    FindVolumeGuard guard(enumeration);
    std::uint32_t volume_count = 0;
    std::uint32_t extent_count = 0;
    std::uint64_t sole_starting_offset = 0;
    std::uint64_t sole_length_bytes = 0;
    for (;;) {
        std::wstring open_path(volume_path.data());
        if (open_path.empty() || open_path.back() != L'\\') {
            return Result<TargetVolumeExtents>::failure(
                execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"FindFirstVolumeW(provisioning)",
                    L"Windows returned a malformed volume GUID path"));
        }
        open_path.pop_back();
        UniqueHandle volume(CreateFileW(
            open_path.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr));
        if (!volume) {
            return Result<TargetVolumeExtents>::failure(
                windows_execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"CreateFileW(volume extent preflight)",
                    L"Could not prove that an enumerated volume avoids the target disk"));
        }

        auto storage_number_bytes = bounded_device_io_control(
            volume.get(),
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            {},
            sizeof(STORAGE_DEVICE_NUMBER),
            ProvisioningExecutionError::open_failed,
            L"IOCTL_STORAGE_GET_DEVICE_NUMBER(provisioning)");
        if (!storage_number_bytes.ok()) {
            return Result<TargetVolumeExtents>::failure(
                storage_number_bytes.error());
        }
        if (storage_number_bytes.value().size()
            < sizeof(STORAGE_DEVICE_NUMBER)) {
            return Result<TargetVolumeExtents>::failure(
                windows_execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"IOCTL_STORAGE_GET_DEVICE_NUMBER(provisioning)",
                    L"Windows returned truncated volume device identity",
                    ERROR_INVALID_DATA));
        }
        STORAGE_DEVICE_NUMBER storage_number{};
        std::memcpy(
            &storage_number,
            storage_number_bytes.value().data(),
            sizeof(storage_number));

        if (volume_requires_disk_extents(storage_number.DeviceType)) {
        std::vector<std::byte> extent_bytes(4096);
        bool queried = false;
        for (;;) {
            auto extent_result = bounded_device_io_control(
                volume.get(),
                IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                {},
                extent_bytes.size(),
                ProvisioningExecutionError::open_failed,
                L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)");
            if (extent_result.ok()) {
                extent_bytes = extent_result.move_value();
                queried = true;
                break;
            }
            const DWORD error = extent_result.error().native_code;
            if ((error != ERROR_MORE_DATA
                    && error != ERROR_INSUFFICIENT_BUFFER)
                || extent_bytes.size() >= 1024ULL * 1024ULL) {
                return Result<TargetVolumeExtents>::failure(
                    windows_execution_failure(
                        ProvisioningExecutionError::open_failed,
                        L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                        L"Could not prove that a volume avoids the target disk",
                        error));
            }
            extent_bytes.resize(extent_bytes.size() * 2);
        }
        const auto extent_size = extent_bytes.size();
        if (!queried || extent_size < offsetof(VOLUME_DISK_EXTENTS, Extents)) {
            return Result<TargetVolumeExtents>::failure(
                execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                    L"Windows returned truncated volume extent evidence"));
        }
        const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(
            extent_bytes.data());
        if (extents->NumberOfDiskExtents
            > (extent_size - offsetof(VOLUME_DISK_EXTENTS, Extents))
                / sizeof(DISK_EXTENT)) {
            return Result<TargetVolumeExtents>::failure(
                execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                    L"A volume extent count exceeded its evidence buffer"));
        }
        bool uses_target = false;
        std::uint32_t target_extents = 0;
        for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
            if (extents->Extents[index].DiskNumber != disk_number) continue;
            const auto& extent = extents->Extents[index];
            if (extent.StartingOffset.QuadPart < 0
                || extent.ExtentLength.QuadPart <= 0) {
                return Result<TargetVolumeExtents>::failure(
                    execution_failure(
                        ProvisioningExecutionError::open_failed,
                        L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                        L"The target volume extent has invalid geometry"));
            }
            uses_target = true;
            if (target_extents
                == (std::numeric_limits<std::uint32_t>::max)()) {
                return Result<TargetVolumeExtents>::failure(
                    execution_failure(
                        ProvisioningExecutionError::open_failed,
                        L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                        L"The target extent count overflowed"));
            }
            if (volume_count == 0 && target_extents == 0) {
                sole_starting_offset = static_cast<std::uint64_t>(
                    extent.StartingOffset.QuadPart);
                sole_length_bytes = static_cast<std::uint64_t>(
                    extent.ExtentLength.QuadPart);
            }
            ++target_extents;
        }
        if (uses_target) {
            if (volume_count
                    == (std::numeric_limits<std::uint32_t>::max)()
                || extent_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                        - target_extents) {
                return Result<TargetVolumeExtents>::failure(
                    execution_failure(
                        ProvisioningExecutionError::open_failed,
                        L"IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(provisioning)",
                        L"The target volume count overflowed"));
            }
            ++volume_count;
            extent_count += target_extents;
        }
        }

        if (!FindNextVolumeW(
                enumeration,
                volume_path.data(),
                static_cast<DWORD>(volume_path.size()))) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) break;
            return Result<TargetVolumeExtents>::failure(
                windows_execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"FindNextVolumeW(provisioning)",
                    L"Volume enumeration ended without complete evidence",
                    error));
        }
    }
    return Result<TargetVolumeExtents>::success({
        volume_count,
        extent_count,
        volume_count == 1 && extent_count == 1,
        sole_starting_offset,
        sole_length_bytes});
}

Result<RetainedDiskSnapshot> inspect_retained_disk(
    HANDLE handle,
    std::uint32_t expected_number) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return Result<RetainedDiskSnapshot>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::open_failed,
                L"inspect_retained_disk",
                L"The retained PhysicalDrive handle is invalid",
                ERROR_INVALID_HANDLE));
    }
    auto device = query_disk_ioctl<STORAGE_DEVICE_NUMBER>(
        handle,
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        L"IOCTL_STORAGE_GET_DEVICE_NUMBER(provisioning)");
    if (!device.ok()) {
        return Result<RetainedDiskSnapshot>::failure(device.error());
    }
    if (device.value().DeviceType != kStorageDeviceTypeDisk
        || device.value().DeviceNumber != expected_number) {
        return policy_failure<RetainedDiskSnapshot>(
            ProvisioningPolicyError::identity_conflict,
            L"The retained handle is not the expected disk device number");
    }
    auto address = query_disk_ioctl<SCSI_ADDRESS>(
        handle,
        IOCTL_SCSI_GET_ADDRESS,
        L"IOCTL_SCSI_GET_ADDRESS(provisioning)");
    if (!address.ok()) {
        return Result<RetainedDiskSnapshot>::failure(address.error());
    }
    if (address.value().Length != sizeof(SCSI_ADDRESS)) {
        return Result<RetainedDiskSnapshot>::failure(
            windows_execution_failure(
                ProvisioningExecutionError::open_failed,
                L"IOCTL_SCSI_GET_ADDRESS(provisioning)",
                L"Windows returned an invalid SCSI address length",
                ERROR_INVALID_DATA));
    }
    auto length = query_disk_ioctl<GET_LENGTH_INFORMATION>(
        handle,
        IOCTL_DISK_GET_LENGTH_INFO,
        L"IOCTL_DISK_GET_LENGTH_INFO(provisioning)");
    if (!length.ok()) {
        return Result<RetainedDiskSnapshot>::failure(length.error());
    }
    if (length.value().Length.QuadPart <= 0) {
        return policy_failure<RetainedDiskSnapshot>(
            ProvisioningPolicyError::identity_conflict,
            L"The retained disk has an invalid byte length");
    }

    auto layout_bytes = query_drive_layout(handle);
    if (!layout_bytes.ok()) {
        return Result<RetainedDiskSnapshot>::failure(layout_bytes.error());
    }
    const auto* layout =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            layout_bytes.value().data());
    const auto observed_layout = classify_drive_layout(
        static_cast<std::uint32_t>(layout->PartitionStyle),
        layout->PartitionCount,
        layout->Mbr.Signature,
        layout->Mbr.CheckSum);
    GUID observed_disk_guid{};
    GUID observed_partition_guid{};
    GUID observed_partition_type{};
    std::uint64_t observed_partition_start = 0;
    std::uint64_t observed_partition_length = 0;
    std::uint64_t observed_partition_attributes = 0;
    const bool has_disk_guid = observed_layout == ObservedDiskLayout::gpt;
    bool has_partition_guid = false;
    if (has_disk_guid) {
        observed_disk_guid = layout->Gpt.DiskId;
        if (layout->PartitionCount == 1
            && layout->PartitionEntry[0].PartitionStyle
                == PARTITION_STYLE_GPT) {
            observed_partition_guid =
                layout->PartitionEntry[0].Gpt.PartitionId;
            observed_partition_type =
                layout->PartitionEntry[0].Gpt.PartitionType;
            if (layout->PartitionEntry[0].StartingOffset.QuadPart >= 0
                && layout->PartitionEntry[0].PartitionLength.QuadPart > 0) {
                observed_partition_start = static_cast<std::uint64_t>(
                    layout->PartitionEntry[0].StartingOffset.QuadPart);
                observed_partition_length = static_cast<std::uint64_t>(
                    layout->PartitionEntry[0].PartitionLength.QuadPart);
            }
            observed_partition_attributes =
                layout->PartitionEntry[0].Gpt.Attributes;
            has_partition_guid = true;
        }
    }

    auto attributes = query_disk_ioctl<GET_DISK_ATTRIBUTES>(
        handle,
        IOCTL_DISK_GET_DISK_ATTRIBUTES,
        L"IOCTL_DISK_GET_DISK_ATTRIBUTES(provisioning)");
    if (!attributes.ok()) {
        return Result<RetainedDiskSnapshot>::failure(attributes.error());
    }
    auto volumes = count_volume_extents_for_disk(
        device.value().DeviceNumber);
    if (!volumes.ok()) {
        return Result<RetainedDiskSnapshot>::failure(volumes.error());
    }
    return Result<RetainedDiskSnapshot>::success({
        {device.value().DeviceNumber,
         physical_drive_path(device.value().DeviceNumber),
         static_cast<std::uint64_t>(length.value().Length.QuadPart),
         address.value().PortNumber,
         address.value().PathId,
         address.value().TargetId,
         address.value().Lun},
        observed_layout,
        layout->PartitionCount,
        volumes.value().volume_count,
        volumes.value().extent_count,
        observed_disk_guid,
        observed_partition_guid,
        observed_partition_type,
        has_disk_guid,
        has_partition_guid,
        observed_partition_start,
        observed_partition_length,
        observed_partition_attributes,
        volumes.value().has_single_extent,
        volumes.value().single_starting_offset,
        volumes.value().single_length_bytes,
        (attributes.value().Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0,
        (attributes.value().Attributes & DISK_ATTRIBUTE_OFFLINE) != 0});
}

Result<GUID> windows_guid(const product::Uuid& value) {
    std::wstring text = L"{" + widen_ascii(value.to_string()) + L"}";
    GUID guid{};
    const HRESULT converted = CLSIDFromString(text.data(), &guid);
    if (FAILED(converted)) {
        return Result<GUID>::failure(execution_failure(
            ProvisioningExecutionError::platform_exception,
            L"CLSIDFromString(provisioning)",
            L"Failed to convert an operation-derived GPT identifier"));
    }
    return Result<GUID>::success(guid);
}

Result<NativeGptLayoutPlan> apply_gpt_layout_on_retained_handle(
    HANDLE handle,
    const product::Uuid& operation_id,
    std::uint64_t expected_length_bytes,
    std::optional<NativeGptLayoutPlan>& attempted_plan) {
    auto ids = derive_gpt_ids(operation_id);
    if (!ids.ok()) {
        return Result<NativeGptLayoutPlan>::failure(ids.error());
    }
    auto disk_guid = windows_guid(ids.value().disk_guid);
    if (!disk_guid.ok()) {
        return Result<NativeGptLayoutPlan>::failure(disk_guid.error());
    }
    auto partition_guid = windows_guid(ids.value().partition_guid);
    if (!partition_guid.ok()) {
        return Result<NativeGptLayoutPlan>::failure(partition_guid.error());
    }

    CREATE_DISK create{};
    create.PartitionStyle = PARTITION_STYLE_GPT;
    create.Gpt.DiskId = disk_guid.value();
    create.Gpt.MaxPartitionCount = 128;
    const auto create_input = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(&create), sizeof(create));
    auto created = bounded_device_io_control(
        handle,
        IOCTL_DISK_CREATE_DISK,
        create_input,
        0,
        ProvisioningExecutionError::process_failed,
        L"IOCTL_DISK_CREATE_DISK(provisioning)");
    if (!created.ok()) {
        return Result<NativeGptLayoutPlan>::failure(created.error());
    }

    auto empty_layout = query_drive_layout(handle);
    if (!empty_layout.ok()) {
        return Result<NativeGptLayoutPlan>::failure(empty_layout.error());
    }
    const auto* current =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            empty_layout.value().data());
    if (current->PartitionStyle != PARTITION_STYLE_GPT
        || current->PartitionCount != 0
        || !IsEqualGUID(current->Gpt.DiskId, disk_guid.value())
        || current->Gpt.StartingUsableOffset.QuadPart < 0
        || current->Gpt.UsableLength.QuadPart <= 0) {
        return Result<NativeGptLayoutPlan>::failure(
            execution_failure(
                ProvisioningExecutionError::process_failed,
                L"IOCTL_DISK_CREATE_DISK(provisioning)",
                L"The retained disk did not expose the planned empty GPT"));
    }

    auto geometry_result = bounded_device_io_control(
        handle,
        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        {},
        sizeof(DISK_GEOMETRY_EX) + 1024,
        ProvisioningExecutionError::process_failed,
        L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX(provisioning)");
    if (!geometry_result.ok()) {
        return Result<NativeGptLayoutPlan>::failure(
            geometry_result.error());
    }
    if (geometry_result.value().size()
        < offsetof(DISK_GEOMETRY_EX, Data)) {
        return Result<NativeGptLayoutPlan>::failure(execution_failure(
            ProvisioningExecutionError::process_failed,
            L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX(provisioning)",
            L"Windows returned truncated sector geometry"));
    }
    const auto* geometry = reinterpret_cast<const DISK_GEOMETRY_EX*>(
        geometry_result.value().data());
    if (geometry->DiskSize.QuadPart <= 0
        || static_cast<std::uint64_t>(geometry->DiskSize.QuadPart)
            != expected_length_bytes) {
        return Result<NativeGptLayoutPlan>::failure(
            execution_failure(
                ProvisioningExecutionError::process_failed,
                L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX(provisioning)",
                L"The retained disk length changed at the mutation boundary"));
    }
    auto plan = plan_gpt_layout(
        operation_id,
        expected_length_bytes,
        static_cast<std::uint64_t>(
            current->Gpt.StartingUsableOffset.QuadPart),
        static_cast<std::uint64_t>(current->Gpt.UsableLength.QuadPart),
        geometry->Geometry.BytesPerSector);
    if (!plan.ok()) return plan;
    attempted_plan = plan.value();

    constexpr std::size_t kLayoutBytes =
        offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)
        + sizeof(PARTITION_INFORMATION_EX);
    std::array<std::byte, kLayoutBytes> new_layout_bytes{};
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(
        new_layout_bytes.data());
    layout->PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionCount = 1;
    layout->Gpt = current->Gpt;
    layout->Gpt.DiskId = disk_guid.value();
    auto& partition = layout->PartitionEntry[0];
    partition.PartitionStyle = PARTITION_STYLE_GPT;
    partition.StartingOffset.QuadPart = static_cast<LONGLONG>(
        plan.value().partition_starting_offset);
    partition.PartitionLength.QuadPart = static_cast<LONGLONG>(
        plan.value().partition_length_bytes);
    partition.PartitionNumber = 1;
    partition.RewritePartition = TRUE;
    partition.Gpt.PartitionType = kBasicDataPartitionGuid;
    partition.Gpt.PartitionId = partition_guid.value();
    partition.Gpt.Attributes = GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
    const auto layout_input = std::span<const std::byte>(
        new_layout_bytes.data(), new_layout_bytes.size());
    auto written_request = bounded_device_io_control(
        handle,
        IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
        layout_input,
        0,
        ProvisioningExecutionError::process_failed,
        L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX(provisioning)");
    if (!written_request.ok()) {
        return Result<NativeGptLayoutPlan>::failure(
            written_request.error());
    }
    auto updated = bounded_device_io_control(
        handle,
        IOCTL_DISK_UPDATE_PROPERTIES,
        {},
        0,
        ProvisioningExecutionError::process_failed,
        L"IOCTL_DISK_UPDATE_PROPERTIES(provisioning)");
    if (!updated.ok()) {
        return Result<NativeGptLayoutPlan>::failure(updated.error());
    }

    auto written_layout = query_drive_layout(handle);
    if (!written_layout.ok()) {
        return Result<NativeGptLayoutPlan>::failure(written_layout.error());
    }
    const auto* written =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            written_layout.value().data());
    if (written->PartitionStyle != PARTITION_STYLE_GPT
        || written->PartitionCount != 1
        || !IsEqualGUID(written->Gpt.DiskId, disk_guid.value())) {
        return Result<NativeGptLayoutPlan>::failure(execution_failure(
            ProvisioningExecutionError::process_failed,
            L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX(provisioning)",
            L"The retained disk GPT did not match the written plan"));
    }
    const auto& written_partition = written->PartitionEntry[0];
    if (written_partition.PartitionStyle != PARTITION_STYLE_GPT
        || written_partition.PartitionNumber != 1
        || !IsEqualGUID(
            written_partition.Gpt.PartitionType,
            kBasicDataPartitionGuid)
        || !IsEqualGUID(
            written_partition.Gpt.PartitionId,
            partition_guid.value())
        || written_partition.Gpt.Attributes
            != GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER
        || written_partition.StartingOffset.QuadPart
            != static_cast<LONGLONG>(
                plan.value().partition_starting_offset)
        || written_partition.PartitionLength.QuadPart
            != static_cast<LONGLONG>(
                plan.value().partition_length_bytes)) {
        return Result<NativeGptLayoutPlan>::failure(execution_failure(
            ProvisioningExecutionError::process_failed,
            L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX(provisioning)",
            L"The retained disk partition did not match the written plan"));
    }
    return plan;
}

Result<std::wstring> trusted_windows_directory() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const UINT length = GetWindowsDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return Result<std::wstring>::failure(windows_execution_failure(
            ProvisioningExecutionError::open_failed,
            L"GetWindowsDirectoryW(provisioning)",
            L"Failed to locate the trusted Windows directory"));
    }
    return Result<std::wstring>::success(
        std::wstring(buffer.data(), length));
}

Result<void> require_exact_retained_disk(
    const RetainedDiskSnapshot& snapshot,
    const PhysicalDiskIdentity& expected) {
    if (snapshot.physical_disk != expected) {
        return Result<void>::failure(policy_failure<RetainedDiskSnapshot>(
            ProvisioningPolicyError::identity_conflict,
            L"The retained PhysicalDrive identity changed").error());
    }
    return Result<void>::success();
}

class WindowsProvisioningSession final : public IProvisioningSession {
public:
    WindowsProvisioningSession(
        UniqueHandle disk_handle,
        NewDiskProvisioningExpectation expected) noexcept
        : disk_handle_(std::move(disk_handle)),
          expected_(std::move(expected)) {}

    Result<NewDiskObservation> observe(
        const OperationContext& context) override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<NewDiskObservation>::failure(live.error());
        }
        auto snapshot = inspect_retained_disk(
            disk_handle_.get(),
            expected_.physical_disk.physical_drive_number);
        if (!snapshot.ok()) {
            return Result<NewDiskObservation>::failure(snapshot.error());
        }
        auto exact = require_exact_retained_disk(
            snapshot.value(), expected_.physical_disk);
        if (!exact.ok()) {
            return Result<NewDiskObservation>::failure(exact.error());
        }
        auto directory = trusted_windows_directory();
        if (!directory.ok()) {
            return Result<NewDiskObservation>::failure(directory.error());
        }
        auto command = build_disk_observation_command(
            expected_.physical_disk, directory.value());
        if (!command.ok()) {
            return Result<NewDiskObservation>::failure(command.error());
        }
        auto process = run_windows_process(command.value(), context);
        if (!process.ok()) {
            return Result<NewDiskObservation>::failure(process.error());
        }
        if (process.value().timed_out) {
            return Result<NewDiskObservation>::failure(execution_failure(
                ProvisioningExecutionError::timed_out,
                L"provision.observe",
                L"The read-only disk observer timed out"));
        }
        if (process.value().stdout_truncated
            || process.value().stderr_truncated) {
            return Result<NewDiskObservation>::failure(execution_failure(
                ProvisioningExecutionError::output_truncated,
                L"provision.observe",
                L"The read-only disk observer output was truncated"));
        }
        if (process.value().exit_code != 0) {
            return Result<NewDiskObservation>::failure(execution_failure(
                ProvisioningExecutionError::process_failed,
                L"provision.observe",
                L"The read-only disk observer failed"));
        }
        auto observed = parse_disk_observation(
            snapshot.value().physical_disk,
            snapshot.value().layout,
            snapshot.value().partition_count,
            snapshot.value().volume_count,
            snapshot.value().extent_count,
            snapshot.value().is_read_only,
            snapshot.value().is_offline,
            process.value().stdout_text);
        if (!observed.ok()) return observed;

        const std::array observations{observed.value()};
        auto verified = assess_new_disk_provisioning(
            expected_, observations);
        if (!verified.ok()) {
            return Result<NewDiskObservation>::failure(verified.error());
        }
        authorized_command_ = build_provisioning_command(
            verified.value(), directory.value());
        if (!authorized_command_->ok()) {
            const auto error = authorized_command_->error();
            authorized_command_.reset();
            return Result<NewDiskObservation>::failure(error);
        }
        return observed;
    }

    Result<ProvisioningProcessResult> run(
        const ProvisioningCommand& command,
        const OperationContext& context) override {
        if (!authorized_command_.has_value()
            || !authorized_command_->ok()
            || command != authorized_command_->value()) {
            return Result<ProvisioningProcessResult>::failure(
                execution_failure(
                    ProvisioningExecutionError::process_failed,
                    L"provision.run",
                    L"The child command was not authorized by this preflight"));
        }
        // Consume authorization before the destructive launch. A failure or
        // partial mutation can never be retried through the same session.
        authorized_command_.reset();
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<ProvisioningProcessResult>::failure(live.error());
        }
        auto snapshot = inspect_retained_disk(
            disk_handle_.get(),
            expected_.physical_disk.physical_drive_number);
        if (!snapshot.ok()) {
            return Result<ProvisioningProcessResult>::failure(snapshot.error());
        }
        auto exact = require_exact_retained_disk(
            snapshot.value(), expected_.physical_disk);
        if (!exact.ok()) {
            return Result<ProvisioningProcessResult>::failure(exact.error());
        }
        if (snapshot.value().layout != ObservedDiskLayout::raw
            || snapshot.value().partition_count != 0
            || snapshot.value().volume_count != 0
            || snapshot.value().extent_count != 0
            || snapshot.value().is_read_only
            || snapshot.value().is_offline) {
            return Result<ProvisioningProcessResult>::failure(
                policy_failure<RetainedDiskSnapshot>(
                    ProvisioningPolicyError::identity_conflict,
                    L"The disk changed after provisioning preflight").error());
        }
        auto planned = apply_gpt_layout_on_retained_handle(
            disk_handle_.get(),
            expected_.operation_id,
            expected_.physical_disk.length_bytes,
            attempted_layout_);
        if (!planned.ok()) {
            if (planned.error().code == static_cast<std::uint32_t>(
                    ProvisioningExecutionError::io_indeterminate)) {
                mutation_io_indeterminate_ = true;
            }
            return Result<ProvisioningProcessResult>::failure(
                planned.error());
        }
        // GPT creation is the destructive boundary. Cancellation after this
        // point cannot safely turn an in-flight format into an unobserved
        // early return, so the bounded child runs to a definite result.
        return run_windows_process(command, OperationContext{});
    }

    Result<ProvisioningMutationAudit> audit_after_mutation(
        const OperationContext& context) override {
        if (mutation_io_indeterminate_) {
            return Result<ProvisioningMutationAudit>::failure(
                execution_failure(
                    ProvisioningExecutionError::io_indeterminate,
                    L"provision.audit_after_mutation",
                    L"A bounded mutation IOCTL remains indeterminate after cancellation"));
        }
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<ProvisioningMutationAudit>::failure(live.error());
        }
        auto snapshot = inspect_retained_disk(
            disk_handle_.get(),
            expected_.physical_disk.physical_drive_number);
        if (!snapshot.ok()) {
            return Result<ProvisioningMutationAudit>::failure(
                snapshot.error());
        }
        auto exact = require_exact_retained_disk(
            snapshot.value(), expected_.physical_disk);
        if (!exact.ok()) {
            return Result<ProvisioningMutationAudit>::failure(exact.error());
        }
        auto planned = derive_gpt_ids(expected_.operation_id);
        if (!planned.ok()) {
            return Result<ProvisioningMutationAudit>::failure(
                planned.error());
        }
        auto planned_disk_guid = windows_guid(planned.value().disk_guid);
        auto planned_partition_guid = windows_guid(
            planned.value().partition_guid);
        if (!planned_disk_guid.ok()) {
            return Result<ProvisioningMutationAudit>::failure(
                planned_disk_guid.error());
        }
        if (!planned_partition_guid.ok()) {
            return Result<ProvisioningMutationAudit>::failure(
                planned_partition_guid.error());
        }
        const bool disk_matches = snapshot.value().has_gpt_disk_guid
            && IsEqualGUID(
                snapshot.value().gpt_disk_guid,
                planned_disk_guid.value());
        const bool partition_matches =
            snapshot.value().has_first_partition_guid
            && IsEqualGUID(
                snapshot.value().first_partition_guid,
                planned_partition_guid.value());
        const bool partition_layout_matches =
            attempted_layout_.has_value()
            && partition_matches
            && IsEqualGUID(
                snapshot.value().first_partition_type,
                kBasicDataPartitionGuid)
            && snapshot.value().first_partition_starting_offset
                == attempted_layout_->partition_starting_offset
            && snapshot.value().first_partition_length_bytes
                == attempted_layout_->partition_length_bytes
            && snapshot.value().first_partition_attributes
                == GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
        const bool extent_matches = partition_layout_matches
            && snapshot.value().has_single_volume_extent
            && snapshot.value().single_volume_starting_offset
                == attempted_layout_->partition_starting_offset
            && snapshot.value().single_volume_length_bytes
                == attempted_layout_->partition_length_bytes;
        ProvisioningMutationState state =
            ProvisioningMutationState::unexpected_layout;
        if (snapshot.value().layout == ObservedDiskLayout::raw
            && snapshot.value().partition_count == 0
            && snapshot.value().volume_count == 0
            && snapshot.value().extent_count == 0) {
            state = ProvisioningMutationState::raw_unchanged;
        } else if (snapshot.value().layout == ObservedDiskLayout::gpt
            && disk_matches
            && snapshot.value().partition_count == 0
            && snapshot.value().volume_count == 0
            && snapshot.value().extent_count == 0) {
            state = ProvisioningMutationState::planned_gpt_empty;
        } else if (snapshot.value().layout == ObservedDiskLayout::gpt
            && disk_matches && partition_layout_matches
            && snapshot.value().partition_count == 1
            && snapshot.value().volume_count == 0
            && snapshot.value().extent_count == 0) {
            state = ProvisioningMutationState::planned_partition_no_volume;
        } else if (snapshot.value().layout == ObservedDiskLayout::gpt
            && disk_matches && partition_layout_matches && extent_matches
            && snapshot.value().partition_count == 1
            && snapshot.value().volume_count == 1
            && snapshot.value().extent_count == 1) {
            state = ProvisioningMutationState::planned_partition_with_volume;
        }
        return Result<ProvisioningMutationAudit>::success({
            state,
            snapshot.value().physical_disk,
            snapshot.value().layout,
            snapshot.value().partition_count,
            snapshot.value().volume_count,
            snapshot.value().extent_count,
            disk_matches,
            partition_matches,
            partition_layout_matches,
            extent_matches});
    }

    Result<VerifiedVolumeIdentity> verify_postcondition(
        const VolumeIdentityExpectation& expected,
        const OperationContext& context) override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<VerifiedVolumeIdentity>::failure(live.error());
        }
        if (expected.owner != expected_.owner
            || expected.physical_disk != expected_.physical_disk) {
            return Result<VerifiedVolumeIdentity>::failure(
                policy_failure<RetainedDiskSnapshot>(
                    ProvisioningPolicyError::identity_conflict,
                    L"The provisioning postcondition owner changed").error());
        }
        auto snapshot = inspect_retained_disk(
            disk_handle_.get(),
            expected_.physical_disk.physical_drive_number);
        if (!snapshot.ok()) {
            return Result<VerifiedVolumeIdentity>::failure(snapshot.error());
        }
        auto exact = require_exact_retained_disk(
            snapshot.value(), expected_.physical_disk);
        if (!exact.ok()) {
            return Result<VerifiedVolumeIdentity>::failure(exact.error());
        }
        return verify_windows_volume_identity(expected);
    }

private:
    UniqueHandle disk_handle_;
    NewDiskProvisioningExpectation expected_;
    std::optional<Result<ProvisioningCommand>> authorized_command_;
    std::optional<NativeGptLayoutPlan> attempted_layout_;
    bool mutation_io_indeterminate_{};
};

class WindowsProvisioningPlatform final : public IProvisioningPlatform {
public:
    Result<std::unique_ptr<IProvisioningSession>> open_exact(
        const NewDiskProvisioningExpectation& expected,
        const OperationContext& context) override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                live.error());
        }
        if (!valid_expectation(expected)
            || expected.lifecycle != ProvisioningLifecycle::provisioning
            || expected.source != ProvisioningSource::new_image
            || !expected.operation_owns_session
            || !expected.device_created_by_operation
            || expected.has_recorded_disk_identity
            || expected.has_recorded_partition_identity
            || expected.has_recorded_volume_identity) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                policy_failure<VerifiedNewDisk>(
                    ProvisioningPolicyError::invalid_expectation,
                    L"The Windows provisioning gate rejected the expectation")
                    .error());
        }
        UniqueHandle disk(CreateFileW(
            expected.physical_disk.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (!disk) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                windows_execution_failure(
                    ProvisioningExecutionError::open_failed,
                    L"CreateFileW(PhysicalDrive provisioning)",
                    L"Failed to retain the exact provisioning disk"));
        }
        auto snapshot = inspect_retained_disk(
            disk.get(), expected.physical_disk.physical_drive_number);
        if (!snapshot.ok()) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                snapshot.error());
        }
        auto exact = require_exact_retained_disk(
            snapshot.value(), expected.physical_disk);
        if (!exact.ok()) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                exact.error());
        }
        std::unique_ptr<IProvisioningSession> session =
            std::make_unique<WindowsProvisioningSession>(
                std::move(disk), expected);
        return Result<std::unique_ptr<IProvisioningSession>>::success(
            std::move(session));
    }

    Result<std::wstring> windows_directory() override {
        return trusted_windows_directory();
    }
};

template <typename Function>
auto invoke_provisioning_platform(
    Function&& function,
    const wchar_t* operation) -> decltype(function()) {
    using ResultType = decltype(function());
    try {
        return function();
    } catch (...) {
        return ResultType::failure(execution_failure(
            ProvisioningExecutionError::platform_exception,
            operation,
            L"The provisioning platform threw an exception"));
    }
}

bool begin_provisioning_step(
    const OperationContext& context,
    ProvisioningOutcome& outcome) {
    auto live = check_operation_context(context);
    if (live.ok()) return true;
    outcome.error = live.error();
    return false;
}

} // namespace

struct ProvisionedDiskProofFactory final {
    static ProvisionedDisk create(
        product::Uuid operation_id,
        VerifiedVolumeIdentity identity) {
        return ProvisionedDisk{
            std::move(operation_id), std::move(identity)};
    }
};

VerifiedNewDisk::VerifiedNewDisk(
    NewDiskProvisioningExpectation expectation,
    NewDiskObservation observation) noexcept
    : expectation_(std::move(expectation)),
      observation_(std::move(observation)) {}

const HostOwnershipMarker& VerifiedNewDisk::owner() const noexcept {
    return expectation_.owner;
}

const product::Uuid& VerifiedNewDisk::operation_id() const noexcept {
    return expectation_.operation_id;
}

const PhysicalDiskIdentity& VerifiedNewDisk::physical_disk() const noexcept {
    return expectation_.physical_disk;
}

const NewDiskObservation& VerifiedNewDisk::observation() const noexcept {
    return observation_;
}

Result<VerifiedNewDisk> assess_new_disk_provisioning(
    const NewDiskProvisioningExpectation& expected,
    std::span<const NewDiskObservation> observations) {
    if (!valid_expectation(expected)) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::invalid_expectation,
            L"The new-disk expectation is incomplete or contradictory");
    }
    if (expected.lifecycle != ProvisioningLifecycle::provisioning) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::wrong_lifecycle,
            L"Only a Provisioning profile may initialize a disk");
    }
    if (expected.source != ProvisioningSource::new_image) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::source_not_new,
            L"Only a newly created image may initialize a disk");
    }
    if (!expected.operation_owns_session
        || !expected.device_created_by_operation) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::ownership_unproven,
            L"The current operation does not own the exact worker and device");
    }
    if (expected.has_recorded_disk_identity
        || expected.has_recorded_partition_identity
        || expected.has_recorded_volume_identity) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::already_provisioned,
            L"Provisioned identity already exists; reformat is forbidden");
    }

    const NewDiskObservation* exact = nullptr;
    bool conflict = false;
    for (const auto& observed : observations) {
        if (observed.physical_disk == expected.physical_disk) {
            if (exact != nullptr) {
                return policy_failure<VerifiedNewDisk>(
                    ProvisioningPolicyError::ambiguous,
                    L"Multiple observations match the exact new disk");
            }
            exact = &observed;
        } else if (shares_authoritative_anchor(
                       expected.physical_disk,
                       observed.physical_disk)) {
            conflict = true;
        }
    }
    if (conflict) {
        return policy_failure<VerifiedNewDisk>(
            exact == nullptr ? ProvisioningPolicyError::identity_conflict
                             : ProvisioningPolicyError::ambiguous,
            L"A disk number or complete SCSI address has conflicting identity");
    }
    if (exact == nullptr) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::not_found,
            L"The exact newly owned disk was not found");
    }
    if (exact->layout != ObservedDiskLayout::raw) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::not_raw,
            L"Only a RAW disk may be provisioned");
    }
    if (exact->partition_count != 0 || exact->volume_count != 0
        || exact->extent_count != 0) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::not_empty,
            L"The disk already contains a partition, volume, or extent");
    }
    if (protected_disk(exact->protection)) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::protected_disk,
            L"The disk is protected by a system role");
    }
    if (exact->protection.is_read_only) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::read_only,
            L"The exact disk is read-only");
    }
    if (exact->protection.is_offline) {
        return policy_failure<VerifiedNewDisk>(
            ProvisioningPolicyError::offline,
            L"The exact disk is offline");
    }
    return Result<VerifiedNewDisk>::success(
        VerifiedNewDisk{expected, *exact});
}

Result<ProvisioningCommand> build_provisioning_command(
    const VerifiedNewDisk& verified,
    std::wstring_view windows_directory) {
    if (!valid_windows_directory(windows_directory)) {
        return policy_failure<ProvisioningCommand>(
            ProvisioningPolicyError::invalid_environment,
            L"The trusted Windows directory is not canonical");
    }
    auto planned = derive_gpt_ids(verified.operation_id());
    if (!planned.ok()) {
        return Result<ProvisioningCommand>::failure(planned.error());
    }
    std::wstring executable(windows_directory);
    executable +=
        L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::vector<std::wstring> arguments{
        L"-NoLogo",
        L"-NoProfile",
        L"-NonInteractive",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-EncodedCommand",
        encode_powershell_command(provisioning_script(
            verified, planned.value()))};
    return Result<ProvisioningCommand>::success({
        std::move(executable),
        std::move(arguments),
        std::chrono::minutes(5),
        64ULL * 1024ULL,
        64ULL * 1024ULL,
        "provision.gpt_ntfs"});
}

Result<ProvisionalProvisioningIdentity> parse_provisioning_result(
    std::string_view output) {
    constexpr std::size_t kMaximumOutputBytes = 64ULL * 1024ULL;
    auto malformed = [] {
        return policy_failure<ProvisionalProvisioningIdentity>(
            ProvisioningPolicyError::malformed_result,
            L"The provisioning process returned malformed identity output");
    };
    if (output.empty() || output.size() > kMaximumOutputBytes
        || output.find('\0') != std::string_view::npos) {
        return malformed();
    }

    std::array<std::string_view, 4> lines{};
    std::size_t line_count = 0;
    std::size_t start = 0;
    while (start < output.size()) {
        if (line_count == lines.size()) return malformed();
        const auto newline = output.find('\n', start);
        auto line = output.substr(
            start,
            newline == std::string_view::npos
                ? output.size() - start
                : newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return malformed();
        lines[line_count++] = line;
        if (newline == std::string_view::npos) {
            start = output.size();
        } else {
            start = newline + 1;
        }
    }
    if (line_count != lines.size()
        || lines[0] != "MYRD_PROVISIONING_V1") {
        return malformed();
    }

    const auto parse_line = [&](std::size_t index, std::string_view prefix)
        -> Result<product::Uuid> {
        const auto line = lines[index];
        const auto malformed_uuid = [] {
            return policy_failure<product::Uuid>(
                ProvisioningPolicyError::malformed_result,
                L"The provisioning process returned malformed identity output");
        };
        if (!line.starts_with(prefix)) return malformed_uuid();
        const auto value_text = line.substr(prefix.size());
        auto value = product::Uuid::parse(value_text);
        if (!value.ok() || value.value().to_string() != value_text
            || is_nil(value.value())) {
            return malformed_uuid();
        }
        return value;
    };
    auto disk_guid = parse_line(1, "disk_guid=");
    if (!disk_guid.ok()) return malformed();
    auto partition_guid = parse_line(2, "partition_guid=");
    if (!partition_guid.ok()) return malformed();
    auto volume_guid = parse_line(3, "volume_guid=");
    if (!volume_guid.ok()) return malformed();
    return Result<ProvisionalProvisioningIdentity>::success({
        disk_guid.move_value(),
        partition_guid.move_value(),
        volume_guid.move_value()});
}

ProvisionedDisk::ProvisionedDisk(
    product::Uuid operation_id,
    VerifiedVolumeIdentity identity) noexcept
    : operation_id_(std::move(operation_id)),
      identity_(std::move(identity)) {}

const product::Uuid& ProvisionedDisk::operation_id() const noexcept {
    return operation_id_;
}

const VerifiedVolumeIdentity& ProvisionedDisk::identity() const noexcept {
    return identity_;
}

ProvisioningOutcome provision_new_disk(
    IProvisioningPlatform& platform,
    const NewDiskProvisioningExpectation& expected,
    const OperationContext& context) {
    ProvisioningOutcome outcome{};
    if (!begin_provisioning_step(context, outcome)) return outcome;

    auto opened = invoke_provisioning_platform(
        [&] { return platform.open_exact(expected, context); },
        L"provision.open_exact");
    if (!opened.ok()) {
        outcome.error = opened.error();
        return outcome;
    }
    auto session = opened.move_value();
    if (!session) {
        outcome.error = execution_failure(
            ProvisioningExecutionError::platform_exception,
            L"provision.open_exact",
            L"The provisioning platform returned a null session");
        return outcome;
    }
    outcome.opened = true;

    auto observed = invoke_provisioning_platform(
        [&] { return session->observe(context); },
        L"provision.observe");
    if (!observed.ok()) {
        outcome.error = observed.error();
        return outcome;
    }
    const std::array observations{observed.value()};
    auto verified = assess_new_disk_provisioning(expected, observations);
    if (!verified.ok()) {
        outcome.error = verified.error();
        return outcome;
    }
    outcome.preflight_verified = true;

    auto windows_directory = invoke_provisioning_platform(
        [&] { return platform.windows_directory(); },
        L"provision.windows_directory");
    if (!windows_directory.ok()) {
        outcome.error = windows_directory.error();
        return outcome;
    }
    auto command = build_provisioning_command(
        verified.value(), windows_directory.value());
    if (!command.ok()) {
        outcome.error = command.error();
        return outcome;
    }
    if (!begin_provisioning_step(context, outcome)) return outcome;

    outcome.process_started = true;
    auto process = invoke_provisioning_platform(
        [&] { return session->run(command.value(), context); },
        L"provision.run");
    const auto fail_after_mutation = [&](Error error) {
        outcome.error = std::move(error);
        auto audited = invoke_provisioning_platform(
            [&] {
                return session->audit_after_mutation(OperationContext{});
            },
            L"provision.audit_after_mutation");
        outcome.postmutation_audited = audited.ok();
        if (audited.ok()) {
            outcome.audit = audited.move_value();
        } else {
            outcome.audit_error = audited.error();
        }
        return outcome;
    };
    if (!process.ok()) {
        return fail_after_mutation(process.error());
    }
    outcome.process_completed = !process.value().timed_out;
    if (process.value().timed_out) {
        return fail_after_mutation(execution_failure(
            ProvisioningExecutionError::timed_out,
            L"provision.run",
            L"The provisioning process timed out"));
    }
    if (process.value().stdout_truncated
        || process.value().stderr_truncated) {
        return fail_after_mutation(execution_failure(
            ProvisioningExecutionError::output_truncated,
            L"provision.run",
            L"The provisioning process output exceeded its bound"));
    }
    if (process.value().exit_code != 0) {
        return fail_after_mutation(execution_failure(
            ProvisioningExecutionError::process_failed,
            L"provision.run",
            L"The provisioning process returned a failure exit code"));
    }

    auto provisional = parse_provisioning_result(
        process.value().stdout_text);
    if (!provisional.ok()) {
        return fail_after_mutation(provisional.error());
    }
    auto planned = derive_gpt_ids(expected.operation_id);
    if (!planned.ok()) {
        return fail_after_mutation(planned.error());
    }
    if (provisional.value().gpt_disk_guid != planned.value().disk_guid
        || provisional.value().partition_guid
            != planned.value().partition_guid) {
        return fail_after_mutation(
            policy_failure<ProvisionalProvisioningIdentity>(
                ProvisioningPolicyError::identity_conflict,
                L"The child returned GPT identifiers outside the retained-handle plan")
                .error());
    }
    VolumeIdentityExpectation postcondition{
        expected.owner,
        expected.physical_disk,
        provisional.value().gpt_disk_guid,
        provisional.value().partition_guid,
        provisional.value().volume_guid,
        std::nullopt};

    // Once the mutating child has started, postcondition verification is
    // mandatory even if the caller cancels while the child is running.
    auto postverified = invoke_provisioning_platform(
        [&] {
            return session->verify_postcondition(
                postcondition, OperationContext{});
        },
        L"provision.verify_postcondition");
    if (!postverified.ok()) {
        return fail_after_mutation(postverified.error());
    }
    outcome.postcondition_verified = true;

    // Task 15 proves the volume lineage and NTFS identity. Re-read the exact
    // retained PhysicalDrive once more after that proof so formatting cannot
    // silently clear the no-drive-letter attribute or alter the planned GPT
    // geometry before a provisioning capability is issued.
    auto final_audit = invoke_provisioning_platform(
        [&] {
            return session->audit_after_mutation(OperationContext{});
        },
        L"provision.final_postformat_audit");
    if (!final_audit.ok()) {
        outcome.audit_error = final_audit.error();
        return outcome;
    }
    outcome.postmutation_audited = true;
    outcome.audit = final_audit.move_value();
    if (outcome.audit->state
            != ProvisioningMutationState::planned_partition_with_volume
        || outcome.audit->layout != ObservedDiskLayout::gpt
        || outcome.audit->partition_count != 1
        || outcome.audit->volume_count != 1
        || outcome.audit->extent_count != 1
        || !outcome.audit->planned_disk_guid_matches
        || !outcome.audit->planned_partition_guid_matches
        || !outcome.audit->planned_partition_layout_matches
        || !outcome.audit->volume_extent_matches) {
        outcome.error = execution_failure(
            ProvisioningExecutionError::postcondition_failed,
            L"provision.final_postformat_audit",
            L"The formatted volume no longer matches the exact GPT, no-drive-letter, and extent plan");
        return outcome;
    }
    outcome.proof = ProvisionedDiskProofFactory::create(
        expected.operation_id, postverified.move_value());
    return outcome;
}

std::unique_ptr<IProvisioningPlatform> make_windows_provisioning_platform() {
    return std::make_unique<WindowsProvisioningPlatform>();
}

#ifdef MYRD_ENABLE_TEST_SEAMS
namespace testing {

Result<ProvisioningProcessResult> run_windows_provisioning_process(
    const ProvisioningCommand& command,
    const OperationContext& context) {
    return run_windows_process(command, context);
}

Result<ProvisioningCommand> build_windows_disk_observation_command(
    const PhysicalDiskIdentity& expected,
    std::wstring_view windows_directory) {
    return build_disk_observation_command(expected, windows_directory);
}

Result<NewDiskObservation> parse_windows_disk_observation(
    const PhysicalDiskIdentity& physical_disk,
    ObservedDiskLayout handle_layout,
    std::uint32_t handle_partition_count,
    std::uint32_t handle_volume_count,
    std::uint32_t handle_extent_count,
    bool handle_read_only,
    bool handle_offline,
    std::string_view output) {
    return parse_disk_observation(
        physical_disk,
        handle_layout,
        handle_partition_count,
        handle_volume_count,
        handle_extent_count,
        handle_read_only,
        handle_offline,
        output);
}

Result<GptLayoutPlan> plan_windows_gpt_layout(
    const product::Uuid& operation_id,
    std::uint64_t disk_length_bytes,
    std::uint64_t starting_usable_offset,
    std::uint64_t usable_length_bytes,
    std::uint32_t bytes_per_sector) {
    auto planned = plan_gpt_layout(
        operation_id,
        disk_length_bytes,
        starting_usable_offset,
        usable_length_bytes,
        bytes_per_sector);
    if (!planned.ok()) {
        return Result<GptLayoutPlan>::failure(planned.error());
    }
    return Result<GptLayoutPlan>::success({
        planned.value().ids.disk_guid,
        planned.value().ids.partition_guid,
        planned.value().partition_starting_offset,
        planned.value().partition_length_bytes});
}

bool provisioning_volume_requires_disk_extents(
    std::uint32_t device_type) noexcept {
    return volume_requires_disk_extents(device_type);
}

ObservedDiskLayout classify_windows_provisioning_layout(
    std::uint32_t partition_style,
    std::uint32_t partition_count,
    std::uint32_t mbr_signature,
    std::uint32_t mbr_checksum) noexcept {
    return classify_drive_layout(
        partition_style,
        partition_count,
        mbr_signature,
        mbr_checksum);
}

} // namespace testing
#endif

} // namespace myrd::host
