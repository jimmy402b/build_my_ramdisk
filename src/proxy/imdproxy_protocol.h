#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "src/common/result.h"

// IMDPROXY protocol ABI — all values from audited contract
// Upstream commit: d379caa027d21902e45fce8c9daf02fd116c2dda

namespace myrd::proxy {

// ── Shared memory layout constants ─────────────────────────────

constexpr std::size_t kHeaderSize  = 4096;
constexpr std::size_t kDataAreaSize = 8ull * 1024ull * 1024ull;
constexpr std::size_t kMappingSize  = kHeaderSize + kDataAreaSize;

// ── Request codes ──────────────────────────────────────────────

constexpr std::uint64_t kReqInfo  = 1;
constexpr std::uint64_t kReqRead  = 2;
constexpr std::uint64_t kReqWrite = 3;
constexpr std::uint64_t kReqClose = 5;

// ── INFO response ──────────────────────────────────────────────

struct InfoResponse {
    std::uint64_t file_size;
    std::uint64_t req_alignment;
    std::uint64_t flags;
};

static_assert(sizeof(InfoResponse) == 24, "IMDPROXY_INFO_RESP size mismatch");
static_assert(offsetof(InfoResponse, file_size) == 0);
static_assert(offsetof(InfoResponse, req_alignment) == 8);
static_assert(offsetof(InfoResponse, flags) == 16);

// ── READ request / response ────────────────────────────────────

struct ReadRequest {
    std::uint64_t request_code;
    std::uint64_t offset;
    std::uint64_t length;
};

static_assert(sizeof(ReadRequest) == 24, "IMDPROXY_READ_REQ size mismatch");

struct ReadResponse {
    std::uint64_t errorno;
    std::uint64_t length;
};

static_assert(sizeof(ReadResponse) == 16, "IMDPROXY_READ_RESP size mismatch");

// ── WRITE request / response ───────────────────────────────────

struct WriteRequest {
    std::uint64_t request_code;
    std::uint64_t offset;
    std::uint64_t length;
};

static_assert(sizeof(WriteRequest) == 24, "IMDPROXY_WRITE_REQ size mismatch");

struct WriteResponse {
    std::uint64_t errorno;
    std::uint64_t length;
};

static_assert(sizeof(WriteResponse) == 16, "IMDPROXY_WRITE_RESP size mismatch");

// ── CLOSE request ──────────────────────────────────────────────

struct CloseRequest {
    std::uint64_t request_code;
};

static_assert(sizeof(CloseRequest) == 8, "IMDPROXY_CLOSE_REQ size mismatch");

// ── Safe wire helpers ──────────────────────────────────────────

template <class T>
Result<T> read_wire(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return Result<T>::failure({ErrorDomain::protocol, 0,
            L"read_wire", L"wire buffer is truncated"});
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return Result<T>::success(value);
}

template <class T>
Result<void> write_wire(std::span<std::byte> bytes, std::size_t offset,
                        const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"write_wire", L"wire buffer is too small"});
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
    return Result<void>::success();
}

} // namespace myrd::proxy
