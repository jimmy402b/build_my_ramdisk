#include "src/common/diagnostic_trace.h"
#include "src/common/unique_handle.h"
#include "src/run/windows_volume.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace myrd {
namespace {
Error failure(VolumeError code, const wchar_t* operation, const wchar_t* message,
              std::uint32_t native_code = 0) {
    return {ErrorDomain::win32, static_cast<std::uint32_t>(code), operation, message, native_code};
}

bool same_identity(const ProductVolume& actual, const ProductVolume& expected) noexcept {
    return actual.owner_token == expected.owner_token &&
           actual.volume_guid == expected.volume_guid &&
           actual.extents == expected.extents &&
           actual.drive_letter == expected.drive_letter &&
           actual.mounted == expected.mounted &&
           actual.operation_owned == expected.operation_owned;
}

bool valid_letter(wchar_t letter) noexcept {
    return (letter >= L'A' && letter <= L'Z') ||
           (letter >= L'a' && letter <= L'z');
}

#ifdef _WIN32
constexpr DWORD kProvisionOutputCap = 64u * 1024u;

Result<void> run_provisioning_helper(std::uint32_t disk_number,
                                     wchar_t drive_letter,
                                     std::uint32_t timeout_ms) {
    if (!valid_letter(drive_letter) || timeout_ms == 0 || timeout_ms > 120000)
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"invalid provisioning helper arguments"));

    // Resolve resources from the final executable explicitly.  This code is
    // linked through a static library; passing a null module handle relies on
    // the caller-module convention and can fail under some loader contexts.
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = module ? FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10)) : nullptr;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = loaded ? static_cast<const std::byte*>(LockResource(loaded)) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    if (!data || size == 0)
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"embedded provisioning script unavailable", GetLastError()));

    wchar_t temp[MAX_PATH]{};
    const DWORD temp_len = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (temp_len == 0 || temp_len >= std::size(temp))
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"temporary script path failed", GetLastError()));

    std::wstring script_path;
    HANDLE script = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt != 8 && script == INVALID_HANDLE_VALUE; ++attempt) {
        std::array<std::byte, 16> random{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()),
                            static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return Result<void>::failure(failure(VolumeError::platform_error,
                L"provision_new_volume", L"temporary script random generation failed"));
        static constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring suffix;
        for (const auto byte : random) {
            const auto value = static_cast<unsigned char>(byte);
            suffix += hex[value >> 4]; suffix += hex[value & 15];
        }
        script_path = std::wstring(temp) + L"myrd-" + suffix + L".ps1";
        script = CreateFileW(script_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (script == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
            return Result<void>::failure(failure(VolumeError::platform_error,
                L"provision_new_volume", L"temporary script creation failed", GetLastError()));
    }
    if (script == INVALID_HANDLE_VALUE)
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"temporary script creation failed", GetLastError()));

    DWORD written{};
    const BOOL wrote = WriteFile(script, data, size, &written, nullptr);
    const DWORD write_error = GetLastError();
    const BOOL flushed = wrote && written == size && FlushFileBuffers(script);
    const DWORD flush_error = GetLastError();
    const BOOL closed = CloseHandle(script);
    script = INVALID_HANDLE_VALUE;
    if (!wrote || written != size || !flushed || !closed) {
        const DWORD error = !wrote || written != size ? write_error :
            (!flushed ? flush_error : GetLastError());
        DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"temporary script write failed", error));
    }

    std::wstring command = L"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\""
        L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
        script_path + L"\" -DiskNumber " + std::to_wstring(disk_number) +
        L" -DriveLetter " + drive_letter;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE out_read{}, out_write{}, err_read{}, err_write{};
    auto close_pipes = [&] {
        for (HANDLE handle : {out_read, out_write, err_read, err_write})
            if (handle) CloseHandle(handle);
        out_read = out_write = err_read = err_write = nullptr;
    };
    if (!CreatePipe(&out_read, &out_write, &security, 0) ||
        !CreatePipe(&err_read, &err_write, &security, 0) ||
        !SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error = GetLastError(); close_pipes(); DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"bounded output pipe creation failed", error));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = out_write; startup.StartupInfo.hStdError = err_write;
    SIZE_T attributes_size{};
    SetLastError(ERROR_SUCCESS);
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
    const DWORD attribute_query_error = GetLastError();
    if (attribute_query_error != ERROR_INSUFFICIENT_BUFFER || attributes_size == 0 || attributes_size > 64 * 1024) {
        close_pipes(); DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"restricted child handle list sizing failed", attribute_query_error));
    }
    std::vector<std::byte> attribute_storage(attributes_size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size)) {
        const DWORD error = GetLastError(); close_pipes(); DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"restricted child handle list initialization failed", error));
    }
    HANDLE inherited[] = {out_write, err_write};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        const DWORD error = GetLastError(); DeleteProcThreadAttributeList(startup.lpAttributeList);
        close_pipes(); DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"restricted child handle list update failed", error));
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(out_write); out_write = nullptr; CloseHandle(err_write); err_write = nullptr;
    if (!created) {
        close_pipes(); DeleteFileW(script_path.c_str());
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume", L"CreateProcessW failed", create_error));
    }

    std::atomic<DWORD> out_bytes{}, err_bytes{};
    // **The helper's own words, kept rather than counted.**
    //
    // The drain threads used to discard what they read and keep only a byte count, so a failing
    // provisioning helper reported `native=1 code=21001` -- a bare exit code with the reason thrown away.
    // That is why this failure has been illegible: the PowerShell script says what went wrong and the
    // code dropped it on the floor.  Both streams are now retained (bounded by the same cap that already
    // guards the byte counts) and printed when the helper fails.
    //
    // Guarded by a mutex because two drain threads append, and interleaved halves of two messages would
    // be worse than none.
    std::mutex text_mutex;
    // Narrow, because the bytes come off the pipe as they are; widened only when printed.
    std::string out_text, err_text;
    std::atomic_bool producer_done{}, cancel_drains{}, output_overflow{}, pipe_error{};
    auto drain = [&](HANDLE pipe, std::atomic<DWORD>& total, std::string& text) {
        std::array<std::byte, 4096> buffer{};
        for (;;) {
            if (cancel_drains) return;
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE && !producer_done) pipe_error = true;
                return;
            }
            if (available) {
                const DWORD chunk = (std::min)(available, static_cast<DWORD>(buffer.size()));
                DWORD read{};
                if (!ReadFile(pipe, buffer.data(), chunk, &read, nullptr) || read != chunk) {
                    if (!producer_done) pipe_error = true;
                    return;
                }
                {
                    // Appended as it arrived; a multi-byte character split across two reads would only
                    // ever cost readability, and the helper's messages are ASCII in practice.
                    std::lock_guard<std::mutex> guard(text_mutex);
                    text.append(reinterpret_cast<const char*>(buffer.data()), read);
                }
                const DWORD previous = total.fetch_add(read);
                if (previous > kProvisionOutputCap || read > kProvisionOutputCap - previous) {
                    output_overflow = true; cancel_drains = true; return;
                }
                continue;
            }
            if (producer_done) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    std::thread out_thread(drain, out_read, std::ref(out_bytes), std::ref(out_text));
    std::thread err_thread(drain, err_read, std::ref(err_bytes), std::ref(err_text));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    DWORD wait = WAIT_TIMEOUT; bool exited = false; bool terminate_failed = false;
    for (;;) {
        wait = WaitForSingleObject(process.hProcess, 10);
        if (wait == WAIT_OBJECT_0) { exited = true; break; }
        if (wait == WAIT_FAILED || output_overflow || pipe_error ||
            std::chrono::steady_clock::now() >= deadline) {
            const DWORD reason = output_overflow ? ERROR_BUFFER_OVERFLOW :
                (pipe_error ? ERROR_READ_FAULT : ERROR_TIMEOUT);
            if (!TerminateProcess(process.hProcess, reason) ||
                WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
                terminate_failed = true;
            } else exited = true;
            cancel_drains = true;
            break;
        }
    }
    producer_done = true;
    out_thread.join(); err_thread.join();
    CloseHandle(out_read); CloseHandle(err_read);
    DWORD exit_code{}; const BOOL exit_ok = GetExitCodeProcess(process.hProcess, &exit_code);
    const DWORD exit_error = GetLastError();
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (!exited) {
        return Result<void>::failure(failure(VolumeError::cleanup_failed,
            L"provision_new_volume", L"helper cleanup failed; temporary script retained",
            terminate_failed ? ERROR_TIMEOUT : exit_error));
    }
    const BOOL deleted = DeleteFileW(script_path.c_str());
    const DWORD delete_error = GetLastError();
    if (!deleted || !exit_ok || output_overflow || pipe_error || terminate_failed ||
        wait != WAIT_OBJECT_0 || exit_code != 0) {
        // **Print what the helper said before returning, because that is the whole diagnosis.**
        //
        // The script is the only thing that knows why the volume could not be provisioned -- which disk
        // it could not find, which partition call refused, which letter was taken.  Reporting only the
        // exit code is what made this failure take several rounds to even locate, since it looked like a
        // storage fault rather than a script refusal.
        {
            std::lock_guard<std::mutex> guard(text_mutex);
            // Printed with %hs since the captured bytes are narrow and the stream is wide.
            if (!out_text.empty()) {
                std::fwprintf(stderr, L"myrd-test: provisioning helper stdout: %hs\n",
                              out_text.c_str());
            }
            if (!err_text.empty()) {
                std::fwprintf(stderr, L"myrd-test: provisioning helper stderr: %hs\n",
                              err_text.c_str());
            }
            std::fflush(stderr);
        }
        return Result<void>::failure(failure(VolumeError::platform_error,
            L"provision_new_volume",
            output_overflow ? L"provisioning helper output exceeded 64 KiB" :
                (!deleted ? L"provisioning temporary script cleanup failed" :
                 L"provisioning helper failed"),
            !deleted ? delete_error : (exit_ok ? exit_code : exit_error)));
    }
    return Result<void>::success();
}
#endif

