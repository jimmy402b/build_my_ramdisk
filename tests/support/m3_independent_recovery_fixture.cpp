#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "m3_independent_recovery_fixture.h"

#include "test_paths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::test::m3_recovery_oracle {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kBaseLength = 6 * kMiB;
constexpr std::uint64_t kPayloadOffset = 4 * kMiB;
constexpr std::uint64_t kJournalLength = 0x16000;
constexpr std::uint64_t kDataOffset = 0x1000;
constexpr std::uint64_t kZeroOffset = 0x12000;
constexpr std::uint64_t kDiscardOffset = 0x13000;
constexpr std::uint64_t kRootOffset = 0x14000;
constexpr std::uint64_t kCommitOffset = 0x15000;
constexpr std::uint32_t kRootCrc = 0x9c9cdcb6U;
constexpr std::uint32_t kCommitCrc = 0xba75cf01U;

using Bytes16 = std::array<std::byte, 16>;
using Bytes32 = std::array<std::byte, 32>;

constexpr Bytes16 kImageUuid{
    std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
    std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
    std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
    std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}};
constexpr Bytes16 kJournalUuid{
    std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
    std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
    std::byte{0x28}, std::byte{0x29}, std::byte{0x2a}, std::byte{0x2b},
    std::byte{0x2c}, std::byte{0x2d}, std::byte{0x2e}, std::byte{0x2f}};
constexpr Bytes16 kFileWriteGuid{
    std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
    std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
    std::byte{0x38}, std::byte{0x39}, std::byte{0x3a}, std::byte{0x3b},
    std::byte{0x3c}, std::byte{0x3d}, std::byte{0x3e}, std::byte{0x3f}};
constexpr Bytes16 kDataWriteGuid{
    std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
    std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47},
    std::byte{0x48}, std::byte{0x49}, std::byte{0x4a}, std::byte{0x4b},
    std::byte{0x4c}, std::byte{0x4d}, std::byte{0x4e}, std::byte{0x4f}};
// Frozen by journal-v1-layout.md and journal-checkpoint-digest-v1.hex.
constexpr Bytes32 kCheckpointDigest{
    std::byte{0x63}, std::byte{0x2e}, std::byte{0x32}, std::byte{0xde},
    std::byte{0x34}, std::byte{0xe3}, std::byte{0xee}, std::byte{0xce},
    std::byte{0xeb}, std::byte{0x69}, std::byte{0x01}, std::byte{0x7a},
    std::byte{0x1a}, std::byte{0xc6}, std::byte{0xdf}, std::byte{0xfc},
    std::byte{0xa8}, std::byte{0x1a}, std::byte{0x20}, std::byte{0xf6},
    std::byte{0xea}, std::byte{0x9f}, std::byte{0x0f}, std::byte{0x60},
    std::byte{0x92}, std::byte{0xc0}, std::byte{0xb7}, std::byte{0x7b},
    std::byte{0x37}, std::byte{0x16}, std::byte{0x87}, std::byte{0x01}};

