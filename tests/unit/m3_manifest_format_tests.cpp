#include "test_framework.h"

#include "src/persistence/manifest_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string>

namespace myrd::persistence {
namespace {

void put_u16(ManifestSlotBytes& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void put_u32(ManifestSlotBytes& bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

void put_u64(ManifestSlotBytes& bytes, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

std::uint32_t independent_crc32c(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : data) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void refresh_crc(ManifestSlotBytes& bytes) {
    put_u32(bytes, 24, 0);
    put_u32(bytes, 24, independent_crc32c(bytes));
}

void require_format_rejected(std::span<const std::byte> bytes) {
    const auto decoded = decode_manifest_slot(bytes);
    REQUIRE(!decoded.ok());
    REQUIRE_EQ(decoded.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(decoded.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::InvalidFormat));
}

void require_format_rejected_with_message(
    std::span<const std::byte> bytes, std::wstring_view message) {
    const auto decoded = decode_manifest_slot(bytes);
    REQUIRE(!decoded.ok());
    REQUIRE_EQ(decoded.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(decoded.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::InvalidFormat));
    REQUIRE_EQ(decoded.error().message, message);
}

void zero_range(ManifestSlotBytes& bytes, std::size_t offset,
                std::size_t length) {
    std::fill_n(bytes.begin() + offset, length, std::byte{0});
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

ManifestSlot sample_slot() {
    ManifestSlot slot{};
    slot.slot_index = 1;
    for (std::size_t i = 0; i < 16; ++i) {
        slot.image_uuid.bytes[i] = static_cast<std::byte>(0x10U + i);
        slot.base_identity.file_write_guid[i] = static_cast<std::byte>(0x20U + i);
        slot.base_identity.data_write_guid[i] = static_cast<std::byte>(0x30U + i);
        slot.journal_identity.journal_uuid[i] = static_cast<std::byte>(0x40U + i);
    }
    slot.generation.checkpoint_generation = 5;
    slot.generation.base_generation = 4;
    slot.generation.journal.epoch = 1;
    slot.generation.journal.committed_length = 0x6000;
    slot.generation.journal.commit.offset = 0x5000;
    slot.generation.journal.commit.length = 0x1000;
    slot.generation.journal.commit.crc32c = 0xA1B2C3D4U;
    for (std::size_t i = 0; i < 32; ++i) {
        slot.generation.journal.commit.chain_digest[i] =
            static_cast<std::byte>(0x50U + i);
    }
    slot.generation.journal.root.offset = 0x4000;
    slot.generation.journal.root.length = 0x1000;
    slot.generation.journal.root.crc32c = 0x01020304U;
    slot.geometry = {64ULL * 1024 * 1024, 512, 4096};
    slot.base_relative_name = "base-0005.vhdx";
    slot.base_expected_length = 8ULL * 1024 * 1024;
    slot.journal_relative_name = "journal-e0001.rdj";
    slot.feature_flags = 1;
    slot.created_time_100ns = 0x1122334455667788ULL;
    slot.committed_time_100ns = 0x8877665544332211ULL;
    return slot;
}

ManifestSlotBytes independent_golden() {
    ManifestSlotBytes bytes{};
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'R'}, std::byte{'D'}, std::byte{'M'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(bytes, 8, 1);
    put_u32(bytes, 12, 0);
    put_u32(bytes, 16, 4096);
    bytes[20] = std::byte{1};
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[32 + i] = static_cast<std::byte>(0x10U + i);
    }
    put_u64(bytes, 48, 5);
    put_u64(bytes, 56, 64ULL * 1024 * 1024);
    put_u32(bytes, 64, 512);
    put_u32(bytes, 68, 4096);
    put_u32(bytes, 72, 64U * 1024U);
    put_u16(bytes, 80, 14);
    put_u16(bytes, 82, 17);
    const std::string base = "base-0005.vhdx";
    const std::string journal = "journal-e0001.rdj";
    for (std::size_t i = 0; i < base.size(); ++i) {
        bytes[88 + i] = static_cast<std::byte>(base[i]);
    }
    for (std::size_t i = 0; i < journal.size(); ++i) {
        bytes[344 + i] = static_cast<std::byte>(journal[i]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[600 + i] = static_cast<std::byte>(0x20U + i);
        bytes[616 + i] = static_cast<std::byte>(0x30U + i);
        bytes[648 + i] = static_cast<std::byte>(0x40U + i);
    }
    put_u64(bytes, 632, 8ULL * 1024 * 1024);
    put_u64(bytes, 640, 4);
    put_u64(bytes, 664, 1);
    put_u64(bytes, 672, 0x6000);
    put_u64(bytes, 680, 0x5000);
    put_u64(bytes, 688, 0x1000);
    put_u32(bytes, 696, 0xA1B2C3D4U);
    for (std::size_t i = 0; i < 32; ++i) {
        bytes[704 + i] = static_cast<std::byte>(0x50U + i);
    }
    put_u64(bytes, 736, 0x4000);
    put_u64(bytes, 744, 0x1000);
    put_u32(bytes, 752, 0x01020304U);
    put_u64(bytes, 760, 1);
    put_u64(bytes, 768, 0x1122334455667788ULL);
    put_u64(bytes, 776, 0x8877665544332211ULL);
    put_u32(bytes, 24, independent_crc32c(bytes));
    return bytes;
}

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

ManifestSlotBytes load_sparse_fixture() {
    std::ifstream input{MYRD_M3_MANIFEST_V1_FIXTURE_PATH};
    REQUIRE(input.good());
    ManifestSlotBytes bytes{};
    std::array<bool, kManifestSlotBytes> assigned{};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields{line};
        std::string offset_text;
        std::string hex;
        REQUIRE(static_cast<bool>(fields >> offset_text >> hex));
        const auto offset = static_cast<std::size_t>(
            std::stoul(offset_text, nullptr, 16));
        REQUIRE_EQ(hex.size() % 2, 0ull);
        REQUIRE(offset + hex.size() / 2 <= bytes.size());
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const auto high = hex_nibble(hex[i]);
            const auto low = hex_nibble(hex[i + 1]);
            REQUIRE(high >= 0);
            REQUIRE(low >= 0);
            const auto target = offset + i / 2;
            REQUIRE(!assigned[target]);
            assigned[target] = true;
            bytes[target] = static_cast<std::byte>((high << 4) | low);
        }
    }
    return bytes;
}

TEST_CASE(manifest_v1_hand_builder_matches_frozen_fixture) {
    REQUIRE_EQ(independent_golden(), load_sparse_fixture());
}

TEST_CASE(manifest_v1_encoder_matches_independent_golden_bytes) {
    const auto encoded = encode_manifest_slot(sample_slot());
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value(), independent_golden());
}

TEST_CASE(manifest_v1_decoder_reads_independent_golden_bytes) {
    const auto decoded = decode_manifest_slot(independent_golden());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), sample_slot());
}

TEST_CASE(manifest_v1_crc_covers_every_byte_including_reserved_space) {
    const auto golden = independent_golden();
    for (std::size_t offset = 0; offset < golden.size(); ++offset) {
        auto corrupt = golden;
        corrupt[offset] ^= std::byte{1};
        if (offset < 8) {
            require_format_rejected_with_message(
                corrupt, L"manifest magic mismatch");
        } else if ((offset >= 8 && offset < 12) ||
                   (offset >= 16 && offset < 20)) {
            require_format_rejected_with_message(
                corrupt, L"unsupported version or slot length");
        } else {
            require_format_rejected_with_message(
                corrupt, L"manifest CRC32C mismatch");
        }
    }
}

TEST_CASE(manifest_v1_reserved_and_padding_bytes_are_individually_rejected) {
    constexpr std::array<std::pair<std::size_t, std::size_t>, 7> reserved{
        std::pair{21ull, 3ull}, std::pair{28ull, 4ull},
        std::pair{76ull, 4ull}, std::pair{84ull, 4ull},
        std::pair{700ull, 4ull}, std::pair{756ull, 4ull},
        std::pair{784ull, kManifestSlotBytes - 784ull}};
    for (const auto [begin, length] : reserved) {
        for (std::size_t offset = begin; offset < begin + length; ++offset) {
            auto corrupt = independent_golden();
            corrupt[offset] = std::byte{1};
            refresh_crc(corrupt);
            require_format_rejected_with_message(
                corrupt, L"reserved bytes are nonzero");
        }
    }

    constexpr std::array<std::pair<std::size_t, std::size_t>, 2> padding{
        std::pair{88ull + 14ull, 256ull - 14ull},
        std::pair{344ull + 17ull, 256ull - 17ull}};
    for (const auto [begin, length] : padding) {
        for (std::size_t offset = begin; offset < begin + length; ++offset) {
            auto corrupt = independent_golden();
            corrupt[offset] = std::byte{1};
            refresh_crc(corrupt);
            require_format_rejected_with_message(
                corrupt, L"name padding is nonzero");
        }
    }
}

TEST_CASE(manifest_v1_decoder_rejects_every_truncation_and_extra_bytes) {
    const auto golden = independent_golden();
    for (std::size_t size = 0; size < golden.size(); ++size) {
        require_format_rejected(
            std::span<const std::byte>{golden.data(), size});
    }

    std::array<std::byte, kManifestSlotBytes + 1> extra{};
    std::copy(golden.begin(), golden.end(), extra.begin());
    require_format_rejected(extra);
}

TEST_CASE(manifest_v1_decoder_applies_header_and_reserved_rules_after_crc) {
    auto bytes = independent_golden();
    bytes[0] = std::byte{'X'};
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u32(bytes, 8, kM3FormatMajor + 1);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u32(bytes, 16, kManifestSlotBytes - 1);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    bytes[20] = std::byte{2};
    refresh_crc(bytes);
    require_format_rejected(bytes);

    constexpr std::array<std::size_t, 7> reserved_offsets{
        21, 28, 76, 84, 700, 756, 4095};
    for (const auto offset : reserved_offsets) {
        bytes = independent_golden();
        bytes[offset] = std::byte{1};
        refresh_crc(bytes);
        require_format_rejected(bytes);
    }

    bytes = independent_golden();
    put_u32(bytes, 12, 99);  // Unknown optional minor remains readable.
    refresh_crc(bytes);
    const auto optional_minor = decode_manifest_slot(bytes);
    REQUIRE(optional_minor.ok());
    REQUIRE_EQ(optional_minor.value(), sample_slot());
}

TEST_CASE(manifest_v1_decoder_rejects_invalid_names_after_crc) {
    auto bytes = independent_golden();
    put_u16(bytes, 80, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u16(bytes, 82, 256);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u16(bytes, 80, 2);
    zero_range(bytes, 88, 256);
    bytes[88] = std::byte{0xC3};
    bytes[89] = std::byte{0x28};
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    const std::string escape = "..\\escape.vhdx";
    put_u16(bytes, 80, static_cast<std::uint16_t>(escape.size()));
    zero_range(bytes, 88, 256);
    std::transform(escape.begin(), escape.end(), bytes.begin() + 88,
                   [](char value) { return static_cast<std::byte>(value); });
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    bytes[88 + 14] = std::byte{1};
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u16(bytes, 82, 2);
    zero_range(bytes, 344, 256);
    bytes[344] = std::byte{0xC3};
    bytes[345] = std::byte{0x28};
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    const std::string journal_escape = "..\\escape.rdj";
    put_u16(bytes, 82,
            static_cast<std::uint16_t>(journal_escape.size()));
    zero_range(bytes, 344, 256);
    std::transform(journal_escape.begin(), journal_escape.end(),
                   bytes.begin() + 344,
                   [](char value) { return static_cast<std::byte>(value); });
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    bytes[344 + 17] = std::byte{1};
    refresh_crc(bytes);
    require_format_rejected(bytes);
}

TEST_CASE(manifest_v1_decoder_rejects_invalid_geometry_and_identity) {
    auto bytes = independent_golden();
    zero_range(bytes, 32, 16);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    constexpr std::array<std::size_t, 3> identity_offsets{600, 616, 648};
    for (const auto offset : identity_offsets) {
        bytes = independent_golden();
        zero_range(bytes, offset, 16);
        refresh_crc(bytes);
        require_format_rejected(bytes);
    }

    bytes = independent_golden();
    put_u64(bytes, 56, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u32(bytes, 64, 1024);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u32(bytes, 68, 512);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u32(bytes, 72, static_cast<std::uint32_t>(kM2BlockSize / 2));
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 632, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);
}

TEST_CASE(manifest_v1_decoder_rejects_invalid_generation_and_ranges) {
    auto bytes = independent_golden();
    put_u64(bytes, 48, kInvalidGeneration);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 640, 6);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 640, kInvalidGeneration);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    for (const auto epoch : {std::uint64_t{0}, kInvalidGeneration}) {
        bytes = independent_golden();
        put_u64(bytes, 664, epoch);
        refresh_crc(bytes);
        require_format_rejected(bytes);
    }