#ifdef _WIN32
Result<void> verify_native_volume_identity(HANDLE handle,
                                           const ProductVolume& expected,
                                           const wchar_t* operation = L"flush") {
    if (!expected.operation_owned || expected.volume_guid.empty() || expected.extents.empty()) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"volume ownership contract is incomplete"));
    }
    std::array<std::byte, 64 * 1024> bytes{};
    std::wstring expected_guid = expected.volume_guid;
    while (!expected_guid.empty() && expected_guid.back() == L'\\') expected_guid.pop_back();
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, bytes.data(), static_cast<DWORD>(bytes.size()),
                         &returned, nullptr)) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"volume extent query failed", GetLastError()));
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
    const auto header = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (returned < header + sizeof(DISK_EXTENT) || extents->NumberOfDiskExtents == 0 ||
        extents->NumberOfDiskExtents > (bytes.size() - header) / sizeof(DISK_EXTENT) ||
        returned < header + extents->NumberOfDiskExtents * sizeof(DISK_EXTENT)) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"malformed volume extents"));
    }
    std::vector<VolumeExtent> actual;
    actual.reserve(extents->NumberOfDiskExtents);
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        const auto& extent = extents->Extents[i];
        actual.push_back({extent.DiskNumber,
            static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
            static_cast<std::uint64_t>(extent.ExtentLength.QuadPart)});
    }
    if (actual != expected.extents) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"volume extents changed"));
    }
    std::array<wchar_t, MAX_PATH> final_path{};
    DWORD path_length = GetFinalPathNameByHandleW(
        handle, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_OPENED);
    const DWORD path_error = GetLastError();
    std::wstring expected_path = expected.volume_guid;
    while (!expected_path.empty() && expected_path.back() == L'\\') expected_path.pop_back();
    if (path_length == 0 &&
        (path_error == ERROR_INVALID_FUNCTION || path_error == ERROR_INVALID_PARAMETER)) {
        path_length = static_cast<DWORD>(expected_path.size());
        std::copy(expected_path.begin(), expected_path.end(), final_path.begin());
    }
    if (path_length == 0 || path_length >= final_path.size()) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"volume handle identity query failed", path_error));
    }
    std::wstring actual_path(final_path.data(), path_length);
    while (!actual_path.empty() && actual_path.back() == L'\\') actual_path.pop_back();
    if (_wcsicmp(actual_path.c_str(), expected_path.c_str()) != 0) {
        return Result<void>::failure(failure(VolumeError::ownership_mismatch,
            operation, L"volume handle is not owned by this operation"));
    }
    return Result<void>::success();
}

std::string guid_text_of(const GUID& guid) {
    std::array<std::byte, 16> bytes{};
    std::memcpy(bytes.data(), &guid, bytes.size());
    return canonical_guid_text(bytes);
}

