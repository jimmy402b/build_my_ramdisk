#include "raw_round_trip.h"
#include "src/common/sha256.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <array>

#pragma comment(lib, "bcrypt.lib")

namespace myrd::poc {

RawRoundTrip::RawRoundTrip(IRawDiskIo& io) noexcept : io_(io) {}

Result<RawRoundTripEvidence> RawRoundTrip::run(
    const aim::LocatedDisk& disk) {

    constexpr std::uint64_t kOffset = 8ull * 1024ull * 1024ull;
    constexpr std::size_t kLength = 4096;

    // 1. Verify identity before write
    auto id = io_.verify_identity(disk);
    if (!id) return Result<RawRoundTripEvidence>::failure(id.error());

    // 2. Generate random test data
    std::array<std::byte, kLength> write_buf{};
    NTSTATUS st = BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(write_buf.data()),
        static_cast<ULONG>(write_buf.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        return Result<RawRoundTripEvidence>::failure(
            {ErrorDomain::internal, static_cast<std::uint32_t>(st),
             L"BCryptGenRandom", L"round trip data generation failed"});
    }

    // 3. Compute written hash before touching disk
    auto written_hash = sha256_hex(write_buf);
    if (!written_hash) return Result<RawRoundTripEvidence>::failure(written_hash.error());

    // 4. Seek
    auto sk = io_.seek(kOffset);
    if (!sk) return Result<RawRoundTripEvidence>::failure(sk.error());

    // 5. Write
    auto wr = io_.write_exact(write_buf);
    if (!wr) return Result<RawRoundTripEvidence>::failure(wr.error());

    // 6. Flush
    auto fl = io_.flush();
    if (!fl) return Result<RawRoundTripEvidence>::failure(fl.error());

    // 7. Re-seek
    auto sk2 = io_.seek(kOffset);
    if (!sk2) return Result<RawRoundTripEvidence>::failure(sk2.error());

    // 8. Read back
    std::array<std::byte, kLength> read_buf{};
    auto rd = io_.read_exact(read_buf);
    if (!rd) return Result<RawRoundTripEvidence>::failure(rd.error());

    // 9. Verify hash
    auto read_hash = sha256_hex(read_buf);
    if (!read_hash) return Result<RawRoundTripEvidence>::failure(read_hash.error());

    RawRoundTripEvidence ev{};
    ev.offset = kOffset;
    ev.length = kLength;
    ev.written_sha256 = written_hash.value();
    ev.read_sha256 = read_hash.value();
    ev.flush_succeeded = true;

    if (ev.written_sha256 != ev.read_sha256) {
        return Result<RawRoundTripEvidence>::failure(
            {ErrorDomain::evidence, 0,
             L"RawRoundTrip", L"hash mismatch"});
    }

    return Result<RawRoundTripEvidence>::success(ev);
}

} // namespace myrd::poc
