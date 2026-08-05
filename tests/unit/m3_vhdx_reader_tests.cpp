#include "test_framework.h"

#include "src/common/crc32c.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kDefaultFileLength = 64 * kMiB;

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
               const char* text) {
    for (std::size_t index = 0; text[index] != '\0'; ++index) {
        bytes[offset + index] = static_cast<std::byte>(text[index]);
    }
}

void put_guid(std::span<std::byte> bytes, std::size_t offset,
              const std::array<std::uint8_t, 16>& value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(value[index]);
    }
}

void seal(std::span<std::byte> bytes) {
    put_u32(bytes, 4, 0);
    put_u32(bytes, 4, crc32c(bytes));
}

constexpr std::array<std::uint8_t, 16> kBatGuid{
    0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
    0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
constexpr std::array<std::uint8_t, 16> kMetadataGuid{
    0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
    0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
constexpr std::array<std::uint8_t, 16> kFileParametersGuid{
    0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
    0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
constexpr std::array<std::uint8_t, 16> kVirtualSizeGuid{
    0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
    0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
constexpr std::array<std::uint8_t, 16> kVirtualIdGuid{
    0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
    0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46};
constexpr std::array<std::uint8_t, 16> kLogicalSectorGuid{
    0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
    0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
constexpr std::array<std::uint8_t, 16> kPhysicalSectorGuid{
    0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
    0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};

struct ReaderImage final {
    std::unique_ptr<IPersistenceFile> file;
    std::shared_ptr<test::FaultFileState> state;
    std::optional<VhdxLayout> layout;
    WindowsFileIdentity windows_identity{};
};

void put_region_entry(std::span<std::byte> table, std::size_t index,
                      const std::array<std::uint8_t, 16>& guid,
                      std::uint64_t offset, std::uint32_t length) {
    const auto base = 16 + index * 32;
    put_guid(table, base, guid);
    put_u64(table, base + 16, offset);
    put_u32(table, base + 24, length);
    put_u32(table, base + 28, 1);
}

void put_metadata_entry(std::span<std::byte> table, std::size_t index,
                        const std::array<std::uint8_t, 16>& guid,
                        std::uint32_t offset, std::uint32_t length,
                        std::uint32_t flags) {
    const auto base = 32 + index * 32;
    put_guid(table, base, guid);
    put_u32(table, base + 16, offset);
    put_u32(table, base + 20, length);
    put_u32(table, base + 24, flags);
}

ReaderImage make_image(
    std::uint64_t virtual_size = 8 * kMiB,
    std::uint32_t logical_sector = 512,
    std::uint64_t file_length = kDefaultFileLength,
    std::vector<test::FaultFileRule> rules = {},
    test::FaultFileObserver observer = {}) {
    auto fault = test::make_fault_file(
        file_length, std::move(rules), std::move(observer));
    ReaderImage image{
        std::move(fault.file),
        std::move(fault.state), std::nullopt, {}};

    std::array<std::byte, 8> identifier{};
    put_ascii(identifier, 0, "vhdxfile");
    REQUIRE(image.file->write_exact(0, identifier).ok());

    for (std::uint64_t copy = 0; copy < 2; ++copy) {
        std::array<std::byte, 4096> header{};
        put_ascii(header, 0, "head");
        put_u64(header, 8, 10 + copy);
        header[16] = static_cast<std::byte>(0x41 + copy);
        header[32] = static_cast<std::byte>(0x61 + copy);
        put_u16(header, 64, 0);
        put_u16(header, 66, 1);
        put_u32(header, 68, static_cast<std::uint32_t>(kMiB));
        put_u64(header, 72, 3 * kMiB);
        seal(header);
        REQUIRE(image.file->write_exact(
            (copy + 1) * 0x10000, header).ok());
    }

    std::array<std::byte, 65536> region{};
    put_ascii(region, 0, "regi");
    put_u32(region, 8, 2);
    put_region_entry(region, 0, kBatGuid, kMiB,
                     static_cast<std::uint32_t>(kMiB));
    put_region_entry(region, 1, kMetadataGuid, 2 * kMiB,
                     static_cast<std::uint32_t>(kMiB));
    seal(region);
    REQUIRE(image.file->write_exact(0x30000, region).ok());
    REQUIRE(image.file->write_exact(0x40000, region).ok());

    std::array<std::byte, 65536> metadata{};
    put_ascii(metadata, 0, "metadata");
    put_u16(metadata, 10, 5);
    put_metadata_entry(metadata, 0, kFileParametersGuid, 0x10000, 8, 4);
    put_metadata_entry(metadata, 1, kVirtualSizeGuid, 0x10008, 8, 6);
    put_metadata_entry(metadata, 2, kVirtualIdGuid, 0x10010, 16, 6);
    put_metadata_entry(metadata, 3, kLogicalSectorGuid, 0x10020, 4, 6);
    put_metadata_entry(metadata, 4, kPhysicalSectorGuid, 0x10024, 4, 6);
    REQUIRE(image.file->write_exact(2 * kMiB, metadata).ok());

    std::array<std::byte, 40> items{};
    put_u32(items, 0, kVhdxDataBlockBytes);
    put_u64(items, 8, virtual_size);
    items[16] = std::byte{0x7a};
    put_u32(items, 32, logical_sector);
    put_u32(items, 36, 4096);
    REQUIRE(image.file->write_exact(2 * kMiB + 0x10000, items).ok());

    const std::shared_ptr<IPersistenceFile> parse_view(
        image.file.get(), [](IPersistenceFile*) noexcept {});
    auto layout = parse_vhdx_layout(OperationContext{}, parse_view);
    REQUIRE(layout.ok());
    image.layout = layout.move_value();
    image.windows_identity.volume_serial_number = 19;
    image.windows_identity.file_id[0] = std::byte{0x51};
    image.windows_identity.size = file_length;
    image.windows_identity.attributes = 1;
    image.windows_identity.hard_link_count = 1;
    return image;
}

VhdxReaderOptions reader_options(
    std::uint64_t cache_entries = 4,
    std::uint64_t max_extents = 4096,
    std::uint64_t max_observed_payloads = 4096) {
    return {{cache_entries * kVhdxBatPageBytes, cache_entries},
            max_extents, max_observed_payloads};
}

std::unique_ptr<VhdxReader> open_reader(
    ReaderImage& image,
    VhdxReaderOptions options = reader_options()) {
    auto result = VhdxReader::create_for_testing(
        std::move(image.file), image.windows_identity,
        *image.layout, options);
    REQUIRE(result.ok());
    return result.move_value();
}

std::uint64_t payload_bat_index(
    const VhdxLayout& layout, std::uint64_t payload) {
    return payload + payload / layout.chunk_ratio();
}

void set_payload_entry(ReaderImage& image, std::uint64_t payload,
                       std::uint8_t state, std::uint64_t offset_mb = 0,
                       std::uint64_t reserved_bits = 0) {
    const auto raw = (offset_mb << 20U) | reserved_bits | state;
    std::array<std::byte, 8> bytes{};
    put_u64(bytes, 0, raw);
    REQUIRE(image.file->write_exact(
        image.layout->bat_offset() +
            payload_bat_index(*image.layout, payload) * 8,
        bytes).ok());
}

std::array<std::byte, kM2BlockSize> pattern(std::uint8_t seed) {
    std::array<std::byte, kM2BlockSize> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(
            (seed + index * 29U) & 0xffU);
    }
    return result;
}

void require_corrupt(const Result<BlockLoadResult>& result) {
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::BackingDataCorrupt));
}

TEST_CASE(vhdx_reader_resolves_all_dynamic_payload_states) {
    for (const auto state : {0U, 1U, 2U, 3U}) {
        auto image = make_image();
        set_payload_entry(
            image, 0, static_cast<std::uint8_t>(state),
            state == 1U || state == 3U ? 4U : 0U);
        auto reader = open_reader(image);
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0x6d});
        const auto before = destination;
        auto result = reader->read_block(
            OperationContext{}, 0, destination);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value().kind, BlockLoadKind::AllZero);
        REQUIRE(destination == before);
        REQUIRE_EQ(reader->stats().payload_read_calls, 0ULL);
    }

    auto image = make_image();
    set_payload_entry(image, 0, 6, 4);
    const auto expected = pattern(7);
    REQUIRE(image.file->write_exact(4 * kMiB, expected).ok());
    auto reader = open_reader(image);
    std::array<std::byte, kM2BlockSize> destination{};
    auto result = reader->read_block(OperationContext{}, 0, destination);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, BlockLoadKind::Data);
    REQUIRE(destination == expected);
}