void put_u16(const std::span<std::byte> bytes, const std::size_t offset,
             const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(const std::span<std::byte> bytes, const std::size_t offset,
             const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(const std::span<std::byte> bytes, const std::size_t offset,
             const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::uint32_t get_u32(const std::span<const std::byte> bytes,
                      const std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

void put_ascii(const std::span<std::byte> bytes, const std::size_t offset,
               const std::string_view text) {
    std::transform(text.begin(), text.end(), bytes.begin() + offset,
                   [](const char value) {
                       return static_cast<std::byte>(value);
                   });
}

template <std::size_t Size>
void put_bytes(const std::span<std::byte> bytes, const std::size_t offset,
               const std::array<std::byte, Size>& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
}

std::uint32_t independent_crc32c(const std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

void seal_crc(const std::span<std::byte> bytes,
              const std::size_t crc_offset) {
    put_u32(bytes, crc_offset, 0);
    put_u32(bytes, crc_offset, independent_crc32c(bytes));
}

Bytes16 guid(const std::uint32_t a, const std::uint16_t b,
             const std::uint16_t c,
             const std::array<std::uint8_t, 8>& tail) {
    Bytes16 result{};
    put_u32(result, 0, a);
    put_u16(result, 4, b);
    put_u16(result, 6, c);
    for (std::size_t index = 0; index < tail.size(); ++index) {
        result[8 + index] = static_cast<std::byte>(tail[index]);
    }
    return result;
}

const auto kBatGuid = guid(
    0x2dc27766, 0xf623, 0x4200,
    {0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08});
const auto kMetadataGuid = guid(
    0x8b7ca206, 0x4790, 0x4b9a,
    {0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e});
const auto kFileParametersGuid = guid(
    0xcaa16737, 0xfa36, 0x4d43,
    {0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b});
const auto kVirtualSizeGuid = guid(
    0x2fa54224, 0xcd1b, 0x4876,
    {0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8});
const auto kVirtualIdGuid = guid(
    0xbeca12ab, 0xb2e6, 0x4523,
    {0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46});
const auto kLogicalSectorGuid = guid(
    0x8141bf1d, 0xa96f, 0x4709,
    {0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f});
const auto kPhysicalSectorGuid = guid(
    0xcda348c7, 0x445d, 0x4471,
    {0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56});

void build_vhdx_header(const std::span<std::byte> header,
                       const std::uint64_t sequence) {
    put_ascii(header, 0, "head");
    put_u64(header, 8, sequence);
    put_bytes(header, 16, kFileWriteGuid);
    put_bytes(header, 32, kDataWriteGuid);
    put_u16(header, 64, 0);
    put_u16(header, 66, 1);
    put_u32(header, 68, static_cast<std::uint32_t>(kMiB));
    put_u64(header, 72, 3 * kMiB);
    seal_crc(header, 4);
}

void put_region_entry(const std::span<std::byte> table,
                      const std::size_t entry, const Bytes16& id,
                      const std::uint64_t offset,
                      const std::uint32_t length) {
    const auto base = 16 + entry * 32;
    put_bytes(table, base, id);
    put_u64(table, base + 16, offset);
    put_u32(table, base + 24, length);
    put_u32(table, base + 28, 1);
}

void build_region_table(const std::span<std::byte> table) {
    put_ascii(table, 0, "regi");
    put_u32(table, 8, 2);
    put_region_entry(table, 0, kBatGuid, kMiB,
                     static_cast<std::uint32_t>(kMiB));
    put_region_entry(table, 1, kMetadataGuid, 2 * kMiB,
                     static_cast<std::uint32_t>(kMiB));
    seal_crc(table, 4);
}

void put_metadata_entry(const std::span<std::byte> table,
                        const std::size_t entry, const Bytes16& id,
                        const std::uint32_t offset,
                        const std::uint32_t length,
                        const std::uint32_t flags) {
    const auto base = 32 + entry * 32;
    put_bytes(table, base, id);
    put_u32(table, base + 16, offset);
    put_u32(table, base + 20, length);
    put_u32(table, base + 24, flags);
}

std::vector<std::byte> exact_dynamic_vhdx() {
    // Independently transcribed from the strict exact-byte golden profile in
    // m3_vhdx_format_tests.cpp. Payload zero is deliberately allocated and
    // nonzero so journal ZERO/DISCARD cannot accidentally fall through to the
    // base while still satisfying an AllZero-only oracle.
    std::vector<std::byte> bytes(kBaseLength);
    put_ascii(bytes, 0, "vhdxfile");
    build_vhdx_header(std::span<std::byte>{bytes}.subspan(0x10000, 4096), 10);
    build_vhdx_header(std::span<std::byte>{bytes}.subspan(0x20000, 4096), 11);
    build_region_table(std::span<std::byte>{bytes}.subspan(0x30000, 65536));
    build_region_table(std::span<std::byte>{bytes}.subspan(0x40000, 65536));

    auto metadata = std::span<std::byte>{bytes}.subspan(2 * kMiB, 65536);
    put_ascii(metadata, 0, "metadata");
    put_u16(metadata, 10, 5);
    put_metadata_entry(metadata, 0, kFileParametersGuid, 0x10000, 8, 4);
    put_metadata_entry(metadata, 1, kVirtualSizeGuid, 0x10008, 8, 6);
    put_metadata_entry(metadata, 2, kVirtualIdGuid, 0x10010, 16, 6);
    put_metadata_entry(metadata, 3, kLogicalSectorGuid, 0x10020, 4, 6);
    put_metadata_entry(metadata, 4, kPhysicalSectorGuid, 0x10024, 4, 6);
    auto items = std::span<std::byte>{bytes}.subspan(2 * kMiB + 0x10000, 40);
    put_u32(items, 0, 2U * 1024U * 1024U);
    put_u64(items, 8, kVirtualBytes);
    const auto disk_id = guid(
        0xdeadbeef, 0x1111, 0x2222,
        {0x80, 0x81, 1, 3, 5, 7, 9, 11});
    put_bytes(items, 16, disk_id);
    put_u32(items, 32, 512);
    put_u32(items, 36, 4096);
    put_u64(bytes, kMiB, ((kPayloadOffset / kMiB) << 20U) | 6U);
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(kPayloadOffset),
              bytes.end(), std::byte{0x5d});
    return bytes;
}

int hex_nibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::vector<std::byte> load_sparse_fixture(const char* path,
                                           const std::size_t size) {
    std::ifstream input(path);
    if (!input.good()) throw std::runtime_error("cannot open frozen fixture");
    std::vector<std::byte> bytes(size);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string offset_text;
        std::string hex;
        row >> offset_text >> hex;
        if (offset_text.empty() || hex.empty() || hex.size() % 2 != 0) {
            throw std::runtime_error("invalid frozen fixture row");
        }
        const auto offset = static_cast<std::size_t>(
            std::stoull(offset_text, nullptr, 16));
        if (offset > bytes.size() || hex.size() / 2 > bytes.size() - offset) {
            throw std::runtime_error("frozen fixture row is out of range");
        }
        for (std::size_t index = 0; index < hex.size() / 2; ++index) {
            const auto high = hex_nibble(hex[index * 2]);
            const auto low = hex_nibble(hex[index * 2 + 1]);
            if (high < 0 || low < 0) {
                throw std::runtime_error("invalid frozen fixture hex");
            }
            bytes[offset + index] =
                static_cast<std::byte>((high << 4) | low);
        }
    }
    return bytes;
}

std::vector<std::byte> frozen_journal() {
    std::vector<std::byte> journal(kJournalLength);
    const auto install = [&](const char* path, const std::size_t size,
                             const std::size_t offset) {
        const auto fixture = load_sparse_fixture(path, size);
        std::copy(fixture.begin(), fixture.end(), journal.begin() + offset);
    };
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_HEADER_FIXTURE_PATH), 4096, 0);
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_DATA_FIXTURE_PATH), 69632, kDataOffset);
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_ZERO_FIXTURE_PATH), 4096, kZeroOffset);
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_DISCARD_FIXTURE_PATH), 4096, kDiscardOffset);
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_INDEX_FIXTURE_PATH), 4096, kRootOffset);
    install(MYRD_TEST_PATH(MYRD_M3_RECOVERY_COMMIT_FIXTURE_PATH), 4096, kCommitOffset);
    if (get_u32(journal, kRootOffset + 32) != kRootCrc ||
        get_u32(journal, kCommitOffset + 32) != kCommitCrc ||
        !std::equal(kCheckpointDigest.begin(), kCheckpointDigest.end(),
                    journal.begin() + kCommitOffset + 160)) {
        throw std::runtime_error("frozen journal descriptors drifted");
    }
    return journal;
}

