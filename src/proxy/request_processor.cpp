#include "request_processor.h"
#include "proxy_error.h"
#include "range_normalizer.h"

#include "src/common/diagnostic_trace.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace myrd::proxy {

RequestProcessor::RequestProcessor(IBlockDevice& device) noexcept
    : device_(device) {}

namespace {

Result<void> validate_io(std::uint64_t offset, std::uint64_t length,
                         std::uint64_t device_size,
                         std::uint32_t sector_size) {
    if (length == 0 || length > kDataAreaSize || offset > device_size ||
        length > device_size - offset) {
        return Result<void>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"validate_io", L"request range is invalid"});
    }
    if ((offset % sector_size) != 0 || (length % sector_size) != 0) {
        return Result<void>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::misaligned),
            L"validate_io", L"request range is not sector aligned"});
    }
    return Result<void>::success();
}

template <class T>
ProcessOutcome write_io_error_response(std::span<std::byte> mapping,
                                       const Error& error,
                                       RequestMetadata meta) {
    T resp{};
    resp.errorno = to_proxy_errorno(error);
    resp.length = 0;
    auto w = write_wire(mapping.subspan(0, sizeof(T)), 0, resp);
    if (!w) {
        return {ProcessorAction::fatal, w.error(), meta};
    }
    return {ProcessorAction::respond, error, meta};
}

template <class T>
ProcessOutcome write_range_error_response(std::span<std::byte> mapping,
                                          const Error& error,
                                          RequestMetadata meta) {
    T resp{};
    resp.errorno = to_proxy_errorno(error);
    auto w = write_wire(mapping.subspan(0, sizeof(T)), 0, resp);
    if (!w) {
        return {ProcessorAction::fatal, w.error(), meta};
    }
    return {ProcessorAction::respond, error, meta};
}

template <class T>
ProcessOutcome write_success_response(std::span<std::byte> mapping,
                                      const T& resp, RequestMetadata meta) {
    auto w = write_wire(mapping.subspan(0, sizeof(T)), 0, resp);
    if (!w) {
        return {ProcessorAction::fatal, w.error(), meta};
    }
    return {ProcessorAction::respond, std::nullopt, meta};
}

ProcessOutcome fatal_outcome(const Error& error) {
    return {ProcessorAction::fatal, error, RequestMetadata{}};
}

} // anonymous namespace

