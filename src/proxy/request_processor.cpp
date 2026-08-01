#include "request_processor.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace myrd::proxy {

RequestProcessor::RequestProcessor(IBlockDevice& device) noexcept
    : device_(device) {}

namespace {

// Validate the six conditions for READ/WRITE.
Result<void> validate_io(std::uint64_t offset, std::uint64_t length,
                         std::uint64_t device_size, std::uint32_t sector_size) {
    if (length == 0) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"zero-length I/O request"});
    }
    if (length > kDataAreaSize) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"request length exceeds data area"});
    }
    if ((offset % sector_size) != 0) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"offset not sector-aligned"});
    }
    if ((length % sector_size) != 0) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"length not sector-aligned"});
    }
    if (offset > device_size) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"offset beyond device"});
    }
    if (length > device_size - static_cast<std::size_t>(offset)) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"validate_io", L"length beyond device end"});
    }
    return Result<void>::success();
}

} // anonymous namespace

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

    case kReqRead: {
        auto req = read_wire<ReadRequest>(
            std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
        if (!req) {
            counters_.fatal_protocol_errors++;
            return {ProcessorAction::fatal, req.error()};
        }
        auto v = validate_io(req.value().offset, req.value().length,
                             device_.size_bytes(), device_.sector_size());
        if (!v) {
            ReadResponse err_resp{};
            err_resp.errorno = 1;
            err_resp.length = 0;
            write_wire(mapping.subspan(0, sizeof(ReadResponse)), 0, err_resp);
            counters_.read++;
            return {ProcessorAction::respond, std::nullopt};
        }
        auto data_area = mapping.subspan(kHeaderSize, kDataAreaSize);
        auto r = device_.read(req.value().offset,
            data_area.subspan(0, static_cast<std::size_t>(req.value().length)));
        if (!r) {
            ReadResponse err_resp{};
            err_resp.errorno = 1;
            err_resp.length = 0;
            write_wire(mapping.subspan(0, sizeof(ReadResponse)), 0, err_resp);
            counters_.read++;
            return {ProcessorAction::respond, std::nullopt};
        }
        ReadResponse ok_resp{};
        ok_resp.errorno = 0;
        ok_resp.length = req.value().length;
        write_wire(mapping.subspan(0, sizeof(ReadResponse)), 0, ok_resp);
        counters_.read++;
        return {ProcessorAction::respond, std::nullopt};
    }

    case kReqWrite: {
        auto req = read_wire<WriteRequest>(
            std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
        if (!req) {
            counters_.fatal_protocol_errors++;
            return {ProcessorAction::fatal, req.error()};
        }
        auto v = validate_io(req.value().offset, req.value().length,
                             device_.size_bytes(), device_.sector_size());
        if (!v) {
            WriteResponse err_resp{};
            err_resp.errorno = 1;
            err_resp.length = 0;
            write_wire(mapping.subspan(0, sizeof(WriteResponse)), 0, err_resp);
            counters_.write++;
            return {ProcessorAction::respond, std::nullopt};
        }
        // Private copy before backend call
        auto data_area = mapping.subspan(kHeaderSize, kDataAreaSize);
        std::vector<std::byte> private_copy(
            data_area.begin(),
            data_area.begin() + static_cast<std::size_t>(req.value().length));
        auto r = device_.write(req.value().offset, private_copy);
        if (!r) {
            WriteResponse err_resp{};
            err_resp.errorno = 1;
            err_resp.length = 0;
            write_wire(mapping.subspan(0, sizeof(WriteResponse)), 0, err_resp);
            counters_.write++;
            return {ProcessorAction::respond, std::nullopt};
        }
        WriteResponse ok_resp{};
        ok_resp.errorno = 0;
        ok_resp.length = req.value().length;
        write_wire(mapping.subspan(0, sizeof(WriteResponse)), 0, ok_resp);
        counters_.write++;
        return {ProcessorAction::respond, std::nullopt};
    }

    case kReqClose: {
        counters_.close++;
        return {ProcessorAction::close, std::nullopt};
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
