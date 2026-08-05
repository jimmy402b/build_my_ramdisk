#include "src/persistence/vhdx_format.h"

#include "src/common/crc32c.h"
#include "src/persistence/byte_codec.h"
#include "src/persistence/persistence_types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
constexpr std::uint64_t kHeaderSectionBytes = kMiB;
constexpr std::uint64_t kMaximumVirtualBytes = 64ull * 1024 * 1024 * 1024 * 1024;
constexpr std::size_t kHeaderBytes = 4096;
constexpr std::size_t kRegionTableBytes = 64 * 1024;
constexpr std::size_t kMetadataTableBytes = 64 * 1024;
constexpr std::uint64_t kHeader1Offset = 64 * kKiB;
constexpr std::uint64_t kHeader2Offset = 128 * kKiB;
constexpr std::uint64_t kRegion1Offset = 192 * kKiB;
constexpr std::uint64_t kRegion2Offset = 256 * kKiB;

using Guid = std::array<std::byte, 16>;

constexpr Guid bytes(std::array<std::uint8_t, 16> source) {
    Guid result{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        result[index] = static_cast<std::byte>(source[index]);
    }
    return result;
}

constexpr auto kBatGuid = bytes({
    0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
    0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08});
constexpr auto kMetadataGuid = bytes({
    0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
    0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e});
constexpr auto kFileParametersGuid = bytes({
    0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
    0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b});
constexpr auto kVirtualSizeGuid = bytes({
    0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
    0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8});
constexpr auto kVirtualIdGuid = bytes({
    0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
    0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46});
constexpr auto kLogicalSectorGuid = bytes({
    0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
    0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f});
constexpr auto kPhysicalSectorGuid = bytes({
    0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
    0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56});
constexpr auto kParentLocatorGuid = bytes({
    0x2d, 0x5f, 0xd3, 0xa8, 0x0b, 0xb3, 0x4d, 0x45,
    0xab, 0xf7, 0xd3, 0xd8, 0x48, 0x34, 0xab, 0x0c});

template <typename T>
Result<T> vhdx_failure(
    std::wstring_view message,
    PersistenceErrorCode code = PersistenceErrorCode::InvalidFormat,
    std::uint32_t native_code = 0) {
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        L"parse_vhdx_layout", std::wstring{message}, native_code});
}

bool checked_end(std::uint64_t offset, std::uint64_t length,
                 std::uint64_t& end) noexcept {
    if (length > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        return false;
    }
    end = offset + length;
    return true;
}

bool overlaps(std::uint64_t left_offset, std::uint64_t left_length,
              std::uint64_t right_offset, std::uint64_t right_length) noexcept {
    std::uint64_t left_end = 0;
    std::uint64_t right_end = 0;
    if (!checked_end(left_offset, left_length, left_end) ||
        !checked_end(right_offset, right_length, right_end)) {
        return true;
    }
    return left_offset < right_end && right_offset < left_end;
}

bool is_nil(const Guid& guid) noexcept {
    return std::ranges::all_of(guid, [](std::byte value) {
        return value == std::byte{0};
    });
}

bool signature(std::span<const std::byte> data, std::string_view expected) {
    if (data.size() < expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (data[index] != static_cast<std::byte>(expected[index])) {
            return false;
        }
    }
    return true;
}

std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    auto cursor = offset;
    (void)read_u32_le(bytes, cursor, value);
    return value;
}

std::uint64_t u64_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    auto cursor = offset;
    (void)read_u64_le(bytes, cursor, value);
    return value;
}

Guid guid_at(std::span<const std::byte> data, std::size_t offset) {
    Guid result{};
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
                result.size(), result.begin());
    return result;
}

template <std::size_t Size>
bool valid_checksum(const std::array<std::byte, Size>& bytes) {
    // Keep the largest (64 KiB) checksum scratch off the caller's bounded
    // Windows thread stack.  Parser reads remain fixed-size and never scale
    // with virtual disk capacity.
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    const auto expected = u32_at(copy, 4);
    std::fill_n(copy.begin() + 4, 4, std::byte{0});
    return crc32c(copy) == expected;
}