// Reads the persistent GPT identities of one enumerated volume.
//
// Best effort by design, and deliberately not an error path: a volume whose GPT
// identities cannot be read -- a non-GPT layout, an unprivileged caller, or the
// ERROR_INVALID_FUNCTION that some volume handles answer to a partition query --
// simply leaves the triple empty, and `complete()` then makes every caller treat
// it as absent.  Substituting a zero GUID would be strictly worse than writing
// nothing, because a zero GUID is a value that compares equal to something
// rather than to nothing.
void fill_persistent_identity(ProductVolume& volume, HANDLE volume_handle,
                              const std::wstring& volume_name) {
    // The volume's own GUID is already in the name it was opened by, so it needs
    // no IOCTL: "\\?\Volume{xxxxxxxx-....}\" -> "xxxxxxxx-....".
    const auto open = volume_name.find(L"Volume{");
    const auto close = volume_name.rfind(L'}');
    if (open != std::wstring::npos && close != std::wstring::npos
        && close > open + 7) {
        const auto inner = volume_name.substr(open + 7, close - (open + 7));
        std::string text;
        text.reserve(inner.size());
        for (const wchar_t unit : inner)
            text.push_back(static_cast<char>(towlower(unit)));
        volume.identity.volume_guid = std::move(text);
    }

    // The partition GUID lives on the volume handle.
    std::array<std::byte, 1024> partition{};
    DWORD partition_returned = 0;
    if (DeviceIoControl(volume_handle, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr,
                        0, partition.data(),
                        static_cast<DWORD>(partition.size()),
                        &partition_returned, nullptr)) {
        const auto* info = reinterpret_cast<const PARTITION_INFORMATION_EX*>(
            partition.data());
        if (partition_returned >= sizeof(PARTITION_INFORMATION_EX)
            && info->PartitionStyle == PARTITION_STYLE_GPT) {
            volume.identity.partition_guid = guid_text_of(info->Gpt.PartitionId);
        }
    }

    // The GPT disk GUID lives on the physical drive backing the volume.  The
    // disk number comes from the extent this enumeration already validated, so
    // this cannot be made to name a different disk than the volume sits on.
    if (volume.extents.empty()) return;
    const std::wstring physical = L"\\\\.\\PhysicalDrive"
        + std::to_wstring(volume.extents.front().disk_number);
    HANDLE disk = CreateFileW(physical.c_str(), 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (disk == INVALID_HANDLE_VALUE) return;
    std::array<std::byte, 64 * 1024> layout{};
    DWORD layout_returned = 0;
    if (DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                        layout.data(), static_cast<DWORD>(layout.size()),
                        &layout_returned, nullptr)) {
        const auto* info = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
            layout.data());
        if (layout_returned >= sizeof(DRIVE_LAYOUT_INFORMATION_EX)
            && info->PartitionStyle == PARTITION_STYLE_GPT) {
            volume.identity.disk_guid = guid_text_of(info->Gpt.DiskId);
        }
    }
    CloseHandle(disk);
}

Result<std::vector<VolumeObservation>> enumerate_native_volumes(
    wchar_t requested_letter) {
    std::vector<VolumeObservation> result;
    wchar_t name[MAX_PATH]{};
    HANDLE finder = FindFirstVolumeW(name, static_cast<DWORD>(std::size(name)));
    if (finder == INVALID_HANDLE_VALUE)
        return Result<std::vector<VolumeObservation>>::failure(
            failure(VolumeError::platform_error, L"enumerate_volumes",
                    L"FindFirstVolumeW failed", GetLastError()));
    auto finish = [&] { FindVolumeClose(finder); };
    for (;;) {
        std::wstring guid(name);
        std::wstring device = guid;
        if (!device.empty() && device.back() == L'\\') device.pop_back();
        HANDLE handle = CreateFileW(device.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            std::array<std::byte, 64 * 1024> bytes{}; DWORD returned{};
            if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                nullptr, 0, bytes.data(), static_cast<DWORD>(bytes.size()),
                                &returned, nullptr)) {
                const auto* ext = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
                const auto header = offsetof(VOLUME_DISK_EXTENTS, Extents);
                if (returned >= header && ext->NumberOfDiskExtents > 0 &&
                    ext->NumberOfDiskExtents <= (bytes.size() - header) / sizeof(DISK_EXTENT) &&
                    returned >= header + ext->NumberOfDiskExtents * sizeof(DISK_EXTENT)) {
                    ProductVolume volume{}; volume.volume_guid = guid;
                    volume.extents.reserve(ext->NumberOfDiskExtents);
                    for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i)
                        volume.extents.push_back({ext->Extents[i].DiskNumber,
                            static_cast<std::uint64_t>(ext->Extents[i].StartingOffset.QuadPart),
                            static_cast<std::uint64_t>(ext->Extents[i].ExtentLength.QuadPart)});
                    std::array<wchar_t, 4096> paths{}; DWORD needed{};
                    bool mounted = false;
                    if (GetVolumePathNamesForVolumeNameW(guid.c_str(), paths.data(),
                                                         static_cast<DWORD>(paths.size()), &needed)) {
                        for (std::size_t at = 0; at < paths.size() && paths[at]; ) {
                            std::wstring_view path(paths.data() + at);
                            if (path.size() >= 2 && towupper(path[0]) == towupper(requested_letter) && path[1] == L':') mounted = true;
                            at += path.size() + 1;
                        }
                    }
                    volume.drive_letter = requested_letter; volume.mounted = mounted;
                    volume.operation_owned = false;
                    fill_persistent_identity(volume, handle, guid);
                    result.push_back({std::move(volume), false,
                                      false, true, false, false, true, true});
                }
            }
            CloseHandle(handle);
        }
        if (!FindNextVolumeW(finder, name, static_cast<DWORD>(std::size(name)))) break;
    }
    finish();
    return Result<std::vector<VolumeObservation>>::success(std::move(result));
}
#endif
} // namespace

std::string canonical_guid_text(const std::span<const std::byte, 16> guid_bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    // The Win32 GUID layout, spelled out: Data1 is four little-endian bytes,
    // Data2 and Data3 are two each, and Data4 is already in text order.  Naming
    // the order once here is what keeps the byte order from being re-derived --
    // and re-derived wrongly -- at each call site.
    static constexpr std::size_t kOrder[16] = {
        3, 2, 1, 0,   // Data1
        5, 4,         // Data2
        7, 6,         // Data3
        8, 9, 10, 11, 12, 13, 14, 15};
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text.push_back('-');
        const auto byte =
            std::to_integer<std::uint8_t>(guid_bytes[kOrder[index]]);
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0FU]);
    }
    return text;
}

Result<void> flush_with_identity_checks(const FlushIdentityCheck& verify_before,
                                        const FlushOperation& flush,
                                        const FlushIdentityCheck& verify_after) {
    auto before = verify_before();
    if (!before.ok()) return before;
    auto flushed = flush();
    if (!flushed.ok()) return flushed;
    return verify_after();
}

