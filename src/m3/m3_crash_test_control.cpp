#include "src/m3/m3_crash_test_control.h"

#if defined(MYRD_ENABLE_TEST_SEAMS)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/common/unique_handle.h"

#include <array>
#include <string>
#include <thread>
#include <utility>

namespace myrd::m3 {
namespace {

constexpr std::array<std::pair<std::string_view, M3CrashTestPhase>, 10>
    kPhaseNames{{
        {"checkpoint.prepare.before",
         M3CrashTestPhase::CheckpointPrepareBefore},
        {"journal.flush.before", M3CrashTestPhase::JournalFlushBefore},
        {"journal.flush.after", M3CrashTestPhase::JournalFlushAfter},
        {"manifest.slot_write.before",
         M3CrashTestPhase::ManifestSlotWriteBefore},
        {"manifest.slot_write.after",
         M3CrashTestPhase::ManifestSlotWriteAfter},
        {"manifest.flush.before", M3CrashTestPhase::ManifestFlushBefore},
        {"manifest.flush.after", M3CrashTestPhase::ManifestFlushAfter},
        {"manifest.readback.after",
         M3CrashTestPhase::ManifestReadbackAfter},
        {"reconcile.before_recover_view",
         M3CrashTestPhase::ReconcileBeforeRecoverView},
        {"reconcile.before_report_success",
         M3CrashTestPhase::ReconcileBeforeReportSuccess},
    }};

Error control_error(
    ErrorDomain domain,
    std::uint32_t code,
    const wchar_t* operation,
    const wchar_t* message) {
    return {domain, code, operation, message, code};
}

Result<void> publish_signal(
    const std::filesystem::path& path,
    const M3CrashTestPhase phase) {
    UniqueHandle file(CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!file) {
        const auto error = GetLastError();
        return Result<void>::failure(control_error(
            ErrorDomain::win32, error, L"CreateFileW(crash signal)",
            L"failed to create the one-shot crash signal"));
    }
    const std::string text =
        "phase=" + std::string(to_string(phase)) + "\r\npid=" +
        std::to_string(GetCurrentProcessId()) + "\r\n";
    DWORD written = 0;
    if (!WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()),
                   &written, nullptr) || written != text.size()) {
        const auto error = GetLastError();
        return Result<void>::failure(control_error(
            ErrorDomain::win32,
            error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error,
            L"WriteFile(crash signal)",
            L"failed to write the complete crash signal"));
    }
    if (!FlushFileBuffers(file.get())) {
        const auto error = GetLastError();
        return Result<void>::failure(control_error(
            ErrorDomain::win32, error, L"FlushFileBuffers(crash signal)",
            L"failed to make the crash signal durable"));
    }
    return Result<void>::success();
}

}  // namespace

Result<M3CrashTestPhase> parse_m3_crash_test_phase(
    const std::string_view name) {
    for (const auto& [candidate, phase] : kPhaseNames) {
        if (name == candidate) {
            return Result<M3CrashTestPhase>::success(phase);
        }
    }
    return Result<M3CrashTestPhase>::failure(control_error(
        ErrorDomain::usage, ERROR_INVALID_PARAMETER,
        L"parse_m3_crash_test_phase", L"unknown crash-test phase"));
}

std::string_view to_string(const M3CrashTestPhase phase) noexcept {
    for (const auto& [name, candidate] : kPhaseNames) {
        if (phase == candidate) return name;
    }
    return "unknown";
}

std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    const persistence::JournalWriterPhase phase) noexcept {
    switch (phase) {
    case persistence::JournalWriterPhase::BeforeFlush:
        return M3CrashTestPhase::JournalFlushBefore;
    case persistence::JournalWriterPhase::AfterFlush:
        return M3CrashTestPhase::JournalFlushAfter;
    default:
        return std::nullopt;
    }
}

std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    const persistence::ManifestPublishTestPhase phase) noexcept {
    switch (phase) {
    case persistence::ManifestPublishTestPhase::BeforeSlotWrite:
        return M3CrashTestPhase::ManifestSlotWriteBefore;
    case persistence::ManifestPublishTestPhase::AfterSlotWrite:
        return M3CrashTestPhase::ManifestSlotWriteAfter;
    case persistence::ManifestPublishTestPhase::BeforeFlush:
        return M3CrashTestPhase::ManifestFlushBefore;
    case persistence::ManifestPublishTestPhase::AfterFlush:
        return M3CrashTestPhase::ManifestFlushAfter;
    case persistence::ManifestPublishTestPhase::AfterReadback:
        return M3CrashTestPhase::ManifestReadbackAfter;
    default:
        return std::nullopt;
    }
}

std::optional<M3CrashTestPhase> map_m3_crash_test_phase(
    const persistence::CheckpointReconcileTestPhase phase) noexcept {
    switch (phase) {
    case persistence::CheckpointReconcileTestPhase::BeforeRecoverView:
        return M3CrashTestPhase::ReconcileBeforeRecoverView;
    case persistence::CheckpointReconcileTestPhase::BeforeReportSuccess:
        return M3CrashTestPhase::ReconcileBeforeReportSuccess;
    default:
        return std::nullopt;
    }
}

M3CrashTestPauseController::M3CrashTestPauseController(
    const M3CrashTestPhase target,
    std::filesystem::path signal_path,
    std::filesystem::path release_path,
    const std::chrono::milliseconds timeout) noexcept
    : target_(target),
      signal_path_(std::move(signal_path)),
      release_path_(std::move(release_path)),
      timeout_(timeout) {}

Result<M3CrashTestPauseController> M3CrashTestPauseController::create(
    const M3CrashTestPhase target,
    std::filesystem::path signal_path,
    std::filesystem::path release_path,
    const std::chrono::milliseconds timeout) {
    std::error_code ec;
    const bool valid = signal_path.is_absolute() && release_path.is_absolute() &&
        signal_path != release_path && timeout.count() > 0 &&
        std::filesystem::is_directory(signal_path.parent_path(), ec) && !ec &&
        !std::filesystem::exists(signal_path, ec) && !ec;
    if (!valid) {
        return Result<M3CrashTestPauseController>::failure(control_error(
            ErrorDomain::usage, ERROR_INVALID_PARAMETER,
            L"M3CrashTestPauseController::create",
            L"crash-test paths or timeout are invalid"));
    }
    return Result<M3CrashTestPauseController>::success(
        M3CrashTestPauseController(
            target, std::move(signal_path), std::move(release_path), timeout));
}

Result<void> M3CrashTestPauseController::observe(
    const M3CrashTestPhase phase) const {
    if (phase != target_) return Result<void>::success();
    auto published = publish_signal(signal_path_, phase);
    if (!published.ok()) return published;

    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto attributes = GetFileAttributesW(release_path_.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                return Result<void>::failure(control_error(
                    ErrorDomain::usage, ERROR_INVALID_DATA,
                    L"M3CrashTestPauseController::observe",
                    L"crash-test release path is not a regular file"));
            }
            return Result<void>::success();
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return Result<void>::failure(control_error(
                ErrorDomain::win32, error,
                L"GetFileAttributesW(crash release)",
                L"failed while waiting for crash-test release"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Result<void>::failure(control_error(
        ErrorDomain::evidence, WAIT_TIMEOUT,
        L"M3CrashTestPauseController::observe",
        L"timed out waiting for crash-test termination or release"));
}

}  // namespace myrd::m3

#endif
