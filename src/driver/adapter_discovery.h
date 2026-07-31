#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"

#include <string>

namespace myrd::aim {

struct AdapterHandle {
    std::wstring interface_path;
    UniqueHandle  handle;
};

Result<AdapterHandle> discover_aim_adapter();

} // namespace myrd::aim
