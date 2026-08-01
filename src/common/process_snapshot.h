#pragma once

#include "result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace myrd {

struct ModuleInfo {
    std::wstring path;
    std::wstring basename;
};

struct ProcessInfo {
    std::uint32_t pid;
    std::wstring name;
};

Result<std::vector<ModuleInfo>> snapshot_current_modules();
Result<std::vector<ProcessInfo>> snapshot_processes();
bool module_list_contains_forbidden(
    const std::vector<ModuleInfo>& modules,
    const std::vector<std::wstring>& forbidden_basenames);

} // namespace myrd