TEST_CASE(vhdx_reader_rejects_reserved_partial_and_reserved_bits) {
    for (const auto state : {4U, 5U}) {
        auto image = make_image();
        set_payload_entry(image, 0, static_cast<std::uint8_t>(state));
        auto reader = open_reader(image);
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0x23});
        const auto before = destination;
        require_corrupt(reader->read_block(
            OperationContext{}, 0, destination));
        REQUIRE(destination == before);
    }
    auto partial = make_image();
    set_payload_entry(partial, 0, 7, 4);
    auto partial_reader = open_reader(partial);
    std::array<std::byte, kM2BlockSize> destination{};
    auto partial_result = partial_reader->read_block(
        OperationContext{}, 0, destination);
    require_corrupt(partial_result);

    auto reserved = make_image();
    set_payload_entry(reserved, 0, 6, 4, 1U << 3U);
    auto reserved_reader = open_reader(reserved);
    require_corrupt(reserved_reader->read_block(
        OperationContext{}, 0, destination));

    for (const auto state : {0U, 2U}) {
        auto offset_on_unallocated = make_image();
        set_payload_entry(
            offset_on_unallocated, 0,
            static_cast<std::uint8_t>(state), 4);
        auto offset_reader = open_reader(offset_on_unallocated);
        require_corrupt(offset_reader->read_block(
            OperationContext{}, 0, destination));
    }
}

