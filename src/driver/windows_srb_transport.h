#pragma once

#include "srb_transport.h"

#include <windows.h>

namespace myrd::aim {

class WindowsSrbTransport final : public ISrbTransport {
public:
    explicit WindowsSrbTransport(HANDLE adapter) noexcept;
    Result<std::vector<std::byte>> transact(
        std::uint32_t control_code,
        std::span<const std::byte> request_payload,
        std::size_t response_payload_capacity,
        std::uint32_t timeout_seconds) override;

private:
    HANDLE adapter_;
};

} // namespace myrd::aim
