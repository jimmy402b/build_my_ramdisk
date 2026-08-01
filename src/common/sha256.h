#pragma once

#include "result.h"

#include <cstdint>
#include <span>
#include <string>

namespace myrd {

Result<std::string> sha256_hex(std::span<const std::byte> data);

} // namespace myrd