std::array<std::byte, 4096> manifest_slot(const SlotSpec& spec) {
    std::array<std::byte, 4096> bytes{};
    put_ascii(bytes, 0, "MYRDRDM\0");
    put_u32(bytes, 8, 1);
    put_u32(bytes, 12, 0);
    put_u32(bytes, 16, 4096);
    bytes[20] = static_cast<std::byte>(spec.slot_index);
    put_bytes(bytes, 32, kImageUuid);
    put_u64(bytes, 48, spec.generation);
    put_u64(bytes, 56, kVirtualBytes);
    put_u32(bytes, 64, 512);
    put_u32(bytes, 68, 4096);
    put_u32(bytes, 72, static_cast<std::uint32_t>(kLogicalBlockBytes));
    constexpr std::string_view base_name = "base.vhdx";
    constexpr std::string_view journal_name = "journal.rdj";
    put_u16(bytes, 80, static_cast<std::uint16_t>(base_name.size()));
    put_u16(bytes, 82, static_cast<std::uint16_t>(journal_name.size()));
    put_ascii(bytes, 88, base_name);
    put_ascii(bytes, 344, journal_name);
    put_bytes(bytes, 600, kFileWriteGuid);
    put_bytes(bytes, 616, kDataWriteGuid);
    put_u64(bytes, 632, kBaseLength);
    put_u64(bytes, 640, 0);
    put_bytes(bytes, 648, kJournalUuid);
    put_u64(bytes, 664, 1);
    put_u64(bytes, 672, kJournalLength);
    put_u64(bytes, 680, kCommitOffset);
    put_u64(bytes, 688, 4096);
    put_u32(bytes, 696, kCommitCrc);
    put_bytes(bytes, 704, kCheckpointDigest);
    put_u64(bytes, 736, kRootOffset);
    put_u64(bytes, 744, 4096);
    put_u32(bytes, 752, kRootCrc);
    put_u64(bytes, 760, 1);

    switch (spec.mutation) {
    case SlotMutation::None:
        break;
    case SlotMutation::BaseIdentity:
        bytes[600] ^= std::byte{1};
        break;
    case SlotMutation::RootCrc:
        bytes[752] ^= std::byte{1};
        break;
    case SlotMutation::ChainDigest:
        bytes[704] ^= std::byte{1};
        break;
    }
    seal_crc(bytes, 24);
    return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) throw std::runtime_error("cannot create fixture file");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) throw std::runtime_error("cannot write fixture file");
}