bool context_error(const Error& error) noexcept {
    if (error.domain != ErrorDomain::storage) return false;
    return error.code == static_cast<std::uint32_t>(
                             OperationErrorCode::cancelled) ||
           error.code == static_cast<std::uint32_t>(
                             OperationErrorCode::timed_out);
}

template <typename T>
Result<T> translate_read_error(const Error& error) {
    if (context_error(error)) return Result<T>::failure(error);
    if (error.native_code == ERROR_HANDLE_EOF) {
        return vhdx_failure<T>(L"VHDX structure is truncated");
    }
    const auto code = error.native_code == ERROR_ACCESS_DENIED
        ? PersistenceErrorCode::AccessDenied
        : PersistenceErrorCode::IoError;
    return vhdx_failure<T>(L"reading VHDX structure failed", code,
                           error.native_code);
}

template <std::size_t Size>
Result<std::array<std::byte, Size>> read_array(
    const OperationContext& context,
    const std::shared_ptr<IPersistenceFile>& file,
    std::uint64_t offset) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::array<std::byte, Size>>::failure(live.error());
    }
    std::array<std::byte, Size> result{};
    auto read = file->read_exact(context, offset, result);
    if (!read.ok()) {
        return translate_read_error<std::array<std::byte, Size>>(read.error());
    }
    live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::array<std::byte, Size>>::failure(live.error());
    }
    return Result<std::array<std::byte, Size>>::success(std::move(result));
}

struct ParsedHeader {
    std::uint64_t sequence{};
    VhdxFileIdentity identity{};
    Guid log_guid{};
    std::uint16_t log_version{};
    std::uint16_t version{};
    std::uint32_t log_length{};
    std::uint64_t log_offset{};
};

std::optional<ParsedHeader> decode_checksum_valid_header(
    const std::array<std::byte, kHeaderBytes>& bytes) {
    if (!signature(bytes, "head") || !valid_checksum(bytes)) {
        return std::nullopt;
    }
    ParsedHeader result{};
    result.sequence = u64_at(bytes, 8);
    result.identity.file_write_guid = guid_at(bytes, 16);
    result.identity.data_write_guid = guid_at(bytes, 32);
    result.log_guid = guid_at(bytes, 48);
    auto cursor = std::size_t{64};
    (void)read_u16_le(bytes, cursor, result.log_version);
    (void)read_u16_le(bytes, cursor, result.version);
    (void)read_u32_le(bytes, cursor, result.log_length);
    (void)read_u64_le(bytes, cursor, result.log_offset);
    return result;
}

Result<ParsedHeader> select_header(
    const std::array<std::byte, kHeaderBytes>& first_bytes,
    const std::array<std::byte, kHeaderBytes>& second_bytes,
    std::uint64_t file_length) {
    const auto first = decode_checksum_valid_header(first_bytes);
    const auto second = decode_checksum_valid_header(second_bytes);
    if (!first && !second) {
        return vhdx_failure<ParsedHeader>(L"both VHDX headers are invalid");
    }
    if (first && second && first->sequence == second->sequence) {
        return vhdx_failure<ParsedHeader>(
            L"valid VHDX headers have conflicting sequence numbers");
    }
    auto selected = first && (!second || first->sequence > second->sequence)
        ? *first : *second;
    const auto& selected_bytes = first &&
            (!second || first->sequence > second->sequence)
        ? first_bytes : second_bytes;
    if (require_zero(std::span(selected_bytes).subspan(80)) !=
        ByteCodecError::none) {
        return vhdx_failure<ParsedHeader>(
            L"current VHDX header has nonzero reserved bytes");
    }
    if (selected.version != 1 || selected.log_version != 0) {
        return vhdx_failure<ParsedHeader>(
            L"VHDX header version is outside the M3 profile",
            PersistenceErrorCode::UnsupportedVhdx);
    }
    if (!is_nil(selected.log_guid)) {
        return vhdx_failure<ParsedHeader>(
            L"active VHDX logs are outside the M3 profile",
            PersistenceErrorCode::UnsupportedVhdx);
    }
    if (is_nil(selected.identity.file_write_guid) ||
        is_nil(selected.identity.data_write_guid)) {
        return vhdx_failure<ParsedHeader>(L"VHDX identity GUID is nil");
    }
    std::uint64_t log_end = 0;
    if (selected.log_length == 0 || selected.log_length % kMiB != 0 ||
        selected.log_offset < kHeaderSectionBytes ||
        selected.log_offset % kMiB != 0 ||
        !checked_end(selected.log_offset, selected.log_length, log_end) ||
        log_end > file_length) {
        return vhdx_failure<ParsedHeader>(L"VHDX log range is invalid");
    }
    return Result<ParsedHeader>::success(std::move(selected));
}

