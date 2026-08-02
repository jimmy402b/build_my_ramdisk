#pragma once

#include <cstdint>
#include <string>

namespace myrd {

enum class ErrorDomain : std::uint8_t {
    usage,
    preflight,
    protocol,
    win32,
    ntstatus,
    storage,
    cleanup,
    evidence,
    internal
};

enum class OperationErrorCode : std::uint32_t {
    protocol_malformed = 0x1001,
    unknown_request = 0x1002,
    invalid_range = 0x1003,
    misaligned = 0x1004,
    read_only = 0x1005,
    backing_read_failed = 0x1006,
    memory_hard_limit = 0x1007,
    commit_failed = 0x1008,
    decommit_failed = 0x1009,
    cancelled = 0x100A,
    timed_out = 0x100B,
    session_closed = 0x100C,
    version_changed = 0x100D,
    internal_invariant = 0x100E
};

struct Error {
    ErrorDomain domain;
    std::uint32_t code;
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code = 0;
};

} // namespace myrd
