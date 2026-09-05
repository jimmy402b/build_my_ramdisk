#include "tests/support/test_framework.h"

#include "src/common/sha256.h"
#include "src/profile/profile_codec.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::atomic_bool g_track_allocations{false};
std::atomic_size_t g_largest_allocation{};

void record_allocation(std::size_t size) noexcept {
    if (!g_track_allocations.load(std::memory_order_relaxed)) return;
    auto observed = g_largest_allocation.load(std::memory_order_relaxed);
    while (observed < size &&
           !g_largest_allocation.compare_exchange_weak(
               observed, size, std::memory_order_relaxed)) {
    }
}

class AllocationProbe final {
public:
    AllocationProbe() noexcept {
        g_largest_allocation.store(0, std::memory_order_relaxed);
        g_track_allocations.store(true, std::memory_order_release);
    }
    ~AllocationProbe() {
        g_track_allocations.store(false, std::memory_order_release);
    }
    std::size_t largest() const noexcept {
        return g_largest_allocation.load(std::memory_order_relaxed);
    }
};

class FixedRng final {
public:
    explicit FixedRng(std::uint64_t state) noexcept : state_(state) {}
    std::uint64_t next() noexcept {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 0x2545F4914F6CDD1DULL;
    }
private:
    std::uint64_t state_;
};

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::profile::ProfileRecord corpus_record() {
    using namespace myrd::profile;
    Profile profile{
        uuid("00112233-4455-6677-8899-aabbccddeeff"), L"S-1-5-18", L"A",
        1'700'000'000'000ULL, ProfileLifecycle::provisioning, L"D:\\B",
        "disk.rdm", uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
        8ULL << 30, 512, 4096, std::nullopt, std::nullopt, std::nullopt,
        MountKind::drive_letter, L"R:", true,
        MemoryPolicy{MemoryMode::automatic, 1ULL << 30, 896ULL << 20,
                     768ULL << 20, 128ULL << 20},
        CheckpointPolicy{}, CompactionPolicy{}};
    return {7, std::move(profile)};
}

void put_u16(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void refresh_hash(std::vector<std::byte>& bytes) {
    auto hash = myrd::sha256_digest(
        std::span<const std::byte>(bytes).subspan(
            myrd::profile::kProfileHeaderBytes));
    REQUIRE(hash.ok());
    std::copy(hash.value().begin(), hash.value().end(), bytes.begin() + 40);
}

void make_malformed(std::vector<std::byte>& bytes, FixedRng& rng,
                    std::size_t case_index) {
    const auto random = rng.next();
    constexpr std::size_t kOwnerValueOffset = 104;
    constexpr std::size_t kAutoStartValueOffset = 289;
    constexpr std::size_t kLastFieldOffset = 494;
    switch (case_index % 13U) {
    case 0:
        bytes[40 + random % 32U] ^= std::byte{1};
        break;
    case 1:
        bytes.resize(bytes.size() - 1U - random % 32U);
        break;
    case 2:
        bytes.push_back(static_cast<std::byte>(random));
        break;
    case 3:
        put_u64(bytes, 16, UINT64_MAX);
        break;
    case 4:
        put_u32(bytes, 32, UINT32_MAX);
        break;
    case 5:
        put_u32(bytes, 36, myrd::profile::kProfileMaximumFields + 1);
        break;
    case 6:
        bytes[74] = std::byte{0xff};
        refresh_hash(bytes);
        break;
    case 7:
        put_u32(bytes, 76, UINT32_MAX);
        refresh_hash(bytes);
        break;
    case 8:
        put_u16(bytes, kLastFieldOffset, 34);
        refresh_hash(bytes);
        break;
    case 9:
        bytes[kOwnerValueOffset] = std::byte{0xff};
        refresh_hash(bytes);
        break;
    case 10:
        bytes[kAutoStartValueOffset] = std::byte{2};
        refresh_hash(bytes);
        break;
    case 11:
        put_u16(bytes, kLastFieldOffset, 36);
        refresh_hash(bytes);
        break;
    case 12:
        put_u16(bytes, kLastFieldOffset, 36);
        bytes[kLastFieldOffset + 3] = std::byte{0};
        refresh_hash(bytes);
        break;
    }
}

} // namespace

void* operator new(std::size_t size) {
    record_allocation(size);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

TEST_CASE(profile_mutation_fuzz_runs_exact_seeded_count_with_bounded_allocations) {
    constexpr std::uint64_t seed = 0x4D59524450524631ULL;
    constexpr std::size_t case_count = 100'000;
    auto encoded = myrd::profile::encode_profile(corpus_record());
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value().size(), 506U);

    FixedRng rng(seed);
    std::size_t rejected = 0;
    for (std::size_t index = 0; index < case_count; ++index) {
        auto candidate = encoded.value();
        make_malformed(candidate, rng, index);
        AllocationProbe allocations;
        auto decoded = myrd::profile::decode_profile(candidate);
        if (decoded.has_error()) ++rejected;
        REQUIRE(allocations.largest() <= myrd::profile::kProfileMaximumBytes);
    }
    REQUIRE_EQ(rejected, case_count);
    std::cout << "M4 profile fuzz seed=0x4D59524450524631 cases="
              << rejected << std::endl;
}

TEST_CASE(profile_allocation_probe_observes_bounded_large_text_decode) {
    auto record = corpus_record();
    record.profile.bundle_path = L"D:\\" + std::wstring(30'000, L'x');
    auto encoded = myrd::profile::encode_profile(record);
    REQUIRE(encoded.ok());

    AllocationProbe allocations;
    auto decoded = myrd::profile::decode_profile(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE(allocations.largest() >= 30'000);
    REQUIRE(allocations.largest() <= myrd::profile::kProfileMaximumBytes);
}