struct Region {
    std::uint64_t offset{};
    std::uint64_t length{};
};

struct Regions {
    Region bat{};
    Region metadata{};
};

bool checksum_valid_region(
    const std::array<std::byte, kRegionTableBytes>& bytes) {
    return signature(bytes, "regi") && valid_checksum(bytes);
}

Result<Regions> decode_regions(
    const std::array<std::byte, kRegionTableBytes>& bytes,
    const ParsedHeader& header,
    std::uint64_t file_length) {
    const auto count = u32_at(bytes, 8);
    if (count > 2047 || u32_at(bytes, 12) != 0) {
        return vhdx_failure<Regions>(L"VHDX region table header is invalid");
    }
    Regions result{};
    bool have_bat = false;
    bool have_metadata = false;
    std::vector<Guid> seen;
    seen.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto base = 16 + static_cast<std::size_t>(index) * 32;
        const auto id = guid_at(bytes, base);
        if (std::ranges::find(seen, id) != seen.end()) {
            return vhdx_failure<Regions>(L"VHDX region GUID is duplicated");
        }
        seen.push_back(id);
        const auto offset = u64_at(bytes, base + 16);
        const auto length = static_cast<std::uint64_t>(u32_at(bytes, base + 24));
        const auto required = u32_at(bytes, base + 28);
        if (required > 1 || offset < kMiB || offset % kMiB != 0 ||
            length == 0 || length % kMiB != 0) {
            return vhdx_failure<Regions>(L"VHDX region range is invalid");
        }
        std::uint64_t end = 0;
        if (!checked_end(offset, length, end) || end > file_length) {
            return vhdx_failure<Regions>(L"VHDX region exceeds file bounds");
        }
        if (id == kBatGuid) {
            if (required != 1) {
                return vhdx_failure<Regions>(L"BAT region is not required");
            }
            result.bat = {offset, length};
            have_bat = true;
        } else if (id == kMetadataGuid) {
            if (required != 1) {
                return vhdx_failure<Regions>(L"metadata region is not required");
            }
            result.metadata = {offset, length};
            have_metadata = true;
        } else {
            return vhdx_failure<Regions>(
                L"unknown VHDX region is outside the M3 profile",
                PersistenceErrorCode::UnsupportedVhdx);
        }
    }
    if (!have_bat || !have_metadata) {
        return vhdx_failure<Regions>(L"required VHDX region is missing");
    }
    if (overlaps(result.bat.offset, result.bat.length,
                 result.metadata.offset, result.metadata.length) ||
        overlaps(result.bat.offset, result.bat.length,
                 header.log_offset, header.log_length) ||
        overlaps(result.metadata.offset, result.metadata.length,
                 header.log_offset, header.log_length)) {
        return vhdx_failure<Regions>(L"VHDX regions overlap");
    }
    return Result<Regions>::success(result);
}

Result<Regions> select_regions(
    const std::array<std::byte, kRegionTableBytes>& first,
    const std::array<std::byte, kRegionTableBytes>& second,
    const ParsedHeader& header,
    std::uint64_t file_length) {
    const auto first_valid = checksum_valid_region(first);
    const auto second_valid = checksum_valid_region(second);
    if (!first_valid && !second_valid) {
        return vhdx_failure<Regions>(L"both VHDX region tables are invalid");
    }
    if (first_valid && second_valid && first != second) {
        return vhdx_failure<Regions>(L"valid VHDX region tables conflict");
    }
    return decode_regions(first_valid ? first : second, header, file_length);
}

enum class MetadataKind : std::uint8_t {
    FileParameters,
    VirtualSize,
    VirtualId,
    LogicalSector,
    PhysicalSector,
};

struct MetadataEntry {
    MetadataKind kind{};
    std::uint32_t offset{};
    std::uint32_t length{};
};