Result<void> Win32VolumeAdapter::provision_new_volume(
    std::uint32_t disk_number, wchar_t drive_letter,
    std::uint32_t timeout_ms) const {
#ifdef _WIN32
    if (!valid_letter(drive_letter)) {
        return Result<void>::failure(failure(
            VolumeError::occupied_drive_letter, L"provision_new_volume",
            L"invalid drive letter"));
    }
    // Reject an occupied mount point before touching the requested disk.  The
    // provisioning script repeats this check to cover races, but doing it here
    // gives callers a stable classification instead of a generic helper error.
    const auto bit = 1u << (towupper(drive_letter) - L'A');
    const auto logical_drives = GetLogicalDrives();
    if (logical_drives == 0 && GetLastError() != ERROR_SUCCESS) {
        return Result<void>::failure(failure(
            VolumeError::platform_error, L"provision_new_volume",
            L"cannot determine drive-letter occupancy", GetLastError()));
    }
    if ((logical_drives & bit) != 0) {
        return Result<void>::failure(failure(
            VolumeError::occupied_drive_letter, L"provision_new_volume",
            L"requested drive letter is occupied"));
    }
    return run_provisioning_helper(disk_number, drive_letter, timeout_ms);
#else
    (void)disk_number; (void)drive_letter; (void)timeout_ms;
    return Result<void>::failure(failure(VolumeError::platform_error,
        L"provision_new_volume", L"Win32 volume adapter unavailable"));
#endif
}

Result<ProductVolume> Win32VolumeAdapter::provision_and_adopt_volume(
    std::uint32_t disk_number, wchar_t drive_letter,
    std::wstring owner_token, std::uint32_t timeout_ms) const {
    auto provisioned = provision_new_volume(disk_number, drive_letter, timeout_ms);
    if (!provisioned.ok()) return Result<ProductVolume>::failure(provisioned.error());
#ifdef _WIN32
    // Provisioning is a transactional boundary: once the helper reports
    // success, any later adoption/identity failure must undo the exact mount
    // created by this invocation.  Otherwise DiskRuntime has no ProductVolume
    // to own and cannot clean the newly formatted volume on its failure path.
    auto rollback = [&](const std::vector<VolumeObservation>& candidates) {
        for (const auto& candidate : candidates) {
            const auto& value = candidate.volume;
            if (candidate.foreign_mount || value.extents.size() != 1 ||
                value.extents.front().disk_number != disk_number ||
                !value.mounted || value.volume_guid.empty()) {
                continue;
            }
            ProductVolume owned = value;
            owned.owner_token = owner_token;
            owned.drive_letter = drive_letter;
            owned.operation_owned = true;
            (void)remove_drive_letter(owned);
            auto locked = lock(owned, timeout_ms);
            if (locked.ok() && locked.value().locked.has_value())
                (void)locked.value().locked->dismount();
        }
    };
    auto volumes = enumerate_native_volumes(drive_letter);
    for (unsigned attempt = 0; !volumes.ok() && attempt != 4; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        volumes = enumerate_native_volumes(drive_letter);
    }
    if (!volumes.ok()) {
        // A transient enumeration failure must not skip rollback.  Retry the
        // observer once more solely to discover the exact disk/letter mount;
        // if it remains unavailable, the original error is preserved.
        auto retry = enumerate_native_volumes(drive_letter);
        if (retry.ok()) rollback(retry.value());
        return Result<ProductVolume>::failure(volumes.error());
    }
    auto selected = select_volume_for_disk(volumes.value(), disk_number, drive_letter,
                                          owner_token, true);
    if (!selected.ok()) {
        rollback(volumes.value());
        return selected;
    }
    // Provisioning must have mounted the requested letter; adopting an
    // unmounted or foreign volume is never considered success.
    if (!selected.value().mounted) {
        rollback(volumes.value());
        return Result<ProductVolume>::failure(failure(VolumeError::ownership_mismatch,
            L"provision_and_adopt_volume", L"provisioned volume is not mounted at requested letter"));
    }
    return selected;
#else
    (void)disk_number; (void)drive_letter; (void)owner_token;
    return Result<ProductVolume>::failure(failure(VolumeError::platform_error,
        L"provision_and_adopt_volume", L"Win32 volume adapter unavailable"));
#endif
}

Result<ProductVolume> Win32VolumeAdapter::find_recovered_volume(
    std::optional<std::uint32_t> disk_number, wchar_t drive_letter,
    std::wstring owner_token,
    std::optional<std::wstring> expected_guid) const {
#ifdef _WIN32
    auto volumes = enumerate_native_volumes(drive_letter);
    if (!volumes.ok()) return Result<ProductVolume>::failure(volumes.error());
    // **Filter by the durable GUID before selecting, because the letter filter cannot.**
    //
    // `enumerate_native_volumes` stamps every volume with the requested letter, so at this point the
    // list contains every volume on the machine and all of them claim this letter.  Narrowing by GUID
    // first is what makes the choice unambiguous without asking for a physical drive number.
    if (expected_guid.has_value() && !expected_guid->empty()) {
        auto same_guid = [](std::wstring left, const std::wstring& right) {
            while (!left.empty() && left.back() == L'\\') left.pop_back();
            std::wstring expected = right;
            while (!expected.empty() && expected.back() == L'\\') expected.pop_back();
            return _wcsicmp(left.c_str(), expected.c_str()) == 0;
        };
        std::vector<VolumeObservation> matched;
        for (auto& observation : volumes.value()) {
            if (same_guid(observation.volume.volume_guid, *expected_guid))
                matched.push_back(std::move(observation));
        }
        if (matched.empty()) {
            return Result<ProductVolume>::failure(
                failure(VolumeError::not_found, L"find_recovered_volume",
                        L"no volume carries the recorded identity",
                        ERROR_NOT_FOUND));
        }
        // No disk filter either: the GUID has already decided.
        return select_volume_for_disk(matched, std::nullopt, drive_letter,
                                      std::move(owner_token), false);
    }
    // Recovery may observe the volume unmounted after a crash or power loss.
    // The caller owns the subsequent drive-letter assignment; requiring a
    // pre-existing mount here would make valid recovery impossible.
    return select_volume_for_disk(volumes.value(), disk_number, drive_letter,
                                  std::move(owner_token), false);
#else
    (void)disk_number; (void)drive_letter; (void)owner_token; (void)expected_guid;
    return Result<ProductVolume>::failure(failure(VolumeError::platform_error,
        L"find_recovered_volume", L"Win32 volume adapter unavailable"));
#endif
}

LockedVolume::LockedVolume(LockedVolume&& other) noexcept
#ifdef _WIN32
    : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      identity_(std::move(other.identity_)) {}
#else
    : handle_(std::exchange(other.handle_, nullptr)),
      identity_(std::move(other.identity_)) {}
#endif

LockedVolume& LockedVolume::operator=(LockedVolume&& other) noexcept {
    if (this == &other) return *this;
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    handle_ = std::exchange(other.handle_, nullptr);
#endif
    identity_ = std::move(other.identity_);
    return *this;
}

LockedVolume::~LockedVolume() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
}

bool LockedVolume::valid() const noexcept {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return handle_ != nullptr;
#endif
}

