#include "test_framework.h"

#include "src/proxy/request_processor.h"
#include "tests/support/fake_block_device.h"

#include <array>
#include <cstring>
#include <random>
#include <vector>

using namespace myrd;
using namespace myrd::proxy;

static constexpr std::uint64_t kSeed = 0x4D3150524F585955ull;
static constexpr int kCases = 10000;

TEST_CASE(m1_fuzz_deterministic_10000_cases) {
    std::mt19937_64 rng(kSeed);

    test::FakeBlockDevice fake;
    RequestProcessor proc(fake);
    std::vector<std::byte> mapping(kMappingSize, std::byte{0xA5});

    const std::uint64_t valid_codes[] = {
        kReqInfo, kReqRead, kReqWrite, kReqClose, kReqUnmap, kReqZero
    };

    for (int i = 0; i < kCases; ++i) {
        // Randomize only header area (first 4 KiB) — the processor
        // only reads from the header; data area is not validated by fuzz.
        std::uniform_int_distribution<std::uint64_t> dist;
        for (std::size_t j = 0; j < kHeaderSize / 8; ++j) {
            std::uint64_t val = dist(rng);
            std::memcpy(mapping.data() + j * 8, &val, 8);
        }

        // 80% valid code, 20% arbitrary
        std::uint64_t code;
        if (dist(rng) % 100 < 80) {
            code = valid_codes[dist(rng) % 6];
        } else {
            code = dist(rng);
        }

        write_wire<std::uint64_t>(mapping, 0, code);

        // 10% chance: truncate the mapping
        std::span<std::byte> logical_mapping(mapping);
        if (dist(rng) % 100 < 10) {
            std::size_t trunc_size =
                static_cast<std::size_t>(dist(rng) % (kMappingSize + 256));
            if (trunc_size > mapping.size()) trunc_size = mapping.size();
            logical_mapping = logical_mapping.subspan(0, trunc_size);
        }

        // 5% chance: inject backend error
        if (dist(rng) % 100 < 5) {
            fake.next_result = Result<void>::failure({ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::backing_read_failed),
                L"fuzz", L"injected"});
        } else {
            fake.next_result = Result<void>::success();
        }

        // For valid range operations, ensure the encoded length looks
        // plausible in the header
        if (code == kReqUnmap || code == kReqZero) {
            // 30% chance: put a reasonable range count in the length field
            if (dist(rng) % 100 < 30) {
                std::uint64_t count = dist(rng) % 16;
                std::uint64_t enc_len = count * sizeof(DataSetRangeWire);
                if (enc_len <= kDataAreaSize) {
                    write_wire<std::uint64_t>(mapping, 8, enc_len);
                }
            }
        }

        std::uint64_t before = fake.total_calls();
        OperationContext ctx{};
        auto outcome = proc.process(ctx, logical_mapping);

        // Core invariants
        REQUIRE(outcome.action == ProcessorAction::respond ||
                outcome.action == ProcessorAction::close ||
                outcome.action == ProcessorAction::fatal);
        REQUIRE(fake.invalid_call_count == 0);
        REQUIRE(fake.total_calls() >= before);
        REQUIRE(fake.total_calls() <= before + 1);

        // Reset fake state for next iteration
        fake.next_result = Result<void>::success();
    }
}
