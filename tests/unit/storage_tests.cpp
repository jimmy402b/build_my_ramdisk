#include "test_framework.h"

#include "src/storage/memory_block_device.h"

#include <cstring>

constexpr std::uint64_t k64MiB = 64ull * 1024ull * 1024ull;

TEST_CASE(memory_device_reports_fixed_geometry) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    REQUIRE_EQ(dev.size_bytes(), k64MiB);
    REQUIRE_EQ(dev.sector_size(), 512u);
}

TEST_CASE(memory_device_starts_zeroed) {
    myrd::MemoryBlockDevice dev(4096, 512);
    std::byte buf[512]{};
    buf[0] = std::byte{0xFF}; // pre-fill
    auto r = dev.read(0, std::span<std::byte>(buf, 512));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 512u);
    REQUIRE(buf[0] == std::byte{0x00});
}

TEST_CASE(memory_device_round_trips_4096_bytes_at_8_mib) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    std::byte wbuf[4096];
    std::memset(wbuf, 0xAB, sizeof(wbuf));
    auto w = dev.write(8ull * 1024ull * 1024ull,
                       std::span<const std::byte>(wbuf, 4096));
    REQUIRE(w.ok());
    REQUIRE_EQ(w.value(), 4096u);
    std::byte rbuf[4096]{};
    auto r = dev.read(8ull * 1024ull * 1024ull,
                      std::span<std::byte>(rbuf, 4096));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 4096u);
    REQUIRE(std::memcmp(wbuf, rbuf, 4096) == 0);
}

TEST_CASE(memory_device_reads_last_sector) {
    myrd::MemoryBlockDevice dev(4096, 512);
    auto r = dev.read(4096 - 512, std::span<std::byte>(
        static_cast<std::byte*>(std::calloc(512, 1)), 512));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 512u);
    std::free(static_cast<void*>(r.value() ? nullptr : nullptr)); // nop cleanup
}

TEST_CASE(memory_device_rejects_zero_length) {
    myrd::MemoryBlockDevice dev(4096, 512);
    std::byte buf[1];
    auto r = dev.read(0, std::span<std::byte>(buf, 0));
    REQUIRE(r.has_error());
    REQUIRE_EQ(static_cast<int>(r.error().domain),
               static_cast<int>(myrd::ErrorDomain::storage));
}

TEST_CASE(memory_device_rejects_unaligned_offset) {
    myrd::MemoryBlockDevice dev(4096, 512);
    std::byte buf[512];
    auto r = dev.read(1, std::span<std::byte>(buf, 512));
    REQUIRE(r.has_error());
}

TEST_CASE(memory_device_rejects_unaligned_length) {
    myrd::MemoryBlockDevice dev(4096, 512);
    std::byte buf[511];
    auto r = dev.write(0, std::span<const std::byte>(buf, 511));
    REQUIRE(r.has_error());
}

TEST_CASE(memory_device_rejects_first_byte_past_end) {
    myrd::MemoryBlockDevice dev(4096, 512);
    std::byte buf[512];
    auto r = dev.read(4096, std::span<std::byte>(buf, 512));
    REQUIRE(r.has_error());
}

TEST_CASE(memory_device_rejects_overflowed_range) {
    myrd::MemoryBlockDevice dev(k64MiB, 512);
    std::byte buf[512];
    // offset near end so that offset + length would overflow size_t
    auto r = dev.read(k64MiB - 256, std::span<std::byte>(buf, 512));
    REQUIRE(r.has_error());
}
