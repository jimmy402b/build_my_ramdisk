#pragma once

#include "src/common/result.h"

#include <string>
#include <string_view>

namespace myrd::proxy {

struct ObjectNames {
    std::wstring mapping;
    std::wstring request;
    std::wstring response;
    std::wstring server;
    std::wstring driver_path;
    static Result<ObjectNames> from_session(std::wstring_view session_id);
};

Result<std::wstring> generate_session_id();

} // namespace myrd::proxy