struct ParsedMetadata {
    std::uint64_t virtual_size{};
    std::uint32_t block_size{};
    std::uint32_t file_flags{};
    std::uint32_t logical_sector{};
    std::uint32_t physical_sector{};
    Guid virtual_id{};
};

std::optional<MetadataKind> metadata_kind(const Guid& id) {
    if (id == kFileParametersGuid) return MetadataKind::FileParameters;
    if (id == kVirtualSizeGuid) return MetadataKind::VirtualSize;
    if (id == kVirtualIdGuid) return MetadataKind::VirtualId;
    if (id == kLogicalSectorGuid) return MetadataKind::LogicalSector;
    if (id == kPhysicalSectorGuid) return MetadataKind::PhysicalSector;
    return std::nullopt;
}

std::uint32_t expected_length(MetadataKind kind) {
    switch (kind) {
    case MetadataKind::FileParameters: return 8;
    case MetadataKind::VirtualSize: return 8;
    case MetadataKind::VirtualId: return 16;
    case MetadataKind::LogicalSector: return 4;
    case MetadataKind::PhysicalSector: return 4;
    }
    return 0;
}

std::uint32_t expected_flags(MetadataKind kind) {
    return kind == MetadataKind::FileParameters ? 4U : 6U;
}

Result<std::vector<MetadataEntry>> decode_metadata_table(
    const std::array<std::byte, kMetadataTableBytes>& table,
    const Region& metadata) {
    if (!signature(table, "metadata") ||
        require_zero(std::span(table).subspan(8, 2)) != ByteCodecError::none ||
        require_zero(std::span(table).subspan(12, 20)) != ByteCodecError::none) {
        return vhdx_failure<std::vector<MetadataEntry>>(
            L"VHDX metadata table header is invalid");
    }
    const auto count = static_cast<std::uint32_t>(
        std::to_integer<std::uint8_t>(table[10]) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(table[11])) << 8U));
    if (count > 2047 || count != 5) {
        return vhdx_failure<std::vector<MetadataEntry>>(
            L"M3 VHDX metadata set is incomplete");
    }
    std::vector<MetadataEntry> entries;
    entries.reserve(count);
    std::vector<Guid> seen;
    seen.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto base = 32 + static_cast<std::size_t>(index) * 32;
        const auto id = guid_at(table, base);
        const auto offset = u32_at(table, base + 16);
        const auto length = u32_at(table, base + 20);
        const auto flags = u32_at(table, base + 24);
        const auto reserved = u32_at(table, base + 28);
        if (std::ranges::find(seen, id) != seen.end()) {
            return vhdx_failure<std::vector<MetadataEntry>>(
                L"VHDX metadata GUID is duplicated");
        }
        seen.push_back(id);
        if (id == kParentLocatorGuid) {
            return vhdx_failure<std::vector<MetadataEntry>>(
                L"parent locators are outside the M3 profile",
                PersistenceErrorCode::UnsupportedVhdx);
        }
        const auto kind = metadata_kind(id);
        if (!kind) {
            return vhdx_failure<std::vector<MetadataEntry>>(
                L"unknown VHDX metadata is outside the M3 profile",
                PersistenceErrorCode::UnsupportedVhdx);
        }
        if (flags != expected_flags(*kind) || reserved != 0 ||
            length != expected_length(*kind) || offset < kMetadataTableBytes) {
            return vhdx_failure<std::vector<MetadataEntry>>(
                L"VHDX metadata entry is invalid");
        }
        std::uint64_t item_end = 0;
        if (!checked_end(offset, length, item_end) ||
            item_end > metadata.length) {
            return vhdx_failure<std::vector<MetadataEntry>>(
                L"VHDX metadata item exceeds its region");
        }
        for (const auto& existing : entries) {
            if (overlaps(offset, length, existing.offset, existing.length)) {
                return vhdx_failure<std::vector<MetadataEntry>>(
                    L"VHDX metadata items overlap");
            }
        }
        entries.push_back({*kind, offset, length});
    }
    return Result<std::vector<MetadataEntry>>::success(std::move(entries));
}

