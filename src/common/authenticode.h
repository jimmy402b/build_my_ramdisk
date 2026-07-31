#pragma once

#include "result.h"

#include <string>

namespace myrd {

struct AuthenticodeResult {
    std::uint32_t trust_status;
    std::wstring signer_subject;
};

Result<AuthenticodeResult> verify_authenticode(const std::wstring& file_path);

} // namespace myrd
