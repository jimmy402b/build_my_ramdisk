#include "tests/support/test_framework.h"

#include "src/control/ipc_codec.h"
#include "src/control/ipc_frame.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace {

std::atomic_bool g_track_allocations{false};
std::atomic_size_t g_largest_allocation{};

void record_allocation(std::size_t size) noexcept {
    if (!g_track_allocations.load(std::memory_order_relaxed)) return;
    auto observed = g_largest_allocation.load(std::memory_order_relaxed);
    while (observed < size
           && !g_largest_allocation.compare_exchange_weak(
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

void mutate(std::vector<std::byte>& bytes, FixedRng& rng,
            std::size_t case_index) {
    const auto random = rng.next();
    switch (case_index % 8U) {
    case 0:
        if (!bytes.empty()) {
            const auto offset = static_cast<std::size_t>(random % bytes.size());
            bytes[offset] ^= static_cast<std::byte>(1U << (random % 8U));
        }
        break;
    case 1:
        if (!bytes.empty()) {
            bytes.resize(static_cast<std::size_t>(random % bytes.size()));
        }
        break;
    case 2:
        bytes.push_back(static_cast<std::byte>(random & 0xffU));
        break;
    case 3:
        if (bytes.size() >= myrd::ipc::kFrameHeaderBytes) {
            bytes[14] = std::byte{0xff};
            bytes[15] = std::byte{0xff};
            bytes[16] = std::byte{0xff};
            bytes[17] = std::byte{0x7f};
        }
        break;
    case 4:
        if (bytes.size() > myrd::ipc::kFrameHeaderBytes) {
            const auto payload_bytes = bytes.size() - myrd::ipc::kFrameHeaderBytes;
            const auto offset = myrd::ipc::kFrameHeaderBytes
                + static_cast<std::size_t>(random % payload_bytes);
            bytes[offset] = static_cast<std::byte>((random >> 8U) & 0xffU);
        }
        break;
    case 5:
        if (bytes.size() >= myrd::ipc::kFrameHeaderBytes) {
            bytes[8] = static_cast<std::byte>(random & 0xffU);
            bytes[9] = static_cast<std::byte>((random >> 8U) & 0xffU);
        }
        break;
    case 6:
        if (bytes.size() >= myrd::ipc::kFrameHeaderBytes) {
            bytes[50] = static_cast<std::byte>(random & 0xffU);
            bytes[51] = static_cast<std::byte>((random >> 8U) & 0xffU);
            bytes[52] = static_cast<std::byte>((random >> 16U) & 0xffU);
            bytes[53] = static_cast<std::byte>((random >> 24U) & 0xffU);
        }
        break;
    case 7:
        if (bytes.size() > myrd::ipc::kFrameHeaderBytes + 7U) {
            const auto field = myrd::ipc::kFrameHeaderBytes;
            bytes[field + 4] = std::byte{0xff};
            bytes[field + 5] = std::byte{0xff};
            bytes[field + 6] = std::byte{0xff};
            bytes[field + 7] = std::byte{0x7f};
        }
        break;
    }
}

} // namespace

void* operator new(std::size_t size) {
    record_allocation(size);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

TEST_CASE(ipc_mutation_fuzz_runs_exact_seeded_case_count_with_bounded_allocations) {
    constexpr std::uint64_t seed = 0x4D595244C0DEC0DEULL;
    constexpr std::size_t case_count = 100000;

    myrd::control::Command command = myrd::control::DiskConfigSetCommand{
        L"WorkDisk", {{L"checkpoint.interval", L"5m"},
                       {L"checkpoint.enabled", L"true"}}};
    auto encoded_command = myrd::ipc::encode_command_payload(command);
    REQUIRE(encoded_command.ok());
    myrd::ipc::Frame frame{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::request,
        0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        static_cast<std::uint32_t>(encoded_command.value().command_id),
        encoded_command.value().payload};
    auto corpus = myrd::ipc::encode_frame(frame);
    REQUIRE(corpus.ok());

    FixedRng rng(seed);
    std::size_t executed = 0;
    for (; executed < case_count; ++executed) {
        auto candidate = corpus.value();
        mutate(candidate, rng, executed);

        AllocationProbe allocations;
        auto decoded_frame = myrd::ipc::decode_frame(candidate);
        if (decoded_frame.ok()) {
            (void)myrd::ipc::decode_command_payload(
                static_cast<myrd::ipc::CommandId>(decoded_frame.value().command_id),
                decoded_frame.value().payload,
                decoded_frame.value().protocol_minor);
        }
        REQUIRE(allocations.largest() <= myrd::ipc::kMaxPayloadBytes);
    }

    REQUIRE_EQ(executed, case_count);
    std::cout << "M4 IPC fuzz seed=0x4D595244C0DEC0DE cases="
              << executed << std::endl;
}

TEST_CASE(ipc_allocation_probe_observes_near_limit_static_library_allocation) {
    constexpr std::size_t field_header_bytes = 8;
    constexpr std::size_t debug_allocator_bookkeeping_allowance = 64;
    constexpr std::uint32_t value_bytes =
        myrd::ipc::kMaxPayloadBytes - field_header_bytes;
    constexpr std::array<myrd::ipc::FieldDescriptor, 1> fields{{
        {1, myrd::ipc::FieldKind::bytes,
         myrd::ipc::FieldRequirement::required,
         myrd::ipc::FieldDisposition::consume, 0, value_bytes, 0},
    }};
    const myrd::ipc::CommandSchema schema{77, 1, fields};

    std::vector<std::byte> payload;
    payload.reserve(myrd::ipc::kMaxPayloadBytes);
    payload.push_back(std::byte{1});
    payload.push_back(std::byte{0});
    payload.push_back(static_cast<std::byte>(myrd::ipc::FieldKind::bytes));
    payload.push_back(static_cast<std::byte>(
        myrd::ipc::FieldRequirement::required));
    for (unsigned shift = 0; shift < 32; shift += 8) {
        payload.push_back(static_cast<std::byte>((value_bytes >> shift) & 0xffU));
    }
    payload.resize(myrd::ipc::kMaxPayloadBytes, std::byte{0x5a});

    AllocationProbe allocations;
    auto decoded = myrd::ipc::decode_typed_payload(payload, schema, 0);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().allocated_value_bytes,
               static_cast<std::size_t>(value_bytes));
    std::cout << "M4 IPC allocation calibration largest="
              << allocations.largest() << " payload=" << value_bytes
              << std::endl;
    REQUIRE(allocations.largest() >= value_bytes);
    REQUIRE(allocations.largest()
            <= myrd::ipc::kMaxPayloadBytes
                + debug_allocator_bookkeeping_allowance);
}
