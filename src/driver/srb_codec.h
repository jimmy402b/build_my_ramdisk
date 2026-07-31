#pragma once

#include "aim_protocol.h"
#include "srb_transport.h"
#include "src/common/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace myrd::aim {

Result<std::vector<std::byte>> build_srb_frame(
    std::uint32_t control_code,
    std::span<const std::byte> request_payload,
    std::size_t response_payload_capacity,
    std::uint32_t timeout_seconds);

struct SrbResponse {
    std::uint32_t return_code;
    std::span<const std::byte> payload;
};

Result<SrbResponse> parse_srb_response(
    std::span<const std::byte> bytes,
    std::uint32_t expected_control_code,
    std::size_t bytes_returned);

} // namespace myrd::aim