    bytes = independent_golden();
    put_u64(bytes, 672, kJournalAlignment);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 672, 0x5FFF);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 672, 0x7000);  // Commit must end at committed length.
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 680, 0x5001);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 680, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 688, 0x2000);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 680, (std::numeric_limits<std::uint64_t>::max)() - 0xFFF);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 736, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 736, 0);
    put_u64(bytes, 744, 0);
    // A canonical empty root has no checksum either.
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 736, 0x6000);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 736, 0x4001);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 736,
            (std::numeric_limits<std::uint64_t>::max)() - 0xFFF);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 744, 0);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 744, 0x2000);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 760, 1ULL << 63U);
    refresh_crc(bytes);
    require_format_rejected(bytes);

    bytes = independent_golden();
    put_u64(bytes, 776, 1);
    refresh_crc(bytes);
    const auto backwards_diagnostic_clock = decode_manifest_slot(bytes);
    REQUIRE(backwards_diagnostic_clock.ok());
    REQUIRE_EQ(backwards_diagnostic_clock.value().committed_time_100ns, 1ull);
}

TEST_CASE(manifest_v1_encoder_rejects_invalid_public_values) {
    auto slot = sample_slot();
    slot.slot_index = 2;
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.base_relative_name.clear();
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.base_relative_name = "..\\escape.vhdx";
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.image_uuid = {};
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.generation.base_generation =
        slot.generation.checkpoint_generation + 1;
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.generation.journal.committed_length += kJournalAlignment;
    REQUIRE(!encode_manifest_slot(slot).ok());

    slot = sample_slot();
    slot.feature_flags = 1ULL << 63U;
    REQUIRE(!encode_manifest_slot(slot).ok());
}

