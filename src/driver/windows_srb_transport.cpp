#include "windows_srb_transport.h"
#include "srb_codec.h"

#include <winioctl.h>
#include <ntddscsi.h>

namespace myrd::aim {

WindowsSrbTransport::WindowsSrbTransport(HANDLE adapter) noexcept
    : adapter_(adapter) {}

Result<std::vector<std::byte>> WindowsSrbTransport::transact(
    std::uint32_t control_code,
    std::span<const std::byte> request_payload,
    std::size_t response_payload_capacity,
    std::uint32_t timeout_seconds) {

    auto frame = build_srb_frame(control_code, request_payload,
                                  response_payload_capacity, timeout_seconds);
    if (!frame) return Result<std::vector<std::byte>>::failure(frame.error());

    DWORD bytes_returned = 0;
    DWORD input_size = static_cast<DWORD>(frame.value().size());
    DWORD output_size = static_cast<DWORD>(frame.value().size());

    BOOL ok = DeviceIoControl(
        adapter_, IOCTL_SCSI_MINIPORT,
        frame.value().data(), input_size,
        frame.value().data(), output_size,
        &bytes_returned, nullptr);

    DWORD win_err = GetLastError();

    if (!ok) {
        return Result<std::vector<std::byte>>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(win_err),
             L"DeviceIoControl(IOCTL_SCSI_MINIPORT)",
             L"IOCTL failed"});
    }

    if (bytes_returned < sizeof(SrbIoControl)) {
        return Result<std::vector<std::byte>>::failure(
            {ErrorDomain::protocol, 0,
             L"DeviceIoControl",
             L"bytes returned too small for SRB header"});
    }

    auto parsed = parse_srb_response(
        std::span<const std::byte>(frame.value().data(), frame.value().size()),
        control_code, bytes_returned);
    if (!parsed) return Result<std::vector<std::byte>>::failure(parsed.error());

    // Return copy of payload
    std::vector<std::byte> result(
        parsed.value().payload.begin(), parsed.value().payload.end());
    return Result<std::vector<std::byte>>::success(std::move(result));
}

} // namespace myrd::aim
