#pragma once

#include "result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace myrd {

using Sha256Digest = std::array<std::byte, 32>;

Result<Sha256Digest> sha256_digest(std::span<const std::byte> data);
Result<std::string> sha256_hex(std::span<const std::byte> data);

} // namespace myrd
