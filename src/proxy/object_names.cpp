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

Result<ObjectNames> ObjectNames::from_session(
    std::wstring_view session_id) {
    // Session ID: exactly 32 lowercase hex characters
    if (session_id.size() != 32) {
        return Result<ObjectNames>::failure(
            {ErrorDomain::usage, 0,
             L"ObjectNames::from_session",
             L"session_id must be exactly 32 characters"});
    }
    for (auto ch : session_id) {
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) {
            return Result<ObjectNames>::failure(
                {ErrorDomain::usage, 0,
                 L"ObjectNames::from_session",
                 L"session_id contains non-hex or uppercase characters"});
        }
    }

    std::wstring base = std::wstring(L"Global\\myrd-m0-") +
                        std::wstring(session_id);
    ObjectNames names;
    names.mapping     = base;
    names.request     = base + L"_Request";
    names.response    = base + L"_Response";
    names.server      = base + L"_Server";
    names.driver_path = std::wstring(L"\\BaseNamedObjects\\Global\\") +
                         std::wstring(session_id);
    return Result<ObjectNames>::success(names);
}

} // namespace myrd::proxy
