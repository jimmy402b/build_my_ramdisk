#pragma once

#include "src/common/error.h"

#include <cstdint>

namespace myrd::proxy {

enum class ErrorDisposition : std::uint8_t { request, session_fatal };

std::uint64_t to_proxy_errorno(const Error& error) noexcept;
ErrorDisposition classify_error(const Error& error) noexcept;

} // namespace myrd::proxy
