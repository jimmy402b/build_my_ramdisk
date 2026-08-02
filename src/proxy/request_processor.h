#pragma once

#include "src/common/operation_context.h"
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
    std::uint64_t unmap = 0;
    std::uint64_t zero = 0;
    std::uint64_t close = 0;
    std::uint64_t info_errors = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t unmap_errors = 0;
    std::uint64_t zero_errors = 0;
    std::uint64_t fatal_protocol_errors = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t timed_out = 0;
};

struct RequestMetadata {
    std::uint64_t request_code = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct ProcessOutcome {
    ProcessorAction action;
    std::optional<Error> error;
    RequestMetadata request;
};

class RequestProcessor final {
public:
    explicit RequestProcessor(IBlockDevice& device) noexcept;
    ProcessOutcome process(const OperationContext& context,
                           std::span<std::byte> mapping) noexcept;
    ProcessOutcome process(std::span<std::byte> mapping) noexcept;
    RequestCounters counters() const noexcept;

private:
    IBlockDevice& device_;
    RequestCounters counters_{};
};

} // namespace myrd::proxy
