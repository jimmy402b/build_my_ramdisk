#pragma once

#include "src/proxy/imdproxy_protocol.h"
#include "src/storage/block_device.h"

#include <cstdint>
#include <optional>
#include <span>

namespace myrd::proxy {

enum class ProcessorAction : std::uint8_t { respond, close, fatal };

struct RequestCounters {
    std::uint64_t info = 0;
    std::uint64_t read = 0;
    std::uint64_t write = 0;
    std::uint64_t close = 0;
    std::uint64_t fatal_protocol_errors = 0;
};

struct ProcessOutcome {
    ProcessorAction action;
    std::optional<Error> error;
};

class RequestProcessor final {
public:
    explicit RequestProcessor(IBlockDevice& device) noexcept;
    ProcessOutcome process(std::span<std::byte> mapping) noexcept;
    RequestCounters counters() const noexcept;

private:
    IBlockDevice& device_;
    RequestCounters counters_{};
};

} // namespace myrd::proxy
