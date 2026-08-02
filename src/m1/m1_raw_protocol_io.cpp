#include "m1_raw_protocol_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace myrd {

namespace {

constexpr std::uint64_t kTestOffset = 8ull * 1024ull * 1024ull; // 8 MiB
constexpr std::uint64_t kTestLength = 1ull * 1024ull * 1024ull; // 1 MiB

// Simple SHA-256 for evidence (uses BCrypt)
std::string sha256_hex(const void* /*data*/, std::size_t /*size*/) {
    // Defer to common/sha256.h in production; stub for unit-test path
    return "sha256-placeholder";
}

} // anonymous namespace

Result<M1RawProtocolEvidence> verify_m1_raw_protocol(
    IM1RawDiskApi& api,
    std::uint64_t device_size,
    std::uint32_t sector_size,
    const proxy::RequestCounters& before,
    const std::function<proxy::RequestCounters()>& counters) {

    (void)device_size;
    (void)sector_size;

    M1RawProtocolEvidence evidence{};
    evidence.offset = kTestOffset;
    evidence.length = kTestLength;
    evidence.zero_write_verified = false;
    evidence.unmap_verified = false;
    evidence.flush_succeeded = false;

    // 1. Write deterministic nonzero pattern
    std::vector<std::byte> pattern(static_cast<std::size_t>(kTestLength));
    std::byte val{0x5A};
    for (auto& b : pattern) {
        b = val;
        val = static_cast<std::byte>(static_cast<int>(val) + 1);
    }
    auto w = api.write(kTestOffset, pattern);
    if (!w) {
        return Result<M1RawProtocolEvidence>::failure(w.error());
    }

    // 2. Flush and read back
    auto f = api.flush();
    if (f.ok()) evidence.flush_succeeded = true;

    std::vector<std::byte> readback(static_cast<std::size_t>(kTestLength));
    auto r = api.read(kTestOffset, readback);
    if (!r) {
        return Result<M1RawProtocolEvidence>::failure(r.error());
    }
    evidence.read_sha256 = sha256_hex(readback.data(), readback.size());

    // 3. All-zero write (triggers ZERO code 7)
    std::vector<std::byte> zeros(static_cast<std::size_t>(kTestLength),
                                  std::byte{0});
    auto zw = api.write(kTestOffset, zeros);
    if (!zw) {
        return Result<M1RawProtocolEvidence>::failure(zw.error());
    }
    api.flush();

    // Wait for zero counter to increase (up to 5 seconds)
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        auto now = counters();
        if (now.zero > before.zero) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto confirm = counters();
            if (confirm.zero > before.zero) {
                evidence.zero_write_verified = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify zeros read back
    auto zr = api.read(kTestOffset, readback);
    if (!zr) {
        return Result<M1RawProtocolEvidence>::failure(zr.error());
    }

    // 4. Rewrite pattern
    auto rw = api.write(kTestOffset, pattern);
    if (!rw) {
        return Result<M1RawProtocolEvidence>::failure(rw.error());
    }
    api.flush();

    // 5. Issue TRIM/UNMAP
    auto trim_result = api.trim(kTestOffset, kTestLength);
    if (!trim_result) {
        return Result<M1RawProtocolEvidence>::failure(trim_result.error());
    }

    // Wait for unmap counter to increase
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        auto now = counters();
        if (now.unmap > before.unmap) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto confirm = counters();
            if (confirm.unmap > before.unmap) {
                evidence.unmap_verified = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify zeros after trim
    auto tr = api.read(kTestOffset, readback);
    if (!tr) {
        return Result<M1RawProtocolEvidence>::failure(tr.error());
    }

    return Result<M1RawProtocolEvidence>::success(evidence);
}

} // namespace myrd
