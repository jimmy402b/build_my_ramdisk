#include "windows_srb_transport.h"
#include "srb_codec.h"
#include "src/common/unique_handle.h"

#include <winioctl.h>
#include <ntddscsi.h>

namespace myrd::aim {

WindowsSrbTransport::WindowsSrbTransport(HANDLE adapter) noexcept
    : adapter_(adapter) {}

Result<SrbTransactionResult> WindowsSrbTransport::transact(
    std::uint32_t control_code,
    std::span<const std::byte> request_payload,
    std::size_t response_payload_capacity,
    std::uint32_t timeout_seconds) {

    auto frame = build_srb_frame(control_code, request_payload,
                                  response_payload_capacity, timeout_seconds);
    if (!frame) return Result<SrbTransactionResult>::failure(frame.error());

    DWORD bytes_returned = 0;
    DWORD input_size = static_cast<DWORD>(frame.value().size());
    DWORD output_size = static_cast<DWORD>(frame.value().size());

    // **The call is issued asynchronously and then waited on with a bound, and that is a
    // defect fix rather than a style choice.**  `timeout_seconds` above only travels *inside*
    // the SRB frame for the driver to read; it says nothing about the `DeviceIoControl` call
    // itself.  Issued synchronously with a null `OVERLAPPED` -- which is what this did -- an
    // unresponsive driver blocks here **forever**, passing through every timeout above it: the
    // 30 second provider budget and, in the service, the whole reclaim command budget.  A
    // worker that hung this way could not even report which step it was in.
    //
    // So the deadline is enforced here, at the only layer that can enforce it, using the same
    // bound the frame already carries.
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        return Result<SrbTransactionResult>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"CreateEventW", L"could not create the I/O completion event"});
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL ok = DeviceIoControl(
        adapter_, IOCTL_SCSI_MINIPORT,
        frame.value().data(), input_size,
        frame.value().data(), output_size,
        &bytes_returned, &overlapped);

    if (ok == FALSE) {
        const DWORD start_error = GetLastError();
        if (start_error != ERROR_IO_PENDING) {
            return Result<SrbTransactionResult>::failure(
                {ErrorDomain::win32, static_cast<std::uint32_t>(start_error),
                 L"DeviceIoControl(IOCTL_SCSI_MINIPORT)",
                 L"IOCTL failed"});
        }
        // Bounded by the frame's own timeout, so a driver that never completes is reported
        // rather than waited for.  `timeout_seconds == 0` would mean "wait forever", which is
        // the behaviour this exists to remove, so it is refused rather than honoured.
        if (timeout_seconds == 0) {
            static_cast<void>(CancelIoEx(adapter_, &overlapped));
            static_cast<void>(GetOverlappedResult(adapter_, &overlapped,
                                                  &bytes_returned, TRUE));
            return Result<SrbTransactionResult>::failure(
                {ErrorDomain::protocol, 0, L"DeviceIoControl",
                 L"a driver transaction must carry a non-zero timeout"});
        }
        const DWORD wait =
            WaitForSingleObject(event.get(),
                                static_cast<DWORD>(timeout_seconds) * 1000U);
        if (wait != WAIT_OBJECT_0) {
            // **Cancelled, and the completion is still waited for.**  Returning while the
            // request is outstanding would let the driver write into `frame` after it goes out
            // of scope, and would leave the miniport with a request nobody owns.
            static_cast<void>(CancelIoEx(adapter_, &overlapped));
            static_cast<void>(GetOverlappedResult(adapter_, &overlapped,
                                                  &bytes_returned, TRUE));
            if (wait == WAIT_TIMEOUT) {
                return Result<SrbTransactionResult>::failure(
                    {ErrorDomain::protocol, 0, L"DeviceIoControl",
                     L"the AIM driver did not complete the transaction within its timeout"});
            }
            return Result<SrbTransactionResult>::failure(
                {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
                 L"WaitForSingleObject",
                 L"waiting for the AIM driver transaction failed"});
        }
        // The wait is what carried the result, so ask for it explicitly rather than trusting
        // `bytes_returned`, which the synchronous form would have filled in.
        if (GetOverlappedResult(adapter_, &overlapped, &bytes_returned, FALSE) == FALSE) {
            return Result<SrbTransactionResult>::failure(
                {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
                 L"GetOverlappedResult",
                 L"the AIM driver transaction completed with an error"});
        }
    }

    if (bytes_returned < sizeof(SrbIoControl)) {
        return Result<SrbTransactionResult>::failure(
            {ErrorDomain::protocol, 0,
             L"DeviceIoControl",
             L"bytes returned too small for SRB header"});
    }

    auto parsed = parse_srb_response(
        std::span<const std::byte>(frame.value().data(), frame.value().size()),
        control_code, bytes_returned);
    if (!parsed) return Result<SrbTransactionResult>::failure(parsed.error());

    SrbTransactionResult result{};
    result.return_code = parsed.value().return_code;
    result.payload.assign(parsed.value().payload.begin(),
                          parsed.value().payload.end());
    return Result<SrbTransactionResult>::success(std::move(result));
}

} // namespace myrd::aim
