#include "proxy_error.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd::proxy {

std::uint64_t to_proxy_errorno(const Error& error) noexcept {
    const auto code = static_cast<OperationErrorCode>(error.code);
    switch (code) {
    case OperationErrorCode::invalid_range:
    case OperationErrorCode::misaligned:
        return ERROR_INVALID_PARAMETER;
    case OperationErrorCode::read_only:
        return ERROR_WRITE_PROTECT;
    case OperationErrorCode::backing_read_failed:
        return ERROR_READ_FAULT;
    case OperationErrorCode::memory_hard_limit:
        return ERROR_NOT_ENOUGH_MEMORY;
    case OperationErrorCode::commit_failed:
    case OperationErrorCode::decommit_failed:
        if (error.native_code != 0) {
            return error.native_code;
        }
        return ERROR_NOT_ENOUGH_MEMORY;
    case OperationErrorCode::cancelled:
        return ERROR_OPERATION_ABORTED;
    case OperationErrorCode::timed_out:
        return ERROR_TIMEOUT;
    case OperationErrorCode::session_closed:
        return ERROR_BROKEN_PIPE;
    case OperationErrorCode::internal_invariant:
    default:
        return ERROR_INTERNAL_ERROR;
    }
}

ErrorDisposition classify_error(const Error& error) noexcept {
    const auto opcode = static_cast<OperationErrorCode>(error.code);
    switch (opcode) {
    case OperationErrorCode::protocol_malformed:
    case OperationErrorCode::unknown_request:
    case OperationErrorCode::internal_invariant:
        return ErrorDisposition::session_fatal;
    default:
        // Unknown codes are also fatal
        if (error.code >= 0x1001 && error.code <= 0x100E) {
            return ErrorDisposition::request;
        }
        return ErrorDisposition::session_fatal;
    }
}

} // namespace myrd::proxy
