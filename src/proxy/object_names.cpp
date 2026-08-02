#include "object_names.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace myrd::proxy {

Result<std::wstring> generate_session_id() {
    std::array<std::uint8_t, 16> bytes{};
    NTSTATUS st = BCryptGenRandom(nullptr, bytes.data(),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        return Result<std::wstring>::failure(
            {ErrorDomain::internal, static_cast<std::uint32_t>(st),
             L"BCryptGenRandom", L"failed to generate session id"});
    }
    std::wostringstream oss;
    oss << std::hex << std::setfill(L'0');
    for (auto b : bytes) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return Result<std::wstring>::success(oss.str());
}

Result<void> validate_session_id(std::wstring_view session_id) {
    if (session_id.size() != 32) {
        return Result<void>::failure(
            {ErrorDomain::usage, 0,
             L"validate_session_id",
             L"session_id must be exactly 32 characters"});
    }
    for (auto ch : session_id) {
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) {
            return Result<void>::failure(
                {ErrorDomain::usage, 0,
                 L"validate_session_id",
                 L"session_id contains non-hex or uppercase characters"});
        }
    }
    return Result<void>::success();
}

static Result<ObjectNames> build_names_with_prefix(
    std::wstring_view session_id,
    const std::wstring& prefix,
    bool include_driver_path) {
    auto valid = validate_session_id(session_id);
    if (!valid) {
        return Result<ObjectNames>::failure(valid.error());
    }

    std::wstring base = prefix + std::wstring(session_id);
    ObjectNames names;
    names.mapping  = base;
    names.request  = base + L"_Request";
    names.response = base + L"_Response";
    names.cancel   = base + L"_Cancel";
    names.server   = base + L"_Server";
    if (include_driver_path) {
        names.driver_path = std::wstring(L"\\BaseNamedObjects\\") + base;
    }
    return Result<ObjectNames>::success(names);
}

Result<ObjectNames> ObjectNames::from_session(
    std::wstring_view session_id) {
    // M0 compatibility: Global\myrd-m0- prefix
    auto valid = validate_session_id(session_id);
    if (!valid) {
        return Result<ObjectNames>::failure(valid.error());
    }
    std::wstring base = std::wstring(L"Global\\myrd-m0-") +
                        std::wstring(session_id);
    ObjectNames names;
    names.mapping     = base;
    names.request     = base + L"_Request";
    names.response    = base + L"_Response";
    names.cancel      = base + L"_Cancel";
    names.server      = base + L"_Server";
    names.driver_path = std::wstring(L"\\BaseNamedObjects\\") + base;
    return Result<ObjectNames>::success(names);
}

Result<ProxyObjectNames> KernelVisibleNamespacePolicy::build_names(
    std::wstring_view session_id) const {
    return build_names_with_prefix(session_id, L"Global\\myrd-m1-", true);
}

Result<ProxyObjectNames> LocalNamespacePolicy::build_names(
    std::wstring_view session_id) const {
    return build_names_with_prefix(session_id, L"Local\\myrd-m1-test-", false);
}

} // namespace myrd::proxy
