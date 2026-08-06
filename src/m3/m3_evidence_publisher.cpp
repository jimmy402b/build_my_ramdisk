#include "m3_evidence_publisher.h"

#include "src/common/unique_handle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace myrd::m3 {
namespace {

Error publish_error(std::uint32_t code, const wchar_t* message,
                    std::uint32_t native_code = 0) {
    return {ErrorDomain::evidence, code, L"publish_m3_evidence", message,
            native_code};
}

EvidencePublishOutcome failed(
    Error error, const std::optional<std::filesystem::path>& temporary = {}) {
    EvidencePublishOutcome outcome{};
    outcome.retained_temporary_path = temporary;
    outcome.error = std::move(error);
    return outcome;
}

Result<void> run_hook(const EvidencePublishOptions& options,
                      EvidencePublishPhase phase) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (options.phase_hook) return options.phase_hook(phase);
#else
    (void)options;
    (void)phase;
#endif
    return Result<void>::success();
}

Result<std::pair<UniqueHandle, std::filesystem::path>> create_temporary(
    const std::filesystem::path& final_path) {
    static std::atomic<std::uint64_t> sequence{};
    const auto directory = final_path.parent_path();
    const auto filename = final_path.filename().wstring();
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
        const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = directory /
            (filename + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(value) + L"-" +
             std::to_wstring(attempt));
        UniqueHandle handle(CreateFileW(
            candidate.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
            nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (handle) {
            return Result<std::pair<UniqueHandle, std::filesystem::path>>::
                success({std::move(handle), candidate});
        }
        const auto native = GetLastError();
        if (native != ERROR_FILE_EXISTS && native != ERROR_ALREADY_EXISTS) {
            return Result<std::pair<UniqueHandle, std::filesystem::path>>::
                failure(publish_error(2, L"temporary create failed", native));
        }
    }
    return Result<std::pair<UniqueHandle, std::filesystem::path>>::failure(
        publish_error(3, L"unique temporary name budget exhausted"));
}

Result<void> write_exact(HANDLE handle, const OperationContext& context,
                         std::string_view bytes) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        auto live = check_operation_context(context);
        if (!live.ok()) return live;
        const auto remaining = bytes.size() - offset;
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            remaining, (std::numeric_limits<DWORD>::max)()));
        DWORD written{};
        if (!WriteFile(handle, bytes.data() + offset, request, &written,
                       nullptr)) {
            return Result<void>::failure(publish_error(
                4, L"temporary exact write failed", GetLastError()));
        }
        if (written == 0 || written > request) {
            return Result<void>::failure(publish_error(
                5, L"temporary write made invalid progress"));
        }
        offset += written;
    }
    return Result<void>::success();
}

Result<void> rename_open_file_no_replace(
    HANDLE handle, const std::filesystem::path& final_path) {
    const auto target = final_path.wstring();
    if (target.size() >
        ((std::numeric_limits<DWORD>::max)() - sizeof(FILE_RENAME_INFO)) /
            sizeof(wchar_t)) {
        return Result<void>::failure(publish_error(
            7, L"evidence target path is too long"));
    }
    const auto name_bytes = static_cast<DWORD>(
        target.size() * sizeof(wchar_t));
    // Allocate the complete trailing WCHAR member as required by the Win32
    // variable-length structure contract; FileNameLength still excludes the
    // zero-filled trailing storage.
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = name_bytes;
    std::memcpy(rename->FileName, target.data(), name_bytes);
    if (!SetFileInformationByHandle(
            handle, FileRenameInfo, rename,
            static_cast<DWORD>(storage.size()))) {
        return Result<void>::failure(publish_error(
            7, L"handle-anchored no-overwrite evidence rename failed",
            GetLastError()));
    }
    return Result<void>::success();
}

} // namespace

EvidencePublishOutcome publish_m3_evidence(
    const OperationContext& context,
    const std::filesystem::path& final_path,
    const std::string_view exact_json,
    const EvidencePublishOptions& options) {
    const auto filename = final_path.filename();
    if (final_path.empty() || !final_path.is_absolute() || filename.empty() ||
        filename == L"." || filename == L".." ||
        final_path.parent_path().empty() || exact_json.empty()) {
        return failed(publish_error(1, L"publish arguments are invalid"));
    }
    auto live = check_operation_context(context);
    if (!live.ok()) return failed(live.error());

    auto opened = create_temporary(final_path);
    if (!opened.ok()) return failed(opened.error());
    auto temporary = std::move(opened.value().second);
    auto handle = std::move(opened.value().first);

    auto phase = run_hook(options, EvidencePublishPhase::AfterTemporaryCreate);
    if (!phase.ok()) return failed(phase.error(), temporary);
    auto written = write_exact(handle.get(), context, exact_json);
    if (!written.ok()) return failed(written.error(), temporary);
    phase = run_hook(options, EvidencePublishPhase::AfterExactWrite);
    if (!phase.ok()) return failed(phase.error(), temporary);
    if (!FlushFileBuffers(handle.get())) {
        return failed(publish_error(
            6, L"temporary flush failed", GetLastError()), temporary);
    }
    phase = run_hook(options, EvidencePublishPhase::AfterFlush);
    if (!phase.ok()) return failed(phase.error(), temporary);
    phase = run_hook(options, EvidencePublishPhase::BeforeRename);
    if (!phase.ok()) return failed(phase.error(), temporary);
    auto renamed = rename_open_file_no_replace(handle.get(), final_path);
    if (!renamed.ok()) return failed(renamed.error(), temporary);
    EvidencePublishOutcome outcome{};
    outcome.published = true;
    outcome.published_path = final_path;
    // The same write-through handle now names the final object. Flushing it
    // after the handle-anchored rename avoids reopening by path and asks the
    // filesystem to persist both content and the renamed file's metadata.
    if (!FlushFileBuffers(handle.get())) {
        outcome.error = publish_error(
            8, L"post-rename evidence flush failed", GetLastError());
    }
    return outcome;
}

} // namespace myrd::m3