TEST_CASE(vhdx_reader_slices_two_mib_payload_and_caches_one_bat_page) {
    auto image = make_image();
    set_payload_entry(image, 0, 6, 4);
    const auto first = pattern(11);
    const auto last = pattern(29);
    REQUIRE(image.file->write_exact(4 * kMiB, first).ok());
    REQUIRE(image.file->write_exact(
        4 * kMiB + 31 * kM2BlockSize, last).ok());
    auto reader = open_reader(image);
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(destination == first);
    REQUIRE(reader->read_block(
        OperationContext{}, 31, destination).ok());
    REQUIRE(destination == last);
    const auto stats = reader->stats();
    REQUIRE_EQ(stats.bat_read_calls, 1ULL);
    REQUIRE_EQ(stats.bat_cache_misses, 1ULL);
    REQUIRE_EQ(stats.bat_cache_hits, 1ULL);
    REQUIRE_EQ(stats.payload_read_calls, 2ULL);
}

TEST_CASE(vhdx_reader_lazy_io_uses_independent_exact_request_ranges) {
    auto zero_image = make_image();
    auto zero_reader = open_reader(zero_image);
    zero_image.state->clear_requests_for_test();
    std::array<std::byte, kM2BlockSize> destination{};
    auto zero = zero_reader->read_block(
        OperationContext{}, 0, destination);
    REQUIRE(zero.ok());
    REQUIRE_EQ(zero.value().kind, BlockLoadKind::AllZero);
    const auto zero_requests = zero_image.state->requests();
    REQUIRE(!zero_image.state->request_trace_overflowed());
    REQUIRE_EQ(zero_requests.size(), 1ULL);
    REQUIRE_EQ(zero_requests[0].offset,
               zero_image.layout->bat_offset());
    REQUIRE_EQ(zero_requests[0].length, kVhdxBatPageBytes);

    auto data_image = make_image();
    set_payload_entry(data_image, 0, 6, 4);
    const auto expected = pattern(0x2a);
    REQUIRE(data_image.file->write_exact(4 * kMiB, expected).ok());
    auto data_reader = open_reader(data_image);
    data_image.state->clear_requests_for_test();
    auto data = data_reader->read_block(
        OperationContext{}, 0, destination);
    REQUIRE(data.ok());
    REQUIRE(destination == expected);
    const auto data_requests = data_image.state->requests();
    REQUIRE(!data_image.state->request_trace_overflowed());
    REQUIRE_EQ(data_requests.size(), 2ULL);
    REQUIRE_EQ(data_requests[0].offset,
               data_image.layout->bat_offset());
    REQUIRE_EQ(data_requests[0].length, kVhdxBatPageBytes);
    REQUIRE_EQ(data_requests[1].offset, 4 * kMiB);
    REQUIRE_EQ(data_requests[1].length, kM2BlockSize);
}