TEST_CASE(manifest_v1_encoder_supports_frozen_boundary_values) {
    for (const auto slot_index : {std::uint8_t{0}, std::uint8_t{1}}) {
        auto slot = sample_slot();
        slot.slot_index = slot_index;
        slot.generation.checkpoint_generation = 0;
        slot.generation.base_generation = 0;
        slot.generation.journal.root = {};
        slot.base_relative_name.assign(kMaxRelativeNameBytes, 'a');
        slot.journal_relative_name.assign(kMaxRelativeNameBytes, 'b');
        slot.feature_flags = 0xFFFFFFFFULL;
        slot.created_time_100ns = 0;
        slot.committed_time_100ns = 0;

        const auto first = encode_manifest_slot(slot);
        const auto second = encode_manifest_slot(slot);
        REQUIRE(first.ok());
        REQUIRE(second.ok());
        REQUIRE_EQ(first.value(), second.value());
        const auto decoded = decode_manifest_slot(first.value());
        REQUIRE(decoded.ok());
        REQUIRE_EQ(decoded.value(), slot);
    }

    auto maximum = sample_slot();
    maximum.generation.checkpoint_generation = kInvalidGeneration - 1;
    maximum.generation.base_generation = kInvalidGeneration - 1;
    maximum.generation.journal.epoch = kInvalidGeneration - 1;
    const auto encoded = encode_manifest_slot(maximum);
    REQUIRE(encoded.ok());
    const auto decoded = decode_manifest_slot(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), maximum);

    auto reused_root = sample_slot();
    reused_root.generation.journal.root.offset = kJournalAlignment;
    const auto reused_root_bytes = encode_manifest_slot(reused_root);
    REQUIRE(reused_root_bytes.ok());
    const auto reused_root_decoded =
        decode_manifest_slot(reused_root_bytes.value());
    REQUIRE(reused_root_decoded.ok());
    REQUIRE_EQ(reused_root_decoded.value(), reused_root);
}

