#pragma once

#include "src/common/result.h"

#include <memory>
#include <string>
#include <string_view>

namespace myrd::proxy {

struct ObjectNames {
    std::wstring mapping;
    std::wstring request;
    std::wstring response;
    std::wstring cancel;
    std::wstring server;
    std::wstring driver_path;
    static Result<ObjectNames> from_session(std::wstring_view session_id);
};

using ProxyObjectNames = ObjectNames;

class IProxyNamespacePolicy {
public:
    virtual ~IProxyNamespacePolicy() = default;
    virtual Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const = 0;
};

class KernelVisibleNamespacePolicy final : public IProxyNamespacePolicy {
public:
    Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const override;
};

class LocalNamespacePolicy final : public IProxyNamespacePolicy {
public:
    Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const override;
};

Result<std::wstring> generate_session_id();
Result<void> validate_session_id(std::wstring_view session_id);

} // namespace myrd::proxy