void damage_byte(const std::filesystem::path& path,
                 const std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.good()) throw std::runtime_error("cannot open fixture mutation");
    file.seekg(static_cast<std::streamoff>(offset));
    char value{};
    file.read(&value, 1);
    value ^= 1;
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    if (!file.good()) throw std::runtime_error("cannot mutate fixture byte");
}

}  // namespace

IndependentRecoveryFixture::IndependentRecoveryFixture() {
    static std::atomic<std::uint64_t> sequence{};
    root_ = std::filesystem::temp_directory_path() /
        (L"myrd-task26-oracle-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(sequence.fetch_add(1)));
    std::filesystem::create_directories(root_);
    write_bytes(root_ / L"base.vhdx", exact_dynamic_vhdx());
    write_bytes(root_ / L"journal.rdj", frozen_journal());
}

IndependentRecoveryFixture::~IndependentRecoveryFixture() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
}

const std::filesystem::path& IndependentRecoveryFixture::root() const noexcept {
    return root_;
}

void IndependentRecoveryFixture::publish(
    const std::optional<SlotSpec>& a,
    const std::optional<SlotSpec>& b) const {
    std::array<std::byte, 8192> manifest{};
    if (a.has_value()) {
        const auto slot = manifest_slot(*a);
        std::copy(slot.begin(), slot.end(), manifest.begin());
    }
    if (b.has_value()) {
        const auto slot = manifest_slot(*b);
        std::copy(slot.begin(), slot.end(), manifest.begin() + 4096);
    }
    write_bytes(root_ / L"disk.rdm", manifest);
}

void IndependentRecoveryFixture::damage_root_record() const {
    damage_byte(root_ / L"journal.rdj", kRootOffset + 256);
}

void IndependentRecoveryFixture::damage_chain_covered_data() const {
    damage_byte(root_ / L"journal.rdj", kDataOffset + 4096 + 17);
}

}  // namespace myrd::test::m3_recovery_oracle