TEST_CASE(manifest_v1_round_trip_is_deterministic_for_random_valid_values) {
    std::uint64_t state = 0x4D5952444D334D31ULL;
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
        auto slot = sample_slot();
        slot.slot_index = static_cast<std::uint8_t>(next_random(state) & 1U);
        const auto checkpoint = next_random(state) % 1'000'000ULL;
        slot.generation.checkpoint_generation = checkpoint;
        slot.generation.base_generation = checkpoint == 0
            ? 0
            : next_random(state) % (checkpoint + 1);
        slot.generation.journal.epoch = 1 + next_random(state) % 1'000'000ULL;
        const auto commit_page = 2 + next_random(state) % 32;
        slot.generation.journal.commit.offset =
            commit_page * kJournalAlignment;
        slot.generation.journal.commit.length = kJournalAlignment;
        slot.generation.journal.committed_length =
            slot.generation.journal.commit.offset + kJournalAlignment;
        slot.generation.journal.commit.crc32c =
            static_cast<std::uint32_t>(next_random(state));
        if ((next_random(state) & 1U) == 0) {
            slot.generation.journal.root = {};
        } else {
            const auto root_page =
                1 + next_random(state) % (commit_page - 1);
            slot.generation.journal.root.offset =
                root_page * kIndexNodeBytes;
            slot.generation.journal.root.length = kIndexNodeBytes;
            slot.generation.journal.root.crc32c =
                static_cast<std::uint32_t>(next_random(state));
        }
        const auto logical = (next_random(state) & 1U) == 0 ? 512U : 4096U;
        slot.geometry.logical_sector_size = logical;
        slot.geometry.virtual_bytes =
            (1 + next_random(state) % 1'000'000ULL) * logical;
        slot.feature_flags = next_random(state) & 0xFFFFFFFFULL;
        slot.base_expected_length = next_random(state);
        if (slot.base_expected_length == 0) slot.base_expected_length = 1;
        slot.base_relative_name = "base-" + std::to_string(iteration) + "-" +
            std::to_string(next_random(state)) + ".vhdx";
        slot.journal_relative_name = "journal-" +
            std::to_string(iteration) + "-" +
            std::to_string(next_random(state)) + ".rdj";
        slot.created_time_100ns = next_random(state) >> 1U;
        slot.committed_time_100ns = slot.created_time_100ns +
            next_random(state) % 1'000'000ULL;
        for (auto& value : slot.image_uuid.bytes) {
            value = static_cast<std::byte>(next_random(state));
        }
        slot.image_uuid.bytes[0] |= std::byte{1};
        for (auto& value : slot.base_identity.file_write_guid) {
            value = static_cast<std::byte>(next_random(state));
        }
        slot.base_identity.file_write_guid[0] |= std::byte{1};
        for (auto& value : slot.base_identity.data_write_guid) {
            value = static_cast<std::byte>(next_random(state));
        }
        slot.base_identity.data_write_guid[0] |= std::byte{1};
        for (auto& value : slot.journal_identity.journal_uuid) {
            value = static_cast<std::byte>(next_random(state));
        }
        slot.journal_identity.journal_uuid[0] |= std::byte{1};
        for (auto& value : slot.generation.journal.commit.chain_digest) {
            value = static_cast<std::byte>(next_random(state));
        }

        const auto first = encode_manifest_slot(slot);
        const auto second = encode_manifest_slot(slot);
        REQUIRE(first.ok());
        REQUIRE(second.ok());
        REQUIRE_EQ(first.value(), second.value());
        const auto decoded = decode_manifest_slot(first.value());
        REQUIRE(decoded.ok());
        REQUIRE_EQ(decoded.value(), slot);
    }
}

}  // namespace
}  // namespace myrd::persistence
