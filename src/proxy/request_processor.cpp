#include "request_processor.h"

#include <cstring>

namespace myrd::proxy {

RequestProcessor::RequestProcessor(IBlockDevice& device) noexcept
    : device_(device) {}

ProcessOutcome RequestProcessor::process(
    std::span<std::byte> mapping) noexcept {
    if (mapping.size() != kMappingSize) {
        counters_.fatal_protocol_errors++;
        return {ProcessorAction::fatal,
                Error{ErrorDomain::protocol, 0,
                      L"RequestProcessor::process",
                      L"mapping size mismatch"}};
    }

    auto req_code = read_wire<std::uint64_t>(
        std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
    if (!req_code) {
        counters_.fatal_protocol_errors++;
        return {ProcessorAction::fatal, req_code.error()};
    }

    switch (req_code.value()) {
    case kReqInfo: {
        InfoResponse resp{};
        resp.file_size = device_.size_bytes();
        resp.req_alignment = device_.sector_size();
        resp.flags = 0;
        auto w = write_wire(mapping.subspan(0, sizeof(InfoResponse)), 0, resp);
        if (!w) {
            counters_.fatal_protocol_errors++;
            return {ProcessorAction::fatal, w.error()};
        }
        counters_.info++;
        return {ProcessorAction::respond, std::nullopt};
    }
    default:
        counters_.fatal_protocol_errors++;
        return {ProcessorAction::fatal,
                Error{ErrorDomain::protocol, 0,
                      L"RequestProcessor::process",
                      L"unknown request code"}};
    }
}

RequestCounters RequestProcessor::counters() const noexcept {
    return counters_;
}

} // namespace myrd::proxy
