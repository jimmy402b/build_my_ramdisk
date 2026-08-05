#include "test_framework.h"

#include "src/persistence/vhdx_format.h"
#include "src/persistence/persistence_types.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace myrd::persistence {
namespace {

static_assert(!std::is_default_constructible_v<VhdxLayout>);

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kFixtureLength = 4 * kMiB;

using Bytes16 = std::array<std::byte, 16>;

void put_u16(std::span<std::byte> bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::span<std::byte> bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(std::span<std::byte> bytes, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_ascii(std::span<std::byte> bytes, std::size_t offset,
               std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(text[index]);
    }
}

Bytes16 guid(std::uint32_t a, std::uint16_t b, std::uint16_t c,
             std::array<std::uint8_t, 8> tail) {
    Bytes16 result{};
    put_u32(result, 0, a);
    put_u16(result, 4, b);
    put_u16(result, 6, c);
    for (std::size_t index = 0; index < tail.size(); ++index) {
        result[8 + index] = static_cast<std::byte>(tail[index]);
    }
    return result;
}

void put_guid(std::span<std::byte> bytes, std::size_t offset,
              const Bytes16& value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[offset + index] = value[index];
    }
}

std::uint32_t independent_crc32c(std::span<const std::byte> bytes) {
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

// The test freezes canonical GUID fields and independently converts the
// mixed-endian Windows GUID representation to wire bytes.  Production keeps
// separately transcribed wire constants, so one endian mistake cannot make
// both sides agree accidentally.
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
const auto kParentLocatorGuid = guid(
    0xa8d35f2d, 0xb30b, 0x454d,
    {0xab, 0xf7, 0xd3, 0xd8, 0x48, 0x34, 0xab, 0x0c});

void put_literal_guid(std::span<std::byte> bytes, std::size_t offset,
                      const Bytes16& value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[offset + index] = value[index];
    }
}

struct GoldenVhdx {
    std::vector<std::byte> identifier = std::vector<std::byte>(8);
    std::vector<std::byte> header1 = std::vector<std::byte>(4096);
    std::vector<std::byte> header2 = std::vector<std::byte>(4096);
    std::vector<std::byte> region1 = std::vector<std::byte>(65536);
    std::vector<std::byte> region2 = std::vector<std::byte>(65536);
    std::vector<std::byte> metadata_table = std::vector<std::byte>(65536);
    std::vector<std::byte> metadata_items = std::vector<std::byte>(40);
    std::uint64_t metadata_file_offset{2 * kMiB};
    std::uint64_t file_length{kFixtureLength};
};

void seal_checksum(std::span<std::byte> bytes) {
    put_u32(bytes, 4, 0);
    put_u32(bytes, 4, independent_crc32c(bytes));
}

void build_header(std::span<std::byte> header, std::uint64_t sequence,
                  std::uint8_t identity_bias) {
    put_ascii(header, 0, "head");
    put_u64(header, 8, sequence);
    auto file_guid = guid(0x10203040U + identity_bias, 0x5060, 0x7080,
                          {0x90, 0xa0, 1, 2, 3, 4, 5, 6});
    auto data_guid = guid(0x11223344U + identity_bias, 0x5566, 0x7788,
                          {0x99, 0xaa, 6, 5, 4, 3, 2, 1});
    put_guid(header, 16, file_guid);
    put_guid(header, 32, data_guid);
    put_u16(header, 64, 0);
    put_u16(header, 66, 1);
    put_u32(header, 68, static_cast<std::uint32_t>(kMiB));
    put_u64(header, 72, 3 * kMiB);
    seal_checksum(header);
}

void put_region_entry(std::span<std::byte> table, std::size_t entry,
                      const Bytes16& id,
                      std::uint64_t offset, std::uint32_t length,
                      std::uint32_t required = 1) {
    const auto base = 16 + entry * 32;
    put_literal_guid(table, base, id);
    put_u64(table, base + 16, offset);
    put_u32(table, base + 24, length);
    put_u32(table, base + 28, required);
}

void build_region_table(std::span<std::byte> table) {
    put_ascii(table, 0, "regi");
    put_u32(table, 8, 2);
    put_region_entry(table, 0, kBatGuid, kMiB,
                     static_cast<std::uint32_t>(kMiB));
    put_region_entry(table, 1, kMetadataGuid, 2 * kMiB,
                     static_cast<std::uint32_t>(kMiB));
    seal_checksum(table);
}

void put_metadata_entry(std::span<std::byte> table, std::size_t entry,
                        const Bytes16& id,
                        std::uint32_t offset, std::uint32_t length,
                        std::uint32_t flags) {
    const auto base = 32 + entry * 32;
    put_literal_guid(table, base, id);
    put_u32(table, base + 16, offset);
    put_u32(table, base + 20, length);
    put_u32(table, base + 24, flags);
}

GoldenVhdx golden_vhdx() {
    GoldenVhdx result{};
    put_ascii(result.identifier, 0, "vhdxfile");
    build_header(result.header1, 10, 0);
    build_header(result.header2, 11, 1);
    build_region_table(result.region1);
    result.region2 = result.region1;

    put_ascii(result.metadata_table, 0, "metadata");
    put_u16(result.metadata_table, 10, 5);
    put_metadata_entry(result.metadata_table, 0, kFileParametersGuid,
                       0x10000, 8, 4);
    put_metadata_entry(result.metadata_table, 1, kVirtualSizeGuid,
                       0x10008, 8, 6);
    put_metadata_entry(result.metadata_table, 2, kVirtualIdGuid,
                       0x10010, 16, 6);
    put_metadata_entry(result.metadata_table, 3, kLogicalSectorGuid,
                       0x10020, 4, 6);
    put_metadata_entry(result.metadata_table, 4, kPhysicalSectorGuid,
                       0x10024, 4, 6);
    put_u32(result.metadata_items, 0, 2U * 1024U * 1024U);
    put_u32(result.metadata_items, 4, 0);
    put_u64(result.metadata_items, 8, 64 * kMiB);
    const auto disk_id = guid(0xdeadbeef, 0x1111, 0x2222,
                              {0x80, 0x81, 1, 3, 5, 7, 9, 11});
    put_guid(result.metadata_items, 16, disk_id);
    put_u32(result.metadata_items, 32, 512);
    put_u32(result.metadata_items, 36, 4096);
    return result;
}

struct OpenFixture {
    std::shared_ptr<IPersistenceFile> file;
    std::shared_ptr<test::FaultFileState> state;
};

OpenFixture open_fixture(
    const GoldenVhdx& image,
    std::vector<test::FaultFileRule> rules = {},
    test::FaultFileObserver observer = {}) {
    auto backing = test::make_fault_file(
        image.file_length, std::move(rules), std::move(observer));
    OpenFixture result{
        std::shared_ptr<IPersistenceFile>(std::move(backing.file)),
        std::move(backing.state)};
    REQUIRE(result.file->write_exact(0, image.identifier).ok());
    REQUIRE(result.file->write_exact(0x10000, image.header1).ok());
    REQUIRE(result.file->write_exact(0x20000, image.header2).ok());
    REQUIRE(result.file->write_exact(0x30000, image.region1).ok());
    REQUIRE(result.file->write_exact(0x40000, image.region2).ok());
    REQUIRE(result.file->write_exact(image.metadata_file_offset,
                                     image.metadata_table).ok());
    REQUIRE(result.file->write_exact(image.metadata_file_offset + 0x10000,
                                     image.metadata_items).ok());
    return result;
}

Result<VhdxLayout> parse(const GoldenVhdx& image) {
    auto opened = open_fixture(image);
    return parse_vhdx_layout(OperationContext{}, std::move(opened.file));
}

void require_error(const Result<VhdxLayout>& result,
                   PersistenceErrorCode code) {
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(code));
}