TEST_CASE(vhdx_reader_zero_pads_final_partial_block_for_both_sector_sizes) {
    for (const auto sector : {512U, 4096U}) {
        auto image = make_image(2 * kMiB + sector, sector);
        set_payload_entry(image, 1, 6, 6);
        std::vector<std::byte> tail(sector);
        for (std::size_t index = 0; index < tail.size(); ++index) {
            tail[index] = static_cast<std::byte>((index * 7U) & 0xffU);
        }
        REQUIRE(image.file->write_exact(6 * kMiB, tail).ok());
        auto reader = open_reader(image);
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0x55});
        auto result = reader->read_block(
            OperationContext{}, 32, destination);
        REQUIRE(result.ok());
        REQUIRE(std::equal(tail.begin(), tail.end(), destination.begin()));
        REQUIRE(std::all_of(
            destination.begin() + tail.size(), destination.end(),
            [](std::byte value) { return value == std::byte{0}; }));
    }
}

TEST_CASE(vhdx_reader_rejects_payload_structure_overlap_bounds_and_overflow) {
    for (const auto offset_mb : {0ULL, 1ULL, 2ULL, 3ULL, 63ULL}) {
        auto image = make_image();
        set_payload_entry(image, 0, 6, offset_mb);
        auto reader = open_reader(image);
        std::array<std::byte, kM2BlockSize> destination{};
        require_corrupt(reader->read_block(
            OperationContext{}, 0, destination));
    }
    auto overflow = make_image();
    set_payload_entry(overflow, 0, 6, (1ULL << 44U) - 1);
    auto overflow_reader = open_reader(overflow);
    std::array<std::byte, kM2BlockSize> destination{};
    require_corrupt(overflow_reader->read_block(
        OperationContext{}, 0, destination));
}

TEST_CASE(vhdx_reader_detects_post_open_truncation_and_preserves_destination) {
    auto image = make_image();
    set_payload_entry(image, 0, 6, 4);
    auto reader = open_reader(image);
    image.state->force_length_for_test(5 * kMiB);
    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0x42});
    const auto before = destination;
    require_corrupt(reader->read_block(
        OperationContext{}, 0, destination));
    REQUIRE(destination == before);
}

TEST_CASE(vhdx_reader_extent_enumeration_is_sorted_sparse_and_payload_free) {
    auto image = make_image(6 * kMiB + 512);
    set_payload_entry(image, 0, 6, 4);
    set_payload_entry(image, 1, 2);
    set_payload_entry(image, 2, 6, 8);
    set_payload_entry(image, 3, 6, 10);
    auto reader = open_reader(image);
    auto extents = reader->enumerate_allocated_extents(OperationContext{});
    REQUIRE(extents.ok());
    const std::vector<VhdxAllocatedExtent> expected{
        {0, 2 * kMiB}, {4 * kMiB, 2 * kMiB},
        {6 * kMiB, 512}};
    REQUIRE(extents.value() == expected);
    REQUIRE_EQ(reader->stats().payload_read_calls, 0ULL);
}

TEST_CASE(vhdx_reader_extent_enumeration_rejects_physical_overlap_and_budget) {
    auto overlap = make_image();
    set_payload_entry(overlap, 0, 6, 4);
    set_payload_entry(overlap, 1, 6, 5);
    auto overlap_reader = open_reader(overlap);
    auto overlap_result = overlap_reader->enumerate_allocated_extents(
        OperationContext{});
    REQUIRE(!overlap_result.ok());
    REQUIRE_EQ(overlap_result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::BackingDataCorrupt));

    auto budget = make_image();
    set_payload_entry(budget, 0, 6, 4);
    set_payload_entry(budget, 1, 6, 6);
    auto budget_reader = open_reader(budget, reader_options(2, 1));
    auto budget_result = budget_reader->enumerate_allocated_extents(
        OperationContext{});
    REQUIRE(!budget_result.ok());
    REQUIRE_EQ(budget_result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::IoError));
}

