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

struct Error {
    ErrorDomain domain;
    std::uint32_t code;
    std::wstring operation;
    std::wstring message;
};

} // namespace myrd