TEST_CASE(vhdx_parser_accepts_independent_strict_dynamic_golden_bytes) {
    auto image = golden_vhdx();
    auto result = parse(image);
    REQUIRE(result.ok());
    const auto& layout = result.value();
    REQUIRE_EQ(layout.virtual_size(), 64 * kMiB);
    REQUIRE_EQ(layout.block_size(), 2U * 1024U * 1024U);
    REQUIRE_EQ(layout.logical_sector_size(), 512U);
    REQUIRE_EQ(layout.physical_sector_size(), 4096U);
    REQUIRE_EQ(layout.bat_offset(), kMiB);
    REQUIRE_EQ(layout.bat_length(), kMiB);
    REQUIRE_EQ(layout.bat_entry_count(), 32ULL);
    REQUIRE_EQ(layout.payload_block_count(), 32ULL);
    REQUIRE_EQ(layout.metadata_offset(), 2 * kMiB);
    REQUIRE_EQ(layout.metadata_length(), kMiB);
    REQUIRE_EQ(layout.log_offset(), 3 * kMiB);
    REQUIRE_EQ(layout.log_length(), kMiB);
    REQUIRE_EQ(layout.file_length(), kFixtureLength);
    REQUIRE(std::equal(
        layout.file_identity().file_write_guid.begin(),
        layout.file_identity().file_write_guid.end(),
        image.header2.begin() + 16));
    REQUIRE(std::equal(
        layout.file_identity().data_write_guid.begin(),
        layout.file_identity().data_write_guid.end(),
        image.header2.begin() + 32));
}