TEST_CASE(vhdx_reader_normal_reads_latch_touched_physical_alias_corruption) {
    auto image = make_image();
    set_payload_entry(image, 0, 6, 4);
    set_payload_entry(image, 1, 6, 4);
    const auto expected = pattern(0x35);
    REQUIRE(image.file->write_exact(4 * kMiB, expected).ok());
    auto reader = open_reader(image);
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(destination == expected);
    destination.fill(std::byte{0x6a});
    const auto before = destination;
    require_corrupt(reader->read_block(
        OperationContext{}, 32, destination));
    REQUIRE(destination == before);
    require_corrupt(reader->read_block(
        OperationContext{}, 0, destination));
}

TEST_CASE(vhdx_reader_bat_cache_is_bounded_and_identity_isolated) {
    auto image = make_image(513 * 2 * kMiB, 512, 16 * kMiB);
    auto reader = open_reader(image, reader_options(1));
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(reader->read_block(
        OperationContext{}, 512 * 32ULL, destination).ok());
    auto stats = reader->stats();
    REQUIRE_EQ(stats.bat_read_calls, 2ULL);
    REQUIRE_EQ(stats.bat_cache_evictions, 1ULL);
    REQUIRE_EQ(stats.peak_cache_entries, 1ULL);
    REQUIRE_EQ(stats.peak_cache_bytes, kVhdxBatPageBytes);

    auto old_image = make_image();
    auto new_image = make_image();
    set_payload_entry(old_image, 0, 6, 4);
    set_payload_entry(new_image, 0, 6, 6);
    const auto old_data = pattern(3);
    const auto new_data = pattern(91);
    REQUIRE(old_image.file->write_exact(4 * kMiB, old_data).ok());
    REQUIRE(new_image.file->write_exact(6 * kMiB, new_data).ok());
    new_image.windows_identity.file_id[0] ^= std::byte{1};
    auto old_reader = open_reader(old_image);
    auto new_reader = open_reader(new_image);
    REQUIRE(old_reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(destination == old_data);
    REQUIRE(old_reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(destination == old_data);
    REQUIRE(new_reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE(destination == new_data);
}

TEST_CASE(vhdx_reader_bat_miss_io_does_not_hold_the_cache_mutex) {
    std::promise<void> stalled_promise;
    auto stalled = stalled_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    auto image = make_image(
        513 * 2 * kMiB, 512, 16 * kMiB, {},
        [&stalled_promise, release](
            test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 24) {
                stalled_promise.set_value();
                release.wait();
            }
        });
    auto reader = open_reader(image, reader_options(2));
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());

    auto miss = std::async(std::launch::async, [&] {
        std::array<std::byte, kM2BlockSize> block{};
        return reader->read_block(
            OperationContext{}, 512 * 32ULL, block);
    });
    REQUIRE_EQ(
        stalled.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);

    auto cached = std::async(std::launch::async, [&] {
        std::array<std::byte, kM2BlockSize> block{};
        return reader->read_block(OperationContext{}, 0, block);
    });
    auto stats = std::async(std::launch::async, [&] {
        return reader->stats();
    });
    auto clear = std::async(std::launch::async, [&] {
        reader->clear_cache();
    });
    const auto cached_status = cached.wait_for(std::chrono::seconds(1));
    const auto stats_status = stats.wait_for(std::chrono::seconds(1));
    const auto clear_status = clear.wait_for(std::chrono::seconds(1));
    release_promise.set_value();
    REQUIRE(miss.get().ok());
    REQUIRE_EQ(cached_status, std::future_status::ready);
    REQUIRE_EQ(stats_status, std::future_status::ready);
    REQUIRE_EQ(clear_status, std::future_status::ready);
    REQUIRE(cached.get().ok());
    (void)stats.get();
    clear.get();
    REQUIRE_EQ(reader->stats().current_cache_entries, 0ULL);
}

TEST_CASE(vhdx_reader_handles_interleaved_sector_bitmap_boundary) {
    constexpr std::uint64_t payload = 2048;
    auto image = make_image((payload + 1) * 2 * kMiB, 512, 16 * kMiB);
    set_payload_entry(image, payload, 6, 4);
    const auto expected = pattern(0x73);
    REQUIRE(image.file->write_exact(4 * kMiB, expected).ok());
    auto reader = open_reader(image, reader_options(4, 4));
    std::array<std::byte, kM2BlockSize> destination{};
    auto read = reader->read_block(
        OperationContext{}, payload * 32, destination);
    REQUIRE(read.ok());
    REQUIRE(destination == expected);
    auto extents = reader->enumerate_allocated_extents(OperationContext{});
    REQUIRE(extents.ok());
    REQUIRE_EQ(extents.value().size(), 1ULL);
    REQUIRE_EQ(extents.value()[0].virtual_offset, payload * 2 * kMiB);

    auto corrupt_image = make_image(
        (payload + 1) * 2 * kMiB, 512, 16 * kMiB);
    set_payload_entry(corrupt_image, payload, 6, 4);
    std::array<std::byte, 8> allocated_bitmap{};
    put_u64(allocated_bitmap, 0, (6ULL << 20U) | 6U);
    REQUIRE(corrupt_image.file->write_exact(
        corrupt_image.layout->bat_offset() + 2048 * 8,
        allocated_bitmap).ok());
    auto corrupt_reader = open_reader(
        corrupt_image, reader_options(4, 4));
    auto read_corrupt = corrupt_reader->read_block(
        OperationContext{}, payload * 32, destination);
    require_corrupt(read_corrupt);
    auto corrupt = corrupt_reader->enumerate_allocated_extents(
        OperationContext{});
    REQUIRE(!corrupt.ok());
    REQUIRE_EQ(corrupt.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::BackingDataCorrupt));
}