Result<void> LockedVolume::flush() const {
#ifdef _WIN32
    if (!valid()) return Result<void>::failure(failure(VolumeError::platform_error, L"flush", L"invalid locked volume handle"));
    auto verify = [&]() { return verify_native_volume_identity(handle_, identity_); };
    return flush_with_identity_checks(verify,
        [&]() -> Result<void> {
            if (!FlushFileBuffers(handle_))
                return Result<void>::failure(failure(VolumeError::flush_failed,
                    L"flush", L"FlushFileBuffers failed", GetLastError()));
            return Result<void>::success();
        }, verify);
#else
    return Result<void>::failure(failure(VolumeError::platform_error, L"flush", L"Win32 volume adapter unavailable"));
#endif
}

// How long a dismantle is given before it is abandoned.
//
// Ten seconds rather than a larger number on purpose: this is a *refusal budget*, not a
// performance target.  A dismount that takes longer than this is one this machine is not going to
// complete, and the engine's response to an uncompleted dismantle is to keep the volume and
// report it -- which is the outcome we want, and one an unbounded wait can never reach.
inline constexpr DWORD kDismountTimeoutMs = 10'000;
Result<void> LockedVolume::dismount() const {
#ifdef _WIN32
    if (!valid()) return Result<void>::failure(failure(VolumeError::platform_error, L"dismount", L"invalid locked volume handle"));
    auto identity = verify_native_volume_identity(handle_, identity_, L"dismount");
    if (!identity.ok()) return identity;

    // **Bounded, because the synchronous form was measured blocking forever.**
    //
    // `FSCTL_DISMOUNT_VOLUME` issued with a null `OVERLAPPED` does not return when the driver
    // does not answer -- which is exactly the state a killed worker leaves its volume in, and
    // exactly the state a reclaim exists to clean up.  It blocked past every deadline above it
    // and the caller had no way to give up, so a dismantle that could not proceed also could not
    // report that it could not proceed.
    //
    // The handle is opened `FILE_FLAG_OVERLAPPED` (see `Win32VolumeAdapter::lock`), which is what
    // makes cancellation possible at all.  A timeout **cancels and then waits for the completion**:
    // returning while the request is outstanding would leave the volume with an I/O nobody owns.
    myrd::diagnostic_trace("LockedVolume::dismount: FSCTL_DISMOUNT_VOLUME");
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready) {
        return Result<void>::failure(failure(VolumeError::platform_error, L"dismount",
            L"could not create the dismount completion event", GetLastError()));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = ready.get();
    DWORD ignored{};
    if (!DeviceIoControl(handle_, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ignored, &overlapped)) {
        const DWORD start_error = GetLastError();
        if (start_error != ERROR_IO_PENDING) {
            return Result<void>::failure(failure(VolumeError::cleanup_failed, L"dismount",
                L"FSCTL_DISMOUNT_VOLUME failed", start_error));
        }
        const DWORD waited = WaitForSingleObject(ready.get(), kDismountTimeoutMs);
        if (waited != WAIT_OBJECT_0) {
            static_cast<void>(CancelIoEx(handle_, &overlapped));
            static_cast<void>(GetOverlappedResult(handle_, &overlapped, &ignored, TRUE));
            // A refusal, not a crash: the caller keeps the volume and retries, which is what the
            // engine already does with a dismantle it could not complete.
            return Result<void>::failure(failure(VolumeError::cleanup_failed, L"dismount",
                L"FSCTL_DISMOUNT_VOLUME did not complete within its timeout", waited));
        }
        if (GetOverlappedResult(handle_, &overlapped, &ignored, FALSE) == FALSE) {
            return Result<void>::failure(failure(VolumeError::cleanup_failed, L"dismount",
                L"FSCTL_DISMOUNT_VOLUME completed with an error", GetLastError()));
        }
    }
    myrd::diagnostic_trace("LockedVolume::dismount: done");
    return Result<void>::success();
#else
    return Result<void>::failure(failure(VolumeError::platform_error, L"dismount", L"Win32 volume adapter unavailable"));
#endif
}

Result<VolumeObservation> Win32VolumeAdapter::observe(const ProductVolume& expected) const {
#ifdef _WIN32
    if (expected.volume_guid.empty()) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::not_found, L"observe", L"volume GUID is empty"));
    }
    std::wstring device = expected.volume_guid;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    HANDLE handle = CreateFileW(device.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::platform_error, L"observe", L"opening volume failed"));
    }
    std::array<std::byte, 64 * 1024> bytes{};
    std::wstring expected_guid = expected.volume_guid;
    while (!expected_guid.empty() && expected_guid.back() == L'\\') expected_guid.pop_back();
    DWORD returned = 0;
    const BOOL queried = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        nullptr, 0, bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr);
    const DWORD query_error = GetLastError();
    std::array<wchar_t, MAX_PATH> final_path{};
    DWORD path_length = queried ? GetFinalPathNameByHandleW(
        handle, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_OPENED) : 0;
    const DWORD path_error = GetLastError();
    CloseHandle(handle);
    if (!queried || returned < offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT)) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::platform_error, L"observe", L"volume extent query failed"));
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
    const auto header = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (extents->NumberOfDiskExtents == 0 ||
        extents->NumberOfDiskExtents > (bytes.size() - header) / sizeof(DISK_EXTENT) ||
        returned < header + extents->NumberOfDiskExtents * sizeof(DISK_EXTENT)) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::platform_error, L"observe", L"malformed volume extents", query_error));
    }
    // Some Windows storage stacks (including the AIM-backed volume in the
    // qualification VM) do not implement GetFinalPathNameByHandleW for a
    // volume GUID handle and return ERROR_INVALID_FUNCTION.  The handle was
    // opened from the already validated expected GUID, and its extents were
    // queried successfully, so retain that authoritative identity in this
    // specific case.  Any other path failure remains fail-closed.
    if (path_length == 0 && path_error == ERROR_INVALID_FUNCTION) {
        path_length = static_cast<DWORD>(expected_guid.size());
        std::copy(expected_guid.begin(), expected_guid.end(), final_path.begin());
    }
    if (path_length == 0 || path_length >= final_path.size()) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::platform_error, L"observe", L"volume handle identity query failed", path_error));
    }
    std::wstring actual_guid(final_path.data(), path_length);
    while (!actual_guid.empty() && actual_guid.back() == L'\\') actual_guid.pop_back();
    if (_wcsicmp(actual_guid.c_str(), expected_guid.c_str()) != 0) {
        return Result<VolumeObservation>::failure(
            failure(VolumeError::ownership_mismatch, L"observe", L"volume handle identity mismatch"));
    }
    ProductVolume actual{};
    actual.volume_guid = std::move(actual_guid);
    actual.drive_letter = expected.drive_letter;
    actual.owner_token = expected.owner_token;
    actual.extents.clear();
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        const auto& extent = extents->Extents[i];
        actual.extents.push_back({extent.DiskNumber,
            static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
            static_cast<std::uint64_t>(extent.ExtentLength.QuadPart)});
    }
    std::array<wchar_t, 64 * 1024 / sizeof(wchar_t)> paths{};
    DWORD needed = 0;
    bool mounted = false;
    const BOOL paths_ok = GetVolumePathNamesForVolumeNameW(expected.volume_guid.c_str(), paths.data(),
                                         static_cast<DWORD>(paths.size()), &needed);
    if (paths_ok) {
        for (std::size_t at = 0; at < paths.size() && paths[at] != L'\0'; ) {
            const std::wstring_view path(paths.data() + at);
            if (path.size() >= 2 && towupper(path[0]) == towupper(expected.drive_letter) &&
                path[1] == L':') mounted = true;
            at += path.size() + 1;
        }
    } else {
        const DWORD mount_path_error = GetLastError();
        if (mount_path_error != ERROR_FILE_NOT_FOUND && mount_path_error != ERROR_PATH_NOT_FOUND) {
            return Result<VolumeObservation>::failure(
                failure(VolumeError::platform_error, L"observe", L"volume mount query failed", mount_path_error));
        }
    }
    actual.mounted = mounted;
    actual.operation_owned = expected.operation_owned &&
        actual.extents == expected.extents &&
        _wcsicmp(actual.volume_guid.c_str(), expected_guid.c_str()) == 0;
    VolumeObservation result{};
    result.volume = std::move(actual);
    result.drive_letter_occupied = expected.drive_letter >= L'A' && expected.drive_letter <= L'Z' &&
        (GetLogicalDrives() & (1u << (expected.drive_letter - L'A'))) != 0 && !mounted;
    return Result<VolumeObservation>::success(std::move(result));
