#include "report_publisher.h"

#include "src/proxy/object_names.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <system_error>

namespace myrd::poc {

Result<void> publish_report_atomic(
    std::string_view json,
    const std::filesystem::path& final_path) {
    std::error_code filesystem_error;
    const auto parent = final_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return Result<void>::failure(
                {ErrorDomain::evidence,
                 static_cast<std::uint32_t>(filesystem_error.value()),
                 L"create_directories(report)",
                 L"failed to create report parent directory"});
        }
    }

    if (std::filesystem::exists(final_path, filesystem_error)) {
        return Result<void>::failure(
            {ErrorDomain::evidence, ERROR_FILE_EXISTS,
             L"publish_report_atomic",
             L"refusing to overwrite an existing report"});
    }
    if (filesystem_error) {
        return Result<void>::failure(
            {ErrorDomain::evidence,
             static_cast<std::uint32_t>(filesystem_error.value()),
             L"exists(report)", L"failed to inspect report path"});
    }

    auto id = proxy::generate_session_id();
    if (!id) return Result<void>::failure(id.error());
    auto temporary_path = final_path;
    temporary_path += L".tmp-" + id.value();

    HANDLE file = CreateFileW(
        temporary_path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(
            {ErrorDomain::win32,
             static_cast<std::uint32_t>(GetLastError()),
             L"CreateFileW(report temp)",
             L"failed to create unique report temporary file"});
    }

    std::size_t offset = 0;
    while (offset < json.size()) {
        const auto remaining = json.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining,
                       static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, json.data() + offset, chunk,
                &written, nullptr) || written != chunk) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return Result<void>::failure(
                {ErrorDomain::win32, error,
                 L"WriteFile(report temp)",
                 L"failed to write complete report; temporary file retained"});
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return Result<void>::failure(
            {ErrorDomain::win32, error,
             L"FlushFileBuffers(report temp)",
             L"failed to flush report; temporary file retained"});
    }
    CloseHandle(file);

    if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        return Result<void>::failure(
            {ErrorDomain::win32,
             static_cast<std::uint32_t>(GetLastError()),
             L"MoveFileExW(report)",
             L"failed to publish report without overwrite; temporary file retained"});
    }
    return Result<void>::success();
}

} // namespace myrd::poc