TEST_CASE(vhdx_reader_validates_4096_sector_bitmap_boundary_on_normal_read) {
    constexpr std::uint64_t payload = 16384;
    auto image = make_image(
        (payload + 1) * 2 * kMiB, 4096, 16 * kMiB);
    set_payload_entry(image, payload, 6, 4);
    std::array<std::byte, 8> allocated_bitmap{};
    put_u64(allocated_bitmap, 0, (6ULL << 20U) | 6U);
    REQUIRE(image.file->write_exact(
        image.layout->bat_offset() + payload * 8,
        allocated_bitmap).ok());
    auto reader = open_reader(image, reader_options(4, 4));
    std::array<std::byte, kM2BlockSize> destination{};
    require_corrupt(reader->read_block(
        OperationContext{}, payload * 32, destination));
}

TEST_CASE(vhdx_reader_create_rejects_layout_from_another_file) {
    auto first = make_image(8 * kMiB);
    auto second = make_image(10 * kMiB);
    auto result = VhdxReader::create_for_testing(
        std::move(second.file), second.windows_identity, *first.layout,
        reader_options());
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::BackingDataCorrupt));
}

TEST_CASE(vhdx_reader_propagates_bat_and_payload_io_faults) {
    auto bat_fault = make_image(
        8 * kMiB, 512, kDefaultFileLength,
        {{test::FaultFilePhase::Read, 23, std::nullopt,
          test::FaultFileAction::NativeError, 1117}});
    auto bat_reader = open_reader(bat_fault);
    std::array<std::byte, kM2BlockSize> destination{};
    auto bat_result = bat_reader->read_block(
        OperationContext{}, 0, destination);
    REQUIRE(!bat_result.ok());
    REQUIRE_EQ(bat_result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::IoError));
    REQUIRE_EQ(bat_result.error().native_code, 1117U);

    auto payload_fault = make_image(
        8 * kMiB, 512, kDefaultFileLength,
        {{test::FaultFilePhase::Read, 24, std::nullopt,
          test::FaultFileAction::NativeError, 1117}});
    set_payload_entry(payload_fault, 0, 6, 4);
    auto payload_reader = open_reader(payload_fault);
    destination.fill(std::byte{0x37});
    const auto before = destination;
    auto payload_result = payload_reader->read_block(
        OperationContext{}, 0, destination);
    REQUIRE(!payload_result.ok());
    REQUIRE_EQ(payload_result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::IoError));
    REQUIRE(destination == before);
}