Result<ParsedMetadata> read_metadata(
    const OperationContext& context,
    const std::shared_ptr<IPersistenceFile>& file,
    const Region& region) {
    if (region.length < kMetadataTableBytes) {
        return vhdx_failure<ParsedMetadata>(L"metadata region is too small");
    }
    auto table = read_array<kMetadataTableBytes>(context, file, region.offset);
    if (!table.ok()) return Result<ParsedMetadata>::failure(table.error());
    auto entries = decode_metadata_table(table.value(), region);
    if (!entries.ok()) return Result<ParsedMetadata>::failure(entries.error());

    ParsedMetadata result{};
    for (const auto& entry : entries.value()) {
        std::array<std::byte, 16> value{};
        auto read = file->read_exact(
            context, region.offset + entry.offset,
            std::span(value).first(entry.length));
        if (!read.ok()) return translate_read_error<ParsedMetadata>(read.error());
        auto live = check_operation_context(context);
        if (!live.ok()) return Result<ParsedMetadata>::failure(live.error());
        switch (entry.kind) {
        case MetadataKind::FileParameters:
            result.block_size = u32_at(value, 0);
            result.file_flags = u32_at(value, 4);
            break;
        case MetadataKind::VirtualSize:
            result.virtual_size = u64_at(value, 0);
            break;
        case MetadataKind::VirtualId:
            result.virtual_id = guid_at(value, 0);
            break;
        case MetadataKind::LogicalSector:
            result.logical_sector = u32_at(value, 0);
            break;
        case MetadataKind::PhysicalSector:
            result.physical_sector = u32_at(value, 0);
            break;
        }
    }
    if ((result.file_flags & 0x3U) != 0) {
        return vhdx_failure<ParsedMetadata>(
            L"fixed or differencing VHDX is outside the M3 profile",
            PersistenceErrorCode::UnsupportedVhdx);
    }
    if ((result.file_flags & ~0x3U) != 0) {
        return vhdx_failure<ParsedMetadata>(
            L"VHDX file parameters contain reserved flags");
    }
    if (result.block_size != kVhdxDataBlockBytes ||
        (result.logical_sector != kLogicalSectorSize512 &&
         result.logical_sector != kLogicalSectorSize4096) ||
        result.physical_sector != kPhysicalSectorSize) {
        return vhdx_failure<ParsedMetadata>(
            L"VHDX geometry is outside the M3 profile",
            PersistenceErrorCode::UnsupportedVhdx);
    }
    if (result.virtual_size == 0 ||
        result.virtual_size > kMaximumVirtualBytes ||
        result.virtual_size % result.logical_sector != 0 ||
        is_nil(result.virtual_id)) {
        return vhdx_failure<ParsedMetadata>(L"VHDX virtual geometry is invalid");
    }
    return Result<ParsedMetadata>::success(result);
}

}  // namespace