#else
    (void)expected;
    return Result<VolumeObservation>::failure(
        failure(VolumeError::platform_error, L"observe", L"Win32 volume adapter unavailable"));
#endif
}

Result<void> Win32VolumeAdapter::flush(const ProductVolume& owned) const {
#ifdef _WIN32
    std::wstring device = owned.volume_guid;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    // **Both of this function's blocking calls are named, because a reclaim was measured
    // stopping here.**  A dismantle flushes before it removes the drive letter, so a flush that
    // never returns is a reclaim that never dismantles -- and the reading could say "flush" but
    // not *which* call, since neither has a timeout to fail out of.
    myrd::diagnostic_trace("Win32VolumeAdapter::flush: opening the volume");
    HANDLE handle = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Result<void>::failure(failure(VolumeError::platform_error, L"flush", L"opening volume failed"));
    myrd::diagnostic_trace("Win32VolumeAdapter::flush: volume opened, flushing buffers");
    auto verify = [&]() { return verify_native_volume_identity(handle, owned); };
    const auto result = flush_with_identity_checks(verify,
        [&]() -> Result<void> {
            if (!FlushFileBuffers(handle))
                return Result<void>::failure(failure(VolumeError::flush_failed,
                    L"flush", L"FlushFileBuffers failed", GetLastError()));
            return Result<void>::success();
        }, verify);
    myrd::diagnostic_trace("Win32VolumeAdapter::flush: done");
    CloseHandle(handle);
    return result;
#else
    (void)owned;
    return Result<void>::failure(failure(VolumeError::platform_error, L"flush", L"Win32 volume adapter unavailable"));
#endif
}