TEST_CASE(vhdx_parser_selects_highest_valid_header_and_surviving_region_copy) {
    auto image = golden_vhdx();
    image.header2[100] ^= std::byte{1};
    image.region1[100] ^= std::byte{1};
    auto result = parse(image);
    REQUIRE(result.ok());
    REQUIRE(std::equal(
        result.value().file_identity().file_write_guid.begin(),
        result.value().file_identity().file_write_guid.end(),
        image.header1.begin() + 16));
}

TEST_CASE(vhdx_parser_rejects_tied_valid_headers_and_conflicting_regions) {
    auto tied = golden_vhdx();
    put_u64(tied.header2, 8, 10);
    seal_checksum(tied.header2);
    require_error(parse(tied), PersistenceErrorCode::InvalidFormat);

    auto conflict = golden_vhdx();
    put_u64(conflict.region2, 16 + 32 + 16, 3 * kMiB);
    seal_checksum(conflict.region2);
    require_error(parse(conflict), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_computes_interleaved_bat_geometry_at_chunk_boundaries) {
    auto logical_512 = golden_vhdx();
    put_u64(logical_512.metadata_items, 8, 4ULL * 1024 * kMiB + 2 * kMiB);
    auto result_512 = parse(logical_512);
    REQUIRE(result_512.ok());
    REQUIRE_EQ(result_512.value().payload_block_count(), 2049ULL);
    REQUIRE_EQ(result_512.value().chunk_ratio(), 2048ULL);
    REQUIRE_EQ(result_512.value().bat_entry_count(), 2050ULL);

    auto logical_4096 = golden_vhdx();
    put_u64(logical_4096.metadata_items, 8,
            32ULL * 1024 * kMiB + 2 * kMiB);
    put_u32(logical_4096.metadata_items, 32, 4096);
    auto result_4096 = parse(logical_4096);
    REQUIRE(result_4096.ok());
    REQUIRE_EQ(result_4096.value().payload_block_count(), 16385ULL);
    REQUIRE_EQ(result_4096.value().chunk_ratio(), 16384ULL);
    REQUIRE_EQ(result_4096.value().bat_entry_count(), 16386ULL);
}

TEST_CASE(vhdx_parser_accepts_exact_64_tib_with_sufficient_sparse_bat) {
    auto image = golden_vhdx();
    constexpr std::uint64_t maximum_size =
        64ULL * 1024 * 1024 * 1024 * 1024;
    constexpr std::uint32_t bat_length = 257U * 1024U * 1024U;
    constexpr std::uint64_t metadata_offset = 258 * kMiB;
    constexpr std::uint64_t log_offset = 259 * kMiB;
    image.file_length = 260 * kMiB;
    image.metadata_file_offset = metadata_offset;
    put_u64(image.metadata_items, 8, maximum_size);
    for (auto* header : {&image.header1, &image.header2}) {
        put_u64(*header, 72, log_offset);
        seal_checksum(*header);
    }
    for (auto* table : {&image.region1, &image.region2}) {
        put_u32(*table, 16 + 24, bat_length);
        put_u64(*table, 16 + 32 + 16, metadata_offset);
        seal_checksum(*table);
    }
    auto result = parse(image);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().virtual_size(), maximum_size);
    REQUIRE_EQ(result.value().payload_block_count(), 33554432ULL);
    REQUIRE_EQ(result.value().bat_entry_count(), 33570815ULL);
    REQUIRE_EQ(result.value().bat_length(),
               static_cast<std::uint64_t>(bat_length));
}