ProcessOutcome RequestProcessor::process(
    const OperationContext& context,
    std::span<std::byte> mapping) noexcept {
    std::scoped_lock lock(mutex_);
    try {
        if (mapping.size() != kMappingSize) {
            counters_.fatal_protocol_errors++;
            return fatal_outcome({ErrorDomain::protocol, 0,
                L"RequestProcessor::process", L"mapping size mismatch"});
        }

        auto req_code = read_wire<std::uint64_t>(
            std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
        if (!req_code) {
            counters_.fatal_protocol_errors++;
            return fatal_outcome(req_code.error());
        }

        RequestMetadata meta{req_code.value(), 0, 0};

        // **Sampled, and deliberately not one line per request.**
        //
        // A line per request on this path was measured wedging the guest: every line is an
        // open/append/flush performed under this processor's lock, and an 8 GiB create went from
        // 192 s to more than ten minutes before the machine stopped answering.  What the large-
        // volume write stall needs is only "did the writes reach this processor, and did the one
        // that stopped return" -- so this logs the first request after a 500 ms idle gap (which
        // is what a stall looks like from here) and every 256th request, and nothing else.
        //
        // Function-local because the sampling state is diagnostic: a second processor instance
        // sharing it would only change which requests are sampled, never what they do.
        static std::uint64_t sampled_requests = 0;
        static std::uint64_t last_trace_tick = 0;
        const std::uint64_t now_tick = GetTickCount64();
        const bool trace_this_request =
            (now_tick - last_trace_tick >= 500 || ((sampled_requests++) & 255) == 0);
        if (trace_this_request) {
            last_trace_tick = now_tick;
            diagnostic_trace(("proxy.request: begin code="
                + std::to_string(req_code.value())).c_str());
        }
        struct RequestDoneTrace {
            bool active;
            ~RequestDoneTrace() {
                if (active) diagnostic_trace("proxy.request: done");
            }
        } request_done_trace{trace_this_request};

        switch (req_code.value()) {

        case kReqInfo: {
            const auto features = device_.features();
            InfoResponse resp{
                device_.size_bytes(),
                device_.sector_size(),
                (features.read_only ? kFlagReadOnly : 0ull) |
                (features.supports_unmap ? kFlagSupportsUnmap : 0ull) |
                (features.supports_zero ? kFlagSupportsZero : 0ull)};
            auto w = write_wire(mapping.subspan(0, sizeof(InfoResponse)), 0, resp);
            if (!w) {
                counters_.fatal_protocol_errors++;
                return fatal_outcome(w.error());
            }
            counters_.info++;
            return {ProcessorAction::respond, std::nullopt, meta};
        }

        case kReqRead: {
            auto req = read_wire<ReadRequest>(
                std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
            if (!req) {
                counters_.fatal_protocol_errors++;
                return fatal_outcome(req.error());
            }
            meta.offset = req.value().offset;
            meta.length = req.value().length;

            auto v = validate_io(req.value().offset, req.value().length,
                                 device_.size_bytes(), device_.sector_size());
            if (!v) {
                counters_.read_errors++;
                return write_io_error_response<ReadResponse>(mapping, v.error(), meta);
            }

            auto live = check_operation_context(context);
            if (!live) {
                if (live.error().code == static_cast<std::uint32_t>(
                        OperationErrorCode::cancelled)) {
                    counters_.cancelled++;
                } else {
                    counters_.timed_out++;
                }
                counters_.read_errors++;
                return write_io_error_response<ReadResponse>(mapping, live.error(), meta);
            }

            auto data_area = mapping.subspan(kHeaderSize, kDataAreaSize);
            auto r = device_.read(context, req.value().offset,
                data_area.subspan(0, static_cast<std::size_t>(req.value().length)));
            if (!r) {
                counters_.read_errors++;
                return write_io_error_response<ReadResponse>(mapping, r.error(), meta);
            }

            counters_.read++;
            ReadResponse ok{0, req.value().length};
            return write_success_response(mapping, ok, meta);
        }

        case kReqWrite: {
            auto req = read_wire<WriteRequest>(
                std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
            if (!req) {
                counters_.fatal_protocol_errors++;
                return fatal_outcome(req.error());
            }
            meta.offset = req.value().offset;
            meta.length = req.value().length;

            auto v = validate_io(req.value().offset, req.value().length,
                                 device_.size_bytes(), device_.sector_size());
            if (!v) {
                counters_.write_errors++;
                return write_io_error_response<WriteResponse>(mapping, v.error(), meta);
            }

            auto live = check_operation_context(context);
            if (!live) {
                if (live.error().code == static_cast<std::uint32_t>(
                        OperationErrorCode::cancelled)) {
                    counters_.cancelled++;
                } else {
                    counters_.timed_out++;
                }
                counters_.write_errors++;
                return write_io_error_response<WriteResponse>(mapping, live.error(), meta);
            }

            const auto length = static_cast<std::size_t>(req.value().length);
            std::vector<std::byte> private_copy(length);
            std::memcpy(private_copy.data(), mapping.data() + kHeaderSize, length);
            auto r = device_.write(context, req.value().offset, private_copy);
            if (!r) {
                counters_.write_errors++;
                return write_io_error_response<WriteResponse>(mapping, r.error(), meta);
            }

            counters_.write++;
            WriteResponse ok{0, req.value().length};
            return write_success_response(mapping, ok, meta);
        }

        case kReqUnmap: {
            auto req = read_wire<UnmapRequest>(
                std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
            if (!req) {
                counters_.fatal_protocol_errors++;
                return fatal_outcome(req.error());
            }
            counters_.unmap++;
            meta.length = req.value().length;

            auto data_area = mapping.subspan(kHeaderSize, kDataAreaSize);
            auto ranges = decode_and_normalize_ranges(
                data_area, req.value().length,
                device_.size_bytes(), device_.sector_size());
            if (!ranges) {
                counters_.unmap_errors++;
                return write_range_error_response<UnmapResponse>(
                    mapping, ranges.error(), meta);
            }

            auto live = check_operation_context(context);
            if (!live) {
                if (live.error().code == static_cast<std::uint32_t>(
                        OperationErrorCode::cancelled)) {
                    counters_.cancelled++;
                } else {
                    counters_.timed_out++;
                }
                counters_.unmap_errors++;
                return write_range_error_response<UnmapResponse>(
                    mapping, live.error(), meta);
            }

            auto r = device_.unmap(context, ranges.value());
            if (!r) {
                counters_.unmap_errors++;
                return write_range_error_response<UnmapResponse>(
                    mapping, r.error(), meta);
            }

            UnmapResponse ok{0};
            return write_success_response(mapping, ok, meta);
        }

        case kReqZero: {
            auto req = read_wire<ZeroRequest>(
                std::span<const std::byte>(mapping.data(), kHeaderSize), 0);
            if (!req) {
                counters_.fatal_protocol_errors++;
                return fatal_outcome(req.error());
            }
            counters_.zero++;
            meta.length = req.value().length;

            auto data_area = mapping.subspan(kHeaderSize, kDataAreaSize);
            auto ranges = decode_and_normalize_ranges(
                data_area, req.value().length,
                device_.size_bytes(), device_.sector_size());
            if (!ranges) {
                counters_.zero_errors++;
                return write_range_error_response<ZeroResponse>(
                    mapping, ranges.error(), meta);
            }

            auto live = check_operation_context(context);
            if (!live) {
                if (live.error().code == static_cast<std::uint32_t>(
                        OperationErrorCode::cancelled)) {
                    counters_.cancelled++;
                } else {
                    counters_.timed_out++;
                }
                counters_.zero_errors++;
                return write_range_error_response<ZeroResponse>(
                    mapping, live.error(), meta);
            }

            auto r = device_.zero(context, ranges.value());
            if (!r) {
                counters_.zero_errors++;
                return write_range_error_response<ZeroResponse>(
                    mapping, r.error(), meta);
            }

            ZeroResponse ok{0};
            return write_success_response(mapping, ok, meta);
        }

        case kReqClose: {
            counters_.close++;
            return {ProcessorAction::close, std::nullopt, meta};
        }

        default:
            counters_.fatal_protocol_errors++;
            return {ProcessorAction::fatal,
                    Error{ErrorDomain::protocol,
                          static_cast<std::uint32_t>(
                              OperationErrorCode::unknown_request),
                          L"RequestProcessor::process",
                          L"unknown request code"},
                    meta};
        }
    } catch (const std::bad_alloc&) {
        return {ProcessorAction::respond,
                Error{ErrorDomain::storage,
                      static_cast<std::uint32_t>(
                          OperationErrorCode::memory_hard_limit),
                      L"process", L"allocation failed"},
                RequestMetadata{}};
    } catch (...) {
        counters_.fatal_protocol_errors++;
        return {ProcessorAction::fatal,
                Error{ErrorDomain::internal,
                      static_cast<std::uint32_t>(
                          OperationErrorCode::internal_invariant),
                      L"process", L"unexpected exception"},
                RequestMetadata{}};
    }
}

ProcessOutcome RequestProcessor::process(
    std::span<std::byte> mapping) noexcept {
    return process(OperationContext{}, mapping);
}

RequestCounters RequestProcessor::counters() const noexcept {
    std::scoped_lock lock(mutex_);
    return counters_;
}

} // namespace myrd::proxy