Result<VolumeLockAttempt> Win32VolumeAdapter::lock(
    const ProductVolume& owned, std::uint32_t timeout_ms) const {
#ifdef _WIN32
    if (timeout_ms == 0)
        return Result<VolumeLockAttempt>::failure(failure(VolumeError::timeout, L"lock", L"lock timeout expired"));
    if (!owned.operation_owned || owned.volume_guid.empty() || owned.extents.empty())
        return Result<VolumeLockAttempt>::failure(failure(VolumeError::ownership_mismatch,
            L"lock", L"volume ownership contract is incomplete"));
    std::wstring device = owned.volume_guid;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    // A volume whose last handle was closed a moment ago can still refuse an open
    // that requests *no* sharing -- which is what an exclusive acquisition asks
    // for -- because the filesystem has not finished tearing that handle's
    // references down yet.
    //
    // This is not hypothetical and it is not rare.  The stop path flushes the
    // volume (its own handle, closed immediately) and then dismantles it (an
    // exclusive open, requested immediately); a create-then-stop sequence hits
    // `ERROR_SHARING_VIOLATION` on the second open in a measured 25-50% of runs,
    // and the same conflict is visible inside the worker as
    // `freeze_writes ... the volume is busy`.  Left alone it reads as a
    // *deterministic* "volume is busy" -- the caller sees a disk that cannot be
    // stopped rather than a timing race, which is how this stayed hidden.
    //
    // So the acquisition is retried inside the budget the caller already passed.
    // `busy` is already documented above as "an expected observation, not an API
    // failure: callers may keep the runtime alive and retry"; this honours that
    // for the case the caller cannot retry itself -- it is *inside* the operation
    // that is failing.  The retry is bounded and it does not soften anything: if
    // the volume is genuinely held by somebody else, every attempt is refused and
    // the caller still gets `busy` with the native code that decided it.
    const DWORD deadline = GetTickCount() + timeout_ms;
    auto remaining = [&deadline]() -> DWORD {
        const DWORD now = GetTickCount();
        return now >= deadline ? 0 : deadline - now;
    };
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (;;) {
        // **`share = 0`, restored: the exclusive open is how this lock excludes others.**
        //
        // It was briefly changed to a shared open plus `FSCTL_LOCK_VOLUME`, so that an
        // unacquirable volume could be abandoned rather than waited on.  That was reverted: the
        // exclusive open excludes another handle **the moment this call returns**, while deferring
        // exclusivity to the IOCTL opens a window between the two steps in which another handle can
        // arrive.  `integration.m4.multi_instance` does *not* cover that -- it injects its locks as
        // fake functions over an in-memory flag -- so the change was unverified for the one thing
        // it altered, and a candidate explanation for a later case failing to provision is worse
        // than the problem it solved.
        //
        // The residue that motivated it does not need it: a reclaim now skips both the flush and
        // the dismount, so it never has to take this lock.  Whatever the residue still blocks, it
        // is not this.
        //
        // `FILE_FLAG_OVERLAPPED` stays.  It does not affect locking or sharing -- exclusivity is the
        // share mode -- and it is what makes the handle's own dismantle cancellable.
        handle = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION
            && error != ERROR_BUSY)
            break;
        if (remaining() == 0) break;
        // A short, fixed pause.  The window being waited out is the filesystem
        // releasing a handle that has already been closed, which is measured in
        // milliseconds; a backoff would only delay the answer.
        //
        // Traced, because a reclaim was measured blocking here for longer than this function's
        // own budget and the two explanations are opposite: a burst of refusals that ends before
        // the budget, or a loop that outlives it.  One line per iteration separates them, and the
        // trace is off unless asked for.
        myrd::diagnostic_trace("Win32VolumeAdapter::lock: open refused, retrying");
        Sleep(remaining() < 50 ? remaining() : 50);
    }
    myrd::diagnostic_trace(handle == INVALID_HANDLE_VALUE
                               ? "Win32VolumeAdapter::lock: gave up opening"
                               : "Win32VolumeAdapter::lock: handle acquired");
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_BUSY)
            return Result<VolumeLockAttempt>::success({false, true, std::nullopt, error});
        return Result<VolumeLockAttempt>::failure(failure(VolumeError::platform_error, L"lock", L"opening volume failed", error));
    }
    // **Bounded, because the synchronous form was measured blocking forever.**
    //
    // `FSCTL_LOCK_VOLUME` has to wait while another handle is open on the volume, and the handle
    // that keeps it waiting belongs to a worker that was killed -- so it waits for something that
    // will never happen.  The retry budget above cannot help: it bounds the *loop*, and this call
    // is inside one iteration of it.  That is why a reclaim was measured exceeding every deadline
    // while reporting no refusals at all.
    //
    // Cancelling is possible because the handle is opened `FILE_FLAG_OVERLAPPED`.  A timeout
    // reports **`busy`**, which this function's own contract already describes as "an expected
    // observation, not an API failure: callers may keep the runtime alive and retry" -- a strictly
    // better answer than waiting forever, and the one the caller already knows how to handle.
    myrd::diagnostic_trace("Win32VolumeAdapter::lock: FSCTL_LOCK_VOLUME");
    UniqueHandle locked(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!locked) {
        CloseHandle(handle);
        return Result<VolumeLockAttempt>::failure(failure(VolumeError::platform_error, L"lock",
            L"could not create the volume lock completion event", GetLastError()));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = locked.get();
    DWORD ignored = 0;
    BOOL ok = DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ignored, &overlapped);
    DWORD error = GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD waited = WaitForSingleObject(locked.get(),
                                                 timeout_ms == 0 ? 1 : timeout_ms);
        if (waited != WAIT_OBJECT_0) {
            static_cast<void>(CancelIoEx(handle, &overlapped));
            static_cast<void>(GetOverlappedResult(handle, &overlapped, &ignored, TRUE));
            CloseHandle(handle);
            myrd::diagnostic_trace("Win32VolumeAdapter::lock: FSCTL_LOCK_VOLUME timed out");
            return Result<VolumeLockAttempt>::success(
                {false, true, std::nullopt, static_cast<DWORD>(WAIT_TIMEOUT)});
        }
        if (GetOverlappedResult(handle, &overlapped, &ignored, FALSE) == FALSE) {
            ok = FALSE;
            error = GetLastError();
        } else {
            ok = TRUE;
        }
    }
    myrd::diagnostic_trace(ok ? "Win32VolumeAdapter::lock: volume locked"
                              : "Win32VolumeAdapter::lock: FSCTL_LOCK_VOLUME refused");
    if (ok) {
        auto identity = verify_native_volume_identity(handle, owned, L"lock");
        if (!identity.ok()) {
            CloseHandle(handle);
            return Result<VolumeLockAttempt>::failure(identity.error());
        }
        return Result<VolumeLockAttempt>::success({true, false,
            std::optional<LockedVolume>(LockedVolume(handle, owned))});
    }
    CloseHandle(handle);
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_BUSY)
        return Result<VolumeLockAttempt>::success({false, true, std::nullopt, error});
    return Result<VolumeLockAttempt>::failure(failure(VolumeError::platform_error, L"lock", L"FSCTL_LOCK_VOLUME failed", error));
#else
    (void)owned; (void)timeout_ms;
    return Result<VolumeLockAttempt>::failure(failure(VolumeError::platform_error, L"lock", L"Win32 volume adapter unavailable"));
#endif
}

Result<void> Win32VolumeAdapter::assign_drive_letter(const ProductVolume& owned, wchar_t drive_letter) const {
#ifdef _WIN32
    if (!valid_letter(drive_letter)) return Result<void>::failure(failure(VolumeError::occupied_drive_letter, L"assign_drive_letter", L"invalid drive letter"));
    std::wstring mount{drive_letter, L':', L'\\', L'\0'};
    std::array<wchar_t, MAX_PATH> current{};
    if (GetVolumeNameForVolumeMountPointW(mount.c_str(), current.data(), static_cast<DWORD>(current.size()))) {
        std::wstring expected_guid = owned.volume_guid;
        while (!expected_guid.empty() && expected_guid.back() == L'\\') expected_guid.pop_back();
        std::wstring actual_guid(current.data());
        while (!actual_guid.empty() && actual_guid.back() == L'\\') actual_guid.pop_back();
        return _wcsicmp(actual_guid.c_str(), expected_guid.c_str()) == 0 ? Result<void>::success() :
            Result<void>::failure(failure(VolumeError::occupied_drive_letter, L"assign_drive_letter", L"requested drive letter is occupied"));
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        return Result<void>::failure(failure(VolumeError::platform_error, L"assign_drive_letter", L"mount query failed", error));
    return SetVolumeMountPointW(mount.c_str(), owned.volume_guid.c_str()) ? Result<void>::success() :
        Result<void>::failure(failure(VolumeError::platform_error, L"assign_drive_letter", L"SetVolumeMountPointW failed", GetLastError()));
#else
    (void)owned; (void)drive_letter;
    return Result<void>::failure(failure(VolumeError::platform_error, L"assign_drive_letter", L"Win32 volume adapter unavailable"));
#endif
}

Result<void> Win32VolumeAdapter::remove_drive_letter(const ProductVolume& owned) const {
#ifdef _WIN32
    if (!valid_letter(owned.drive_letter)) return Result<void>::failure(failure(VolumeError::ownership_mismatch, L"remove_drive_letter", L"invalid drive letter"));
    std::wstring mount{owned.drive_letter, L':', L'\\', L'\0'};
    std::array<wchar_t, MAX_PATH> current{};
    if (!GetVolumeNameForVolumeMountPointW(mount.c_str(), current.data(), static_cast<DWORD>(current.size()))) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return Result<void>::success();
        return Result<void>::failure(failure(VolumeError::platform_error, L"remove_drive_letter", L"mount query failed", error));
    }
    if (_wcsicmp(current.data(), owned.volume_guid.c_str()) != 0)
        return Result<void>::failure(failure(VolumeError::ownership_mismatch, L"remove_drive_letter", L"refusing foreign mount"));
    return DeleteVolumeMountPointW(mount.c_str()) ? Result<void>::success() :
        Result<void>::failure(failure(VolumeError::cleanup_failed, L"remove_drive_letter", L"DeleteVolumeMountPointW failed", GetLastError()));
#else
    (void)owned;
    return Result<void>::failure(failure(VolumeError::platform_error, L"remove_drive_letter", L"Win32 volume adapter unavailable"));
#endif
}