TEST_CASE(vhdx_parser_rejects_bad_root_signatures_and_checksums) {
    auto identifier = golden_vhdx();
    identifier.identifier[0] = std::byte{'x'};
    require_error(parse(identifier), PersistenceErrorCode::InvalidFormat);

    auto headers = golden_vhdx();
    headers.header1[100] ^= std::byte{1};
    headers.header2[100] ^= std::byte{1};
    require_error(parse(headers), PersistenceErrorCode::InvalidFormat);

    auto regions = golden_vhdx();
    regions.region1[100] ^= std::byte{1};
    regions.region2[100] ^= std::byte{1};
    require_error(parse(regions), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_unsupported_header_and_file_parameter_features) {
    auto active_log = golden_vhdx();
    active_log.header2[48] = std::byte{1};
    seal_checksum(active_log.header2);
    require_error(parse(active_log), PersistenceErrorCode::UnsupportedVhdx);

    auto fixed = golden_vhdx();
    put_u32(fixed.metadata_items, 4, 1);
    require_error(parse(fixed), PersistenceErrorCode::UnsupportedVhdx);

    auto parent = golden_vhdx();
    put_u32(parent.metadata_items, 4, 2);
    require_error(parse(parent), PersistenceErrorCode::UnsupportedVhdx);

    auto block_size = golden_vhdx();
    put_u32(block_size.metadata_items, 0, 4U * 1024U * 1024U);
    require_error(parse(block_size), PersistenceErrorCode::UnsupportedVhdx);
}

TEST_CASE(vhdx_parser_rejects_header_versions_nil_identity_and_bad_log_ranges) {
    auto version = golden_vhdx();
    put_u16(version.header2, 66, 2);
    seal_checksum(version.header2);
    require_error(parse(version), PersistenceErrorCode::UnsupportedVhdx);

    auto log_version = golden_vhdx();
    put_u16(log_version.header2, 64, 1);
    seal_checksum(log_version.header2);
    require_error(parse(log_version), PersistenceErrorCode::UnsupportedVhdx);

    auto nil_file_id = golden_vhdx();
    std::fill_n(nil_file_id.header2.begin() + 16, 16, std::byte{0});
    seal_checksum(nil_file_id.header2);
    require_error(parse(nil_file_id), PersistenceErrorCode::InvalidFormat);

    auto nil_data_id = golden_vhdx();
    std::fill_n(nil_data_id.header2.begin() + 32, 16, std::byte{0});
    seal_checksum(nil_data_id.header2);
    require_error(parse(nil_data_id), PersistenceErrorCode::InvalidFormat);

    auto short_log = golden_vhdx();
    put_u32(short_log.header2, 68, 4096);
    seal_checksum(short_log.header2);
    require_error(parse(short_log), PersistenceErrorCode::InvalidFormat);

    auto unaligned_log = golden_vhdx();
    put_u64(unaligned_log.header2, 72, 3 * kMiB + 4096);
    seal_checksum(unaligned_log.header2);
    require_error(parse(unaligned_log), PersistenceErrorCode::InvalidFormat);

    auto log_outside_file = golden_vhdx();
    put_u64(log_outside_file.header2, 72, 4 * kMiB);
    seal_checksum(log_outside_file.header2);
    require_error(parse(log_outside_file), PersistenceErrorCode::InvalidFormat);

    auto log_overlaps_metadata = golden_vhdx();
    put_u64(log_overlaps_metadata.header2, 72, 2 * kMiB);
    seal_checksum(log_overlaps_metadata.header2);
    require_error(parse(log_overlaps_metadata),
                  PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_missing_duplicate_unknown_and_parent_metadata) {
    auto missing = golden_vhdx();
    put_u16(missing.metadata_table, 10, 4);
    require_error(parse(missing), PersistenceErrorCode::InvalidFormat);

    auto duplicate = golden_vhdx();
    put_literal_guid(duplicate.metadata_table, 32 + 4 * 32,
                     kLogicalSectorGuid);
    require_error(parse(duplicate), PersistenceErrorCode::InvalidFormat);

    auto unknown = golden_vhdx();
    put_literal_guid(unknown.metadata_table, 32, kParentLocatorGuid);
    require_error(parse(unknown), PersistenceErrorCode::UnsupportedVhdx);
}

TEST_CASE(vhdx_parser_rejects_missing_and_duplicate_regions) {
    auto missing = golden_vhdx();
    put_u32(missing.region1, 8, 1);
    seal_checksum(missing.region1);
    missing.region2 = missing.region1;
    require_error(parse(missing), PersistenceErrorCode::InvalidFormat);

    auto duplicate = golden_vhdx();
    put_literal_guid(duplicate.region1, 16 + 32, kBatGuid);
    seal_checksum(duplicate.region1);
    duplicate.region2 = duplicate.region1;
    require_error(parse(duplicate), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_region_count_required_length_and_file_bounds) {
    auto count = golden_vhdx();
    put_u32(count.region1, 8, 2048);
    seal_checksum(count.region1);
    count.region2 = count.region1;
    require_error(parse(count), PersistenceErrorCode::InvalidFormat);

    auto not_required = golden_vhdx();
    put_u32(not_required.region1, 16 + 28, 0);
    seal_checksum(not_required.region1);
    not_required.region2 = not_required.region1;
    require_error(parse(not_required), PersistenceErrorCode::InvalidFormat);

    auto zero_length = golden_vhdx();
    put_u32(zero_length.region1, 16 + 24, 0);
    seal_checksum(zero_length.region1);
    zero_length.region2 = zero_length.region1;
    require_error(parse(zero_length), PersistenceErrorCode::InvalidFormat);

    auto outside_file = golden_vhdx();
    put_u64(outside_file.region1, 16 + 16, 4 * kMiB);
    seal_checksum(outside_file.region1);
    outside_file.region2 = outside_file.region1;
    require_error(parse(outside_file), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_metadata_header_entry_and_header_reserved_damage) {
    auto metadata_signature = golden_vhdx();
    metadata_signature.metadata_table[0] = std::byte{'x'};
    require_error(parse(metadata_signature), PersistenceErrorCode::InvalidFormat);

    auto metadata_overlap = golden_vhdx();
    put_u32(metadata_overlap.metadata_table, 32 + 16, 0x10008);
    require_error(parse(metadata_overlap), PersistenceErrorCode::InvalidFormat);

    auto metadata_reserved = golden_vhdx();
    put_u32(metadata_reserved.metadata_table, 32 + 28, 1);
    require_error(parse(metadata_reserved), PersistenceErrorCode::InvalidFormat);

    auto header_reserved = golden_vhdx();
    header_reserved.header2[80] = std::byte{1};
    seal_checksum(header_reserved.header2);
    require_error(parse(header_reserved), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_metadata_flags_lengths_bounds_and_nil_disk_id) {
    auto flags = golden_vhdx();
    put_u32(flags.metadata_table, 32 + 24, 6);
    require_error(parse(flags), PersistenceErrorCode::InvalidFormat);

    auto length = golden_vhdx();
    put_u32(length.metadata_table, 32 + 20, 4);
    require_error(parse(length), PersistenceErrorCode::InvalidFormat);

    auto table_overlap = golden_vhdx();
    put_u32(table_overlap.metadata_table, 32 + 16, 0x1000);
    require_error(parse(table_overlap), PersistenceErrorCode::InvalidFormat);

    auto outside_region = golden_vhdx();
    put_u32(outside_region.metadata_table, 32 + 16,
            static_cast<std::uint32_t>(kMiB));
    require_error(parse(outside_region), PersistenceErrorCode::InvalidFormat);

    auto nil_disk_id = golden_vhdx();
    std::fill_n(nil_disk_id.metadata_items.begin() + 16, 16, std::byte{0});
    require_error(parse(nil_disk_id), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_virtual_size_limits_and_undersized_bat) {
    auto zero = golden_vhdx();
    put_u64(zero.metadata_items, 8, 0);
    require_error(parse(zero), PersistenceErrorCode::InvalidFormat);

    auto too_large = golden_vhdx();
    put_u64(too_large.metadata_items, 8,
            64ULL * 1024 * 1024 * 1024 * 1024 + 512);
    require_error(parse(too_large), PersistenceErrorCode::InvalidFormat);

    auto bat_too_small = golden_vhdx();
    put_u64(bat_too_small.metadata_items, 8,
            64ULL * 1024 * 1024 * 1024 * 1024);
    require_error(parse(bat_too_small), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_invalid_geometry_reserved_flags_and_ranges) {
    auto physical = golden_vhdx();
    put_u32(physical.metadata_items, 36, 512);
    require_error(parse(physical), PersistenceErrorCode::UnsupportedVhdx);

    auto unaligned_size = golden_vhdx();
    put_u64(unaligned_size.metadata_items, 8, 64 * kMiB + 1);
    require_error(parse(unaligned_size), PersistenceErrorCode::InvalidFormat);

    auto flags = golden_vhdx();
    put_u32(flags.metadata_items, 4, 4);
    require_error(parse(flags), PersistenceErrorCode::InvalidFormat);

    auto region_alignment = golden_vhdx();
    put_u64(region_alignment.region1, 16 + 16, kMiB + 4096);
    seal_checksum(region_alignment.region1);
    region_alignment.region2 = region_alignment.region1;
    require_error(parse(region_alignment), PersistenceErrorCode::InvalidFormat);

    auto overlap = golden_vhdx();
    put_u64(overlap.region1, 16 + 32 + 16, kMiB);
    seal_checksum(overlap.region1);
    overlap.region2 = overlap.region1;
    require_error(parse(overlap), PersistenceErrorCode::InvalidFormat);

    auto overflow = golden_vhdx();
    put_u64(overflow.region1, 16 + 16,
            (std::numeric_limits<std::uint64_t>::max)() - kMiB + 1);
    seal_checksum(overflow.region1);
    overflow.region2 = overflow.region1;
    require_error(parse(overflow), PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_rejects_unknown_required_region_and_metadata_item) {
    auto region = golden_vhdx();
    region.region1[16] ^= std::byte{0x40};
    seal_checksum(region.region1);
    region.region2 = region.region1;
    require_error(parse(region), PersistenceErrorCode::UnsupportedVhdx);

    auto metadata = golden_vhdx();
    metadata.metadata_table[32] ^= std::byte{0x40};
    require_error(parse(metadata), PersistenceErrorCode::UnsupportedVhdx);
}

TEST_CASE(vhdx_parser_rejects_truncation_at_every_exact_read_stage) {
    // Read calls are identifier, header 1/2, region 1/2, metadata table,
    // then the five required metadata items.
    for (std::uint64_t occurrence = 1; occurrence <= 11; ++occurrence) {
        auto image = golden_vhdx();
        auto opened = open_fixture(
            image,
            {{test::FaultFilePhase::Read, occurrence, std::nullopt,
              test::FaultFileAction::ZeroProgress, 0}});
        auto result = parse_vhdx_layout(
            OperationContext{}, std::move(opened.file));
        require_error(result, PersistenceErrorCode::InvalidFormat);
    }

    auto physically_short = golden_vhdx();
    auto opened = open_fixture(physically_short);
    REQUIRE(opened.file->truncate(kMiB - 1).ok());
    auto result = parse_vhdx_layout(
        OperationContext{}, std::move(opened.file));
    require_error(result, PersistenceErrorCode::InvalidFormat);
}

TEST_CASE(vhdx_parser_boundary_fuzz_and_checksum_mutations_fail_closed) {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };

    // Deterministically sample physical truncation lengths over the full
    // four-MiB structural file.  Every shorter length must fail without an
    // exception, allocation proportional to virtual size, or fallback data.
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
        auto image = golden_vhdx();
        auto opened = open_fixture(image);
        const auto length = next() % kFixtureLength;
        REQUIRE(opened.file->truncate(length).ok());
        auto result = parse_vhdx_layout(
            OperationContext{}, std::move(opened.file));
        require_error(result, PersistenceErrorCode::InvalidFormat);
    }

    // Mutate both redundant copies so fallback cannot hide checksum damage.
    // The independent fixture builder never calls production checksum code.
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
        auto image = golden_vhdx();
        if ((next() & 1U) == 0) {
            const auto offset = static_cast<std::size_t>(next() % 4096);
            const auto bit = static_cast<unsigned>(next() % 8);
            image.header1[offset] ^= static_cast<std::byte>(1U << bit);
            image.header2[offset] ^= static_cast<std::byte>(1U << bit);
        } else {
            const auto offset = static_cast<std::size_t>(next() % 65536);
            const auto bit = static_cast<unsigned>(next() % 8);
            image.region1[offset] ^= static_cast<std::byte>(1U << bit);
            image.region2[offset] ^= static_cast<std::byte>(1U << bit);
        }
        require_error(parse(image), PersistenceErrorCode::InvalidFormat);
    }
}

TEST_CASE(vhdx_parser_honors_cancel_and_deadline_before_io) {
    auto image = golden_vhdx();
    auto cancelled = open_fixture(image);
    std::stop_source stop;
    stop.request_stop();
    OperationContext cancel_context{};
    cancel_context.stop_token = stop.get_token();
    auto cancel_result = parse_vhdx_layout(
        cancel_context, std::move(cancelled.file));
    REQUIRE(!cancel_result.ok());
    REQUIRE_EQ(cancel_result.error().domain, ErrorDomain::storage);

    auto expired = open_fixture(image);
    OperationContext deadline_context{};
    deadline_context.deadline = std::chrono::steady_clock::time_point::min();
    auto deadline_result = parse_vhdx_layout(
        deadline_context, std::move(expired.file));
    REQUIRE(!deadline_result.ok());
    REQUIRE_EQ(deadline_result.error().domain, ErrorDomain::storage);
}

TEST_CASE(vhdx_parser_honors_cancel_after_io_and_maps_native_read_errors) {
    auto image = golden_vhdx();
    std::stop_source stop;
    auto cancelled = open_fixture(
        image, {}, [&stop](test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 1) {
                stop.request_stop();
            }
        });
    OperationContext context{};
    context.stop_token = stop.get_token();
    auto cancelled_result = parse_vhdx_layout(
        context, std::move(cancelled.file));
    REQUIRE(!cancelled_result.ok());
    REQUIRE_EQ(cancelled_result.error().domain, ErrorDomain::storage);

    auto denied = open_fixture(
        image,
        {{test::FaultFilePhase::Read, 1, std::nullopt,
          test::FaultFileAction::NativeError, 5}});
    auto denied_result = parse_vhdx_layout(
        OperationContext{}, std::move(denied.file));
    require_error(denied_result, PersistenceErrorCode::AccessDenied);
    REQUIRE_EQ(denied_result.error().native_code, 5U);

    auto failed = open_fixture(
        image,
        {{test::FaultFilePhase::Length, 1, std::nullopt,
          test::FaultFileAction::NativeError, 1117}});
    auto failed_result = parse_vhdx_layout(
        OperationContext{}, std::move(failed.file));
    require_error(failed_result, PersistenceErrorCode::IoError);
    REQUIRE_EQ(failed_result.error().native_code, 1117U);
}

TEST_CASE(vhdx_parser_propagates_cancel_and_io_error_at_every_read_stage) {
    for (std::uint64_t occurrence = 1; occurrence <= 11; ++occurrence) {
        auto image = golden_vhdx();
        std::stop_source stop;
        auto cancelled = open_fixture(
            image, {},
            [&stop, occurrence](test::FaultFilePhase phase,
                                std::uint64_t call) {
                if (phase == test::FaultFilePhase::Read &&
                    call == occurrence) {
                    stop.request_stop();
                }
            });
        OperationContext context{};
        context.stop_token = stop.get_token();
        auto result = parse_vhdx_layout(
            context, std::move(cancelled.file));
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
    }

    for (std::uint64_t occurrence = 1; occurrence <= 11; ++occurrence) {
        auto image = golden_vhdx();
        auto failed = open_fixture(
            image,
            {{test::FaultFilePhase::Read, occurrence, std::nullopt,
              test::FaultFileAction::NativeError, 1117}});
        auto result = parse_vhdx_layout(
            OperationContext{}, std::move(failed.file));
        require_error(result, PersistenceErrorCode::IoError);
        REQUIRE_EQ(result.error().native_code, 1117U);
    }

    auto image = golden_vhdx();
    OperationContext deadline{};
    bool final_read_observed = false;
    auto delayed = open_fixture(
        image, {}, [&deadline, &final_read_observed](
                       test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 11) {
                final_read_observed = true;
                deadline.deadline =
                    std::chrono::steady_clock::time_point::min();
            }
        });
    auto deadline_result = parse_vhdx_layout(
        deadline, std::move(delayed.file));
    REQUIRE(final_read_observed);
    REQUIRE(!deadline_result.ok());
    REQUIRE_EQ(deadline_result.error().domain, ErrorDomain::storage);
}

}  // namespace
}  // namespace myrd::persistence