TEST_CASE(vhdx_reader_translates_exact_io_fault_classes_and_length_failure) {
    struct BatFaultCase final {
        test::FaultFileAction action;
        std::uint32_t native_code;
        PersistenceErrorCode expected;
    };
    for (const auto fault : {
             BatFaultCase{test::FaultFileAction::NativeError, 5,
                          PersistenceErrorCode::AccessDenied},
             BatFaultCase{test::FaultFileAction::ShortRead, 0,
                          PersistenceErrorCode::BackingDataCorrupt},
             BatFaultCase{test::FaultFileAction::ZeroProgress, 0,
                          PersistenceErrorCode::BackingDataCorrupt}}) {
        auto image = make_image(
            8 * kMiB, 512, kDefaultFileLength,
            {{test::FaultFilePhase::Read, 23, std::nullopt,
              fault.action, fault.native_code}});
        auto reader = open_reader(image);
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0x67});
        const auto before = destination;
        auto result = reader->read_block(
            OperationContext{}, 0, destination);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
            fault.expected));
        REQUIRE(destination == before);
    }

    auto length_fault = make_image(
        8 * kMiB, 512, kDefaultFileLength,
        {{test::FaultFilePhase::Length, 4, std::nullopt,
          test::FaultFileAction::NativeError, 1117}});
    auto length_reader = open_reader(length_fault);
    std::array<std::byte, kM2BlockSize> destination{};
    auto length_result = length_reader->read_block(
        OperationContext{}, 0, destination);
    REQUIRE(!length_result.ok());
    REQUIRE_EQ(length_result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::IoError));
    REQUIRE_EQ(length_result.error().native_code, 1117U);
}

TEST_CASE(vhdx_reader_enforces_observed_payload_budget) {
    auto image = make_image();
    auto reader = open_reader(image, reader_options(2, 8, 1));
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());
    auto exhausted = reader->read_block(
        OperationContext{}, 32, destination);
    REQUIRE(!exhausted.ok());
    REQUIRE_EQ(exhausted.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::IoError));
}

TEST_CASE(vhdx_reader_propagates_post_io_cancel_and_deadline) {
    std::stop_source bat_stop;
    auto bat_cancelled = make_image(
        8 * kMiB, 512, kDefaultFileLength, {},
        [&bat_stop](test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 23) {
                bat_stop.request_stop();
            }
        });
    auto bat_cancelled_reader = open_reader(bat_cancelled);
    OperationContext bat_cancel_context{};
    bat_cancel_context.stop_token = bat_stop.get_token();
    std::array<std::byte, kM2BlockSize> bat_destination{};
    bat_destination.fill(std::byte{0x21});
    const auto bat_before = bat_destination;
    auto bat_cancel_result = bat_cancelled_reader->read_block(
        bat_cancel_context, 0, bat_destination);
    REQUIRE(!bat_cancel_result.ok());
    REQUIRE_EQ(bat_cancel_result.error().domain, ErrorDomain::storage);
    REQUIRE(bat_destination == bat_before);

    std::stop_source stop;
    auto cancelled = make_image(
        8 * kMiB, 512, kDefaultFileLength, {},
        [&stop](test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 24) {
                stop.request_stop();
            }
        });
    set_payload_entry(cancelled, 0, 6, 4);
    auto cancelled_reader = open_reader(cancelled);
    OperationContext cancel_context{};
    cancel_context.stop_token = stop.get_token();
    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0x44});
    const auto before = destination;
    auto cancel_result = cancelled_reader->read_block(
        cancel_context, 0, destination);
    REQUIRE(!cancel_result.ok());
    REQUIRE_EQ(cancel_result.error().domain, ErrorDomain::storage);
    REQUIRE(destination == before);

    OperationContext deadline{};
    bool payload_read_observed = false;
    auto expired = make_image(
        8 * kMiB, 512, kDefaultFileLength, {},
        [&deadline, &payload_read_observed](
            test::FaultFilePhase phase, std::uint64_t call) {
            if (phase == test::FaultFilePhase::Read && call == 24) {
                payload_read_observed = true;
                deadline.deadline =
                    std::chrono::steady_clock::time_point::min();
            }
        });
    set_payload_entry(expired, 0, 6, 4);
    auto expired_reader = open_reader(expired);
    auto deadline_result = expired_reader->read_block(
        deadline, 0, destination);
    REQUIRE(payload_read_observed);
    REQUIRE(!deadline_result.ok());
    REQUIRE_EQ(deadline_result.error().domain, ErrorDomain::storage);
    REQUIRE(destination == before);
}

