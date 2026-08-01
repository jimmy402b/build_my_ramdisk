#include "srb_codec.h"

#include <algorithm>
#include <cstring>

namespace myrd::aim {

Result<std::vector<std::byte>> build_srb_frame(
    std::uint32_t control_code,
    std::span<const std::byte> request_payload,
    std::size_t response_payload_capacity,
    std::uint32_t timeout_seconds) {

    if (request_payload.size() > 0xFFFFFFFFu) {
        return Result<std::vector<std::byte>>::failure(
            {ErrorDomain::protocol, 0,
             L"build_srb_frame", L"request payload exceeds ULONG max"});
    }

    std::size_t total = sizeof(SrbIoControl) +
        std::max(request_payload.size(), response_payload_capacity);
    if (total > 0xFFFFFFFFu) {
        return Result<std::vector<std::byte>>::failure(
            {ErrorDomain::protocol, 0,
             L"build_srb_frame", L"total frame size exceeds ULONG max"});
    }

    std::vector<std::byte> frame(total, std::byte{0});

    // Build SRB_IO_CONTROL header
    SrbIoControl hdr{};
    hdr.HeaderLength = sizeof(SrbIoControl);
    std::memcpy(hdr.Signature, kSrbSignature, 8);
    hdr.ControlCode = control_code;
    hdr.Length = static_cast<std::uint32_t>(
        std::max(request_payload.size(), response_payload_capacity));
    hdr.Timeout = timeout_seconds;
    hdr.ReturnCode = 0;

    std::memcpy(frame.data(), &hdr, sizeof(hdr));

    // Copy request payload after header
    if (!request_payload.empty()) {
        std::memcpy(frame.data() + sizeof(SrbIoControl),
                    request_payload.data(), request_payload.size());
    }

    return Result<std::vector<std::byte>>::success(std::move(frame));
}

Result<SrbResponse> parse_srb_response(
    std::span<const std::byte> bytes,
    std::uint32_t expected_control_code,
    std::size_t bytes_returned) {

    // 1. Check total header fits
    if (bytes.size() < sizeof(SrbIoControl) ||
        bytes_returned < sizeof(SrbIoControl)) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"response too short for SRB header"});
    }

    // 2. Read header
    SrbIoControl hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));

    // 3. Verify signature
    if (std::memcmp(hdr.Signature, kSrbSignature, 8) != 0) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"wrong SRB signature"});
    }

    // 4. Verify HeaderLength
    if (hdr.HeaderLength != sizeof(SrbIoControl)) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"wrong header length"});
    }

    // 5. Verify ControlCode
    if (hdr.ControlCode != expected_control_code) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"control code mismatch"});
    }

    // 6. Length is the caller-provided payload capacity. AIM can return fewer
    // bytes without rewriting this field, notably for QUERY_DEVICE. The actual
    // response length is DeviceIoControl's bytes_returned value.
    const std::size_t payload_capacity =
        bytes.size() - sizeof(SrbIoControl);
    if (bytes_returned > bytes.size() || hdr.Length > payload_capacity) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"response length exceeds buffer capacity"});
    }
    const std::size_t actual_payload_size =
        bytes_returned - sizeof(SrbIoControl);
    if (actual_payload_size > hdr.Length) {
        return Result<SrbResponse>::failure(
            {ErrorDomain::protocol, 0,
             L"parse_srb_response", L"returned payload exceeds declared capacity"});
    }

    SrbResponse resp{};
    resp.return_code = hdr.ReturnCode;
    resp.payload = bytes.subspan(
        sizeof(SrbIoControl), actual_payload_size);
    return Result<SrbResponse>::success(resp);
}

} // namespace myrd::aim