std::uint64_t VhdxLayout::virtual_size() const noexcept { return virtual_size_; }
std::uint32_t VhdxLayout::logical_sector_size() const noexcept {
    return logical_sector_size_;
}
std::uint32_t VhdxLayout::physical_sector_size() const noexcept {
    return physical_sector_size_;
}
std::uint32_t VhdxLayout::block_size() const noexcept { return block_size_; }
std::uint64_t VhdxLayout::payload_block_count() const noexcept {
    return payload_block_count_;
}
std::uint64_t VhdxLayout::chunk_ratio() const noexcept { return chunk_ratio_; }
std::uint64_t VhdxLayout::bat_entry_count() const noexcept {
    return bat_entry_count_;
}
std::uint64_t VhdxLayout::bat_offset() const noexcept { return bat_offset_; }
std::uint64_t VhdxLayout::bat_length() const noexcept { return bat_length_; }
std::uint64_t VhdxLayout::metadata_offset() const noexcept {
    return metadata_offset_;
}
std::uint64_t VhdxLayout::metadata_length() const noexcept {
    return metadata_length_;
}
std::uint64_t VhdxLayout::log_offset() const noexcept { return log_offset_; }
std::uint64_t VhdxLayout::log_length() const noexcept { return log_length_; }
std::uint64_t VhdxLayout::file_length() const noexcept { return file_length_; }
const VhdxFileIdentity& VhdxLayout::file_identity() const noexcept {
    return file_identity_;
}
const std::array<std::byte, 16>& VhdxLayout::virtual_disk_id() const noexcept {
    return virtual_disk_id_;
}
Result<VhdxLayout> parse_vhdx_layout(
    const OperationContext& context,
    std::shared_ptr<IPersistenceFile> file) {
    try {
    if (!file) return vhdx_failure<VhdxLayout>(L"VHDX file is null");
    auto live = check_operation_context(context);
    if (!live.ok()) return Result<VhdxLayout>::failure(live.error());
    auto length = file->length(context);
    if (!length.ok()) return translate_read_error<VhdxLayout>(length.error());
    if (length.value() < kHeaderSectionBytes) {
        return vhdx_failure<VhdxLayout>(L"VHDX header section is truncated");
    }
    auto identifier = read_array<8>(context, file, 0);
    if (!identifier.ok()) return Result<VhdxLayout>::failure(identifier.error());
    if (!signature(identifier.value(), "vhdxfile")) {
        return vhdx_failure<VhdxLayout>(L"VHDX identifier is invalid");
    }
    auto first_header = read_array<kHeaderBytes>(context, file, kHeader1Offset);
    if (!first_header.ok()) return Result<VhdxLayout>::failure(first_header.error());
    auto second_header = read_array<kHeaderBytes>(context, file, kHeader2Offset);
    if (!second_header.ok()) return Result<VhdxLayout>::failure(second_header.error());
    auto header = select_header(
        first_header.value(), second_header.value(), length.value());
    if (!header.ok()) return Result<VhdxLayout>::failure(header.error());

    auto first_region = read_array<kRegionTableBytes>(
        context, file, kRegion1Offset);
    if (!first_region.ok()) return Result<VhdxLayout>::failure(first_region.error());
    auto second_region = read_array<kRegionTableBytes>(
        context, file, kRegion2Offset);
    if (!second_region.ok()) return Result<VhdxLayout>::failure(second_region.error());
    auto regions = select_regions(
        first_region.value(), second_region.value(), header.value(),
        length.value());
    if (!regions.ok()) return Result<VhdxLayout>::failure(regions.error());

    auto metadata = read_metadata(
        context, file, regions.value().metadata);
    if (!metadata.ok()) return Result<VhdxLayout>::failure(metadata.error());

    const auto payload_blocks =
        (metadata.value().virtual_size - 1) / metadata.value().block_size + 1;
    const auto chunk_bytes =
        (std::uint64_t{1} << 23U) * metadata.value().logical_sector;
    const auto chunk_ratio = chunk_bytes / metadata.value().block_size;
    if (chunk_ratio == 0) {
        return vhdx_failure<VhdxLayout>(L"VHDX chunk ratio is invalid");
    }
    const auto sector_bitmap_entries_before_last_payload =
        (payload_blocks - 1) / chunk_ratio;
    if (sector_bitmap_entries_before_last_payload >
        (std::numeric_limits<std::uint64_t>::max)() - payload_blocks) {
        return vhdx_failure<VhdxLayout>(L"VHDX BAT count overflows");
    }
    const auto bat_entries =
        payload_blocks + sector_bitmap_entries_before_last_payload;
    if (bat_entries > regions.value().bat.length / sizeof(std::uint64_t)) {
        return vhdx_failure<VhdxLayout>(L"VHDX BAT region is too small");
    }

    VhdxLayout result{};
    result.file_identity_ = header.value().identity;
    result.virtual_disk_id_ = metadata.value().virtual_id;
    result.virtual_size_ = metadata.value().virtual_size;
    result.logical_sector_size_ = metadata.value().logical_sector;
    result.physical_sector_size_ = metadata.value().physical_sector;
    result.block_size_ = metadata.value().block_size;
    result.payload_block_count_ = payload_blocks;
    result.chunk_ratio_ = chunk_ratio;
    result.bat_entry_count_ = bat_entries;
    result.bat_offset_ = regions.value().bat.offset;
    result.bat_length_ = regions.value().bat.length;
    result.metadata_offset_ = regions.value().metadata.offset;
    result.metadata_length_ = regions.value().metadata.length;
    result.log_offset_ = header.value().log_offset;
    result.log_length_ = header.value().log_length;
    result.file_length_ = length.value();
    return Result<VhdxLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return vhdx_failure<VhdxLayout>(
            L"allocating bounded VHDX parser state failed",
            PersistenceErrorCode::IoError);
    }
}

}  // namespace myrd::persistence