TEST_CASE(vhdx_reader_random_sparse_map_matches_independent_oracle) {
    constexpr std::uint64_t payload_count = 256;
    auto image = make_image(
        payload_count * 2 * kMiB, 512, 512 * kMiB);
    std::uint64_t random = 0x22c0ffee19a7b11dULL;
    std::uint64_t next_physical_mb = 4;
    std::vector<VhdxAllocatedExtent> expected_extents;
    std::vector<std::uint8_t> states(payload_count);
    for (std::uint64_t payload = 0; payload < payload_count; ++payload) {
        random ^= random << 13U;
        random ^= random >> 7U;
        random ^= random << 17U;
        const auto allocated = (random & 3U) == 0;
        states[payload] = allocated ? 6 :
            static_cast<std::uint8_t>((random >> 8U) & 3U);
        if (allocated) {
            set_payload_entry(image, payload, 6, next_physical_mb);
            const auto data = pattern(static_cast<std::uint8_t>(payload));
            REQUIRE(image.file->write_exact(
                next_physical_mb * kMiB, data).ok());
            expected_extents.push_back({payload * 2 * kMiB, 2 * kMiB});
            next_physical_mb += 2;
        } else {
            set_payload_entry(image, payload, states[payload]);
        }
    }
    auto reader = open_reader(image, reader_options(4, payload_count));
    std::array<std::byte, kM2BlockSize> destination{};
    for (std::uint64_t payload = 0; payload < payload_count; ++payload) {
        destination.fill(std::byte{0xa5});
        const auto before = destination;
        auto result = reader->read_block(
            OperationContext{}, payload * 32, destination);
        REQUIRE(result.ok());
        if (states[payload] == 6) {
            REQUIRE_EQ(result.value().kind, BlockLoadKind::Data);
            REQUIRE(destination == pattern(static_cast<std::uint8_t>(payload)));
        } else {
            REQUIRE_EQ(result.value().kind, BlockLoadKind::AllZero);
            REQUIRE(destination == before);
        }
    }
    auto extents = reader->enumerate_allocated_extents(OperationContext{});
    REQUIRE(extents.ok());
    REQUIRE(extents.value() == expected_extents);
}

TEST_CASE(vhdx_reader_validates_arguments_cancel_and_post_io_cancel) {
    auto image = make_image();
    auto reader = open_reader(image);
    std::array<std::byte, kM2BlockSize> destination{};
    auto outside = reader->read_block(
        OperationContext{}, image.layout->payload_block_count() * 32,
        destination);
    REQUIRE(!outside.ok());
    REQUIRE_EQ(outside.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::InvalidFormat));
    auto short_destination = reader->read_block(
        OperationContext{}, 0, std::span(destination).first(4096));
    REQUIRE(!short_destination.ok());

    std::stop_source stop;
    stop.request_stop();
    OperationContext cancelled{};
    cancelled.stop_token = stop.get_token();
    auto cancel_result = reader->read_block(cancelled, 0, destination);
    REQUIRE(!cancel_result.ok());
    REQUIRE_EQ(cancel_result.error().domain, ErrorDomain::storage);
}

}  // namespace
}  // namespace myrd::persistence
