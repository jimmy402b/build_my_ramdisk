#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"

#include <cstdint>
#include <string>

namespace myrd::aim {

struct AdapterHandle {
    std::wstring interface_path;
    UniqueHandle  handle;
    std::uint32_t dev_inst;
};

Result<AdapterHandle> discover_aim_adapter();
Result<void> rescan_aim_adapter(std::uint32_t dev_inst);

} // namespace myrd::aim