Result<ProductVolume> provision_new_volume(
    const VolumeObservation& observation,
    const ProductVolume& expected) {
    if (observation.foreign_mount) {
        return Result<ProductVolume>::failure(
            failure(VolumeError::foreign_mount, L"provision_new_volume",
                    L"refusing a foreign mount"));
    }
    if (observation.drive_letter_occupied) {
        return Result<ProductVolume>::failure(
            failure(VolumeError::occupied_drive_letter, L"provision_new_volume",
                    L"requested drive letter is occupied"));
    }
    if (!same_identity(observation.volume, expected) ||
        observation.volume.mounted || !observation.volume.operation_owned) {
        return Result<ProductVolume>::failure(
            failure(VolumeError::ownership_mismatch, L"provision_new_volume",
                    L"provisioned volume identity does not match ownership contract"));
    }
    return Result<ProductVolume>::success(observation.volume);
}

Result<ProductVolume> find_recovered_volume(
    std::span<const VolumeObservation> observations,
    const ProductVolume& expected) {
    for (const auto& observation : observations) {
        if (observation.foreign_mount) {
            return Result<ProductVolume>::failure(
                failure(VolumeError::foreign_mount, L"find_recovered_volume",
                        L"foreign mount was observed for the requested volume"));
        }
        if (same_identity(observation.volume, expected)) {
            return Result<ProductVolume>::success(observation.volume);
        }
    }
    return Result<ProductVolume>::failure(
        failure(VolumeError::not_found, L"find_recovered_volume",
                L"owned volume was not found"));
}

Result<ProductVolume> select_volume_for_disk(
    std::span<const VolumeObservation> observations,
    std::optional<std::uint32_t> disk_number, wchar_t drive_letter,
    std::wstring owner_token, bool require_mounted) {
    if (!valid_letter(drive_letter)) {
        return Result<ProductVolume>::failure(
            failure(VolumeError::platform_error, L"select_volume_for_disk",
                    L"invalid drive letter"));
    }
    std::optional<ProductVolume> selected;
    for (const auto& observation : observations) {
        const auto& candidate = observation.volume;
        if (observation.foreign_mount || observation.drive_letter_occupied)
            continue;
        if (candidate.drive_letter != drive_letter ||
            (require_mounted && !candidate.mounted) || candidate.volume_guid.empty() ||
            candidate.extents.size() != 1)
            continue;
        // **The physical-disk filter is skipped when no disk number is given.**
        //
        // Recovery and reclaim pass `nullopt`, because obtaining the number meant opening every
        // candidate disk -- measured blocking for 11 seconds while a device was retiring.  What still
        // identifies the volume is everything above plus `verify_volume`'s GUID comparison against the
        // durable profile, which is the stronger check anyway: Windows reuses a drive number, and does
        // not reuse a volume GUID.
        if (disk_number.has_value() &&
            candidate.extents.front().disk_number != *disk_number)
            continue;
        if (selected.has_value()) {
            return Result<ProductVolume>::failure(
                failure(VolumeError::ownership_mismatch, L"select_volume_for_disk",
                        L"multiple volumes match the requested drive letter"));
        }
        ProductVolume adopted = candidate;
        adopted.owner_token = owner_token;
        adopted.drive_letter = drive_letter;
        adopted.operation_owned = true;
        selected = std::move(adopted);
    }
    if (!selected.has_value()) {
        return Result<ProductVolume>::failure(
            failure(VolumeError::not_found, L"select_volume_for_disk",
                    L"no volume matches the requested physical disk and drive letter"));
    }
    return Result<ProductVolume>::success(std::move(*selected));
}

Result<void> flush_volume(
    const VolumeObservation& observation,
    const ProductVolume& owned) {
    if (!same_identity(observation.volume, owned) ||
        !observation.volume.operation_owned) {
        return Result<void>::failure(
            failure(VolumeError::ownership_mismatch, L"flush_volume",
                    L"refusing to flush a volume not owned by this operation"));
    }
    if (!observation.flush_succeeds) {
        return Result<void>::failure(
            failure(VolumeError::flush_failed, L"flush_volume",
                    L"volume flush failed"));
    }
    return Result<void>::success();
}

Result<VolumeLockAttempt> lock_volume(
    const VolumeObservation& observation,
    std::uint32_t timeout_ms) {
    if (observation.lock_api_error) {
        return Result<VolumeLockAttempt>::failure(
            failure(VolumeError::platform_error, L"lock_volume",
                    L"volume lock API failed"));
    }
    if (observation.lock_timed_out || timeout_ms == 0) {
        if (observation.lock_timed_out) {
            return Result<VolumeLockAttempt>::failure(
                failure(VolumeError::timeout, L"lock_volume",
                        L"volume lock timed out"));
        }
    }
    if (!observation.lock_available) {
        return Result<VolumeLockAttempt>::success({false, true});
    }
    return Result<VolumeLockAttempt>::success({true, false});
}

Result<void> assign_drive_letter(
    const VolumeObservation& observation,
    const ProductVolume& owned,
    wchar_t drive_letter) {
    if (!valid_letter(drive_letter) ||
        !same_identity(observation.volume, owned) ||
        !observation.volume.operation_owned) {
        return Result<void>::failure(
            failure(VolumeError::ownership_mismatch, L"assign_drive_letter",
                    L"volume identity or ownership mismatch"));
    }
    if (observation.drive_letter_occupied) {
        return Result<void>::failure(
            failure(VolumeError::occupied_drive_letter, L"assign_drive_letter",
                    L"requested drive letter is occupied"));
    }
    return Result<void>::success();
}

Result<void> remove_drive_letter(
    const VolumeObservation& observation,
    const ProductVolume& owned) {
    if (!same_identity(observation.volume, owned) ||
        !observation.volume.operation_owned) {
        return Result<void>::failure(
            failure(VolumeError::ownership_mismatch, L"remove_drive_letter",
                    L"refusing to remove a foreign mount"));
    }
    if (!observation.cleanup_succeeds) {
        return Result<void>::failure(
            failure(VolumeError::cleanup_failed, L"remove_drive_letter",
                    L"drive-letter cleanup failed"));
    }
    return Result<void>::success();
}
} // namespace myrd
