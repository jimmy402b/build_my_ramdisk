#pragma once

#include "src/common/result.h"
#include "src/storage/block_device.h"

#include <cstdint>
#include <span>
#include <vector>

namespace myrd::proxy {

Result<std::vector<DataRange>> decode_and_normalize_ranges(
    std::span<const std::byte> data_area,
    std::uint64_t encoded_length,
    std::uint64_t device_size,
    std::uint32_t sector_size);

} // namespace myrd::proxy
