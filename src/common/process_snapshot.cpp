#include "process_snapshot.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

namespace myrd {

Result<std::vector<ModuleInfo>> snapshot_current_modules() {
    std::vector<ModuleInfo> result;
    HANDLE h = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (h == INVALID_HANDLE_VALUE) {
        return Result<std::vector<ModuleInfo>>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateToolhelp32Snapshot(modules)", L"failed"});
    }

    MODULEENTRY32W me{sizeof(me)};
    if (Module32FirstW(h, &me)) {
        do {
            std::wstring path(me.szExePath);
            std::wstring base(me.szModule);
            result.push_back({std::move(path), std::move(base)});
        } while (Module32NextW(h, &me));
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        CloseHandle(h);
        return Result<std::vector<ModuleInfo>>::success(result);
    }
    CloseHandle(h);
    return Result<std::vector<ModuleInfo>>::success(result);
}

Result<std::vector<ProcessInfo>> snapshot_processes() {
    std::vector<ProcessInfo> result;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return Result<std::vector<ProcessInfo>>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateToolhelp32Snapshot(processes)", L"failed"});
    }

    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(h, &pe)) {
        do {
            result.push_back({pe.th32ProcessID, std::wstring(pe.szExeFile)});
        } while (Process32NextW(h, &pe));
    }
    CloseHandle(h);
    return Result<std::vector<ProcessInfo>>::success(result);
}

bool module_list_contains_forbidden(
    const std::vector<ModuleInfo>& modules,
    const std::vector<std::wstring>& forbidden_basenames) {
    for (const auto& m : modules) {
        auto lower_base = m.basename;
        std::transform(lower_base.begin(), lower_base.end(),
            lower_base.begin(), ::towlower);
        for (const auto& fb : forbidden_basenames) {
            auto lower_fb = fb;
            std::transform(lower_fb.begin(), lower_fb.end(),
                lower_fb.begin(), ::towlower);
            if (lower_base == lower_fb) return true;
        }
    }
    return false;
}

} // namespace myrd
