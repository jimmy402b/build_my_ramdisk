#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace myrd::aim {

class ISrbTransport {
public:
    virtual ~ISrbTransport() = default;
    virtual Result<std::vector<std::byte>> transact(
        std::uint32_t control_code,
        std::span<const std::byte> request_payload,
        std::size_t response_payload_capacity,
        std::uint32_t timeout_seconds) = 0;
};

} // namespace myrd::aim
