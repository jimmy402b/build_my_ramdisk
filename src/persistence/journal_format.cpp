#include "src/persistence/journal_format.h"

#include "src/common/crc32c.h"
#include "src/common/sha256_stream.h"
#include "src/persistence/byte_codec.h"

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace myrd::persistence {
namespace {

constexpr std::size_t kMajorOffset = 8;
constexpr std::size_t kMinorOffset = 12;
constexpr std::size_t kHeaderLengthOffset = 16;
constexpr std::size_t kHeaderCrcOffset = 20;
constexpr std::size_t kImageUuidOffset = 24;
constexpr std::size_t kJournalUuidOffset = 40;
constexpr std::size_t kEpochOffset = 56;
constexpr std::size_t kVirtualBytesOffset = 64;
constexpr std::size_t kLogicalSectorOffset = 72;
constexpr std::size_t kPhysicalSectorOffset = 76;
constexpr std::size_t kM2BlockSizeOffset = 80;
constexpr std::size_t kVhdxBlockSizeOffset = 84;
constexpr std::size_t kBaseGenerationOffset = 88;
constexpr std::size_t kPredecessorKindOffset = 96;
constexpr std::size_t kPredecessorEpochOffset = 104;
constexpr std::size_t kPredecessorUuidOffset = 112;
constexpr std::size_t kPredecessorDigestOffset = 128;
constexpr std::size_t kGeometryDigestOffset = 160;
constexpr std::size_t kBaseIdentityDigestOffset = 192;
constexpr std::size_t kFeatureFlagsOffset = 224;
constexpr std::size_t kSemanticBytes = 232;

constexpr std::size_t kRecordHeaderLengthOffset = 16;
constexpr std::size_t kRecordTypeOffset = 20;
constexpr std::size_t kRecordLengthOffset = 24;
constexpr std::size_t kRecordCrcOffset = 32;
constexpr std::size_t kRecordGenerationOffset = 40;
constexpr std::size_t kRecordOperationIdOffset = 48;
constexpr std::size_t kRecordSequenceOffset = 56;
constexpr std::size_t kRecordFlagsOffset = 64;
constexpr std::size_t kBlockIndexOffset = 72;
constexpr std::size_t kSnapshotVersionOffset = 80;
constexpr std::size_t kSourceGenerationOffset = 88;
constexpr std::size_t kDirtyEpochOffset = 96;
constexpr std::size_t kLogicalPayloadLengthOffset = 104;
constexpr std::size_t kStoredPayloadLengthOffset = 108;
constexpr std::size_t kPayloadCrcOffset = 112;
constexpr std::size_t kBlockSemanticBytes = 116;

constexpr std::size_t kIndexLevelOffset = 72;
constexpr std::size_t kIndexEntryCountOffset = 74;
constexpr std::size_t kIndexEntryKindOffset = 76;
constexpr std::size_t kIndexEntryWidthOffset = 78;
constexpr std::size_t kIndexMinimumKeyOffset = 80;
constexpr std::size_t kIndexMaximumKeyOffset = 88;
constexpr std::size_t kIndexNodeGenerationOffset = 96;
constexpr std::size_t kIndexEntriesOffset = 128;

constexpr std::size_t kCommitImageUuidOffset = 72;
constexpr std::size_t kCommitJournalUuidOffset = 88;
constexpr std::size_t kCommitJournalEpochOffset = 104;
constexpr std::size_t kCommitPreviousGenerationOffset = 112;
constexpr std::size_t kCommitNewGenerationOffset = 120;
constexpr std::size_t kCommitPreviousDigestOffset = 128;
constexpr std::size_t kCommitNewDigestOffset = 160;
constexpr std::size_t kCommitDataCountOffset = 192;
constexpr std::size_t kCommitZeroCountOffset = 200;
constexpr std::size_t kCommitDiscardCountOffset = 208;
constexpr std::size_t kCommitIndexCountOffset = 216;
constexpr std::size_t kCommitRootOffsetOffset = 224;
constexpr std::size_t kCommitRootLengthOffset = 232;
constexpr std::size_t kCommitRootCrcOffset = 240;
constexpr std::size_t kCommitBeginOffset = 248;
constexpr std::size_t kCommitEndOffset = 256;
constexpr std::size_t kCommitGeometryDigestOffset = 264;
constexpr std::size_t kCommitInputRecordCountOffset = 296;
constexpr std::size_t kCommitSemanticBytes = 304;

template <typename T>
Result<T> journal_failure(std::wstring_view operation,
                          std::wstring_view message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
        std::wstring{operation}, std::wstring{message}, 0});
}

Result<std::uint32_t> compute_record_crc(
    const std::span<const std::byte> bytes) {
    static constexpr std::array<std::byte, sizeof(std::uint32_t)>
        kZeroCrcField{};
    if (bytes.size() < kRecordCrcOffset + kZeroCrcField.size()) {
        return journal_failure<std::uint32_t>(
            L"compute_record_crc", L"record CRC field is truncated");
    }

    Crc32cStream stream;
    auto update = stream.update(bytes.first(kRecordCrcOffset));
    if (!update.ok()) {
        return Result<std::uint32_t>::failure(update.error());
    }
    update = stream.update(kZeroCrcField);
    if (!update.ok()) {
        return Result<std::uint32_t>::failure(update.error());
    }
    update = stream.update(bytes.subspan(
        kRecordCrcOffset + kZeroCrcField.size()));
    if (!update.ok()) {
        return Result<std::uint32_t>::failure(update.error());
    }
    return stream.finalize();
}

bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

template <typename Integer>
bool write_integer(JournalHeaderBytes& bytes, std::size_t offset,
                   Integer value) noexcept {
    auto local = offset;
    if constexpr (sizeof(Integer) == 4) {
        return write_u32_le(bytes, local, value) == ByteCodecError::none;
    } else {
        return write_u64_le(bytes, local, value) == ByteCodecError::none;
    }
}

template <typename Integer>
bool read_integer(std::span<const std::byte> bytes, std::size_t offset,
                  Integer& value) noexcept {
    auto local = offset;
    if constexpr (sizeof(Integer) == 4) {
        return read_u32_le(bytes, local, value) == ByteCodecError::none;
    } else {
        return read_u64_le(bytes, local, value) == ByteCodecError::none;
    }
}

bool copy_to(JournalHeaderBytes& bytes, std::size_t offset,
             std::span<const std::byte> source) noexcept {
    auto local = offset;
    return write_bytes(bytes, local, source) == ByteCodecError::none;
}

bool copy_from(std::span<const std::byte> bytes, std::size_t offset,
               std::span<std::byte> destination) noexcept {
    auto local = offset;
    return read_bytes(bytes, local, destination) == ByteCodecError::none;
}

template <typename Integer>
bool write_record_integer(std::span<std::byte> bytes, std::size_t offset,
                          Integer value) noexcept {
    auto local = offset;
    if constexpr (sizeof(Integer) == 2) {
        return write_u16_le(bytes, local, value) == ByteCodecError::none;
    } else if constexpr (sizeof(Integer) == 4) {
        return write_u32_le(bytes, local, value) == ByteCodecError::none;
    } else {
        return write_u64_le(bytes, local, value) == ByteCodecError::none;
    }
}

template <typename Integer>
bool read_record_integer(std::span<const std::byte> bytes,
                         std::size_t offset, Integer& value) noexcept {
    auto local = offset;
    if constexpr (sizeof(Integer) == 2) {
        return read_u16_le(bytes, local, value) == ByteCodecError::none;
    } else if constexpr (sizeof(Integer) == 4) {
        return read_u32_le(bytes, local, value) == ByteCodecError::none;
    } else {
        return read_u64_le(bytes, local, value) == ByteCodecError::none;
    }
}

bool block_type(JournalRecordType type) noexcept {
    return type == JournalRecordType::Data ||
           type == JournalRecordType::Zero ||
           type == JournalRecordType::Discard;
}

std::uint64_t physical_length(JournalRecordType type) noexcept {
    return type == JournalRecordType::Data
        ? kDataRecordBytes
        : kFixedRecordBytes;
}

Result<void> validate_block_metadata(const BlockJournalRecord& record,
                                     const ImageGeometry& geometry,
                                     std::wstring_view operation) {
    if (!block_type(record.common.type)) {
        return journal_failure<void>(operation,
                                    L"record type is not a block record");
    }
    if (record.common.checkpoint_generation == 0 ||
        record.common.checkpoint_generation == kInvalidGeneration ||
        record.common.operation_id == 0 ||
        (record.common.flags & kJournalMandatoryRecordFlags) != 0 ||
        record.snapshot_version == 0 || record.dirty_epoch == 0 ||
        record.source_generation == kInvalidGeneration ||
        record.source_generation >=
            record.common.checkpoint_generation) {
        return journal_failure<void>(operation,
                                    L"block record identity is invalid");
    }
    const auto logical = journal_block_logical_length(
        geometry, record.block_index);
    if (!logical.ok()) return Result<void>::failure(logical.error());
    return Result<void>::success();
}

Result<void> validate_record_location(
    std::uint64_t record_offset, std::uint64_t record_length,
    std::uint64_t committed_bound, std::wstring_view operation) {
    if (record_offset < kJournalAlignment ||
        record_offset % kJournalAlignment != 0 ||
        record_offset > committed_bound ||
        record_length > committed_bound - record_offset) {
        return journal_failure<void>(operation,
                                    L"record range exceeds committed bound");
    }
    return Result<void>::success();
}

Result<void> write_common_record(std::span<std::byte> output,
                                 const JournalRecordCommon& common,
                                 std::uint64_t record_length,
                                 std::wstring_view operation) {
    auto cursor = std::size_t{0};
    const bool ok = write_bytes(output, cursor, kJournalRecordMagic) ==
                        ByteCodecError::none &&
                    write_record_integer(output, kMajorOffset,
                                         kM3FormatMajor) &&
                    write_record_integer(output, kMinorOffset,
                                         kM3FormatMinor) &&
                    write_record_integer(
                        output, kRecordHeaderLengthOffset,
                        static_cast<std::uint32_t>(kJournalAlignment)) &&
                    write_record_integer(
                        output, kRecordTypeOffset,
                        static_cast<std::uint16_t>(common.type)) &&
                    write_record_integer(output, kRecordLengthOffset,
                                         record_length) &&
                    write_record_integer(
                        output, kRecordGenerationOffset,
                        common.checkpoint_generation) &&
                    write_record_integer(output, kRecordOperationIdOffset,
                                         common.operation_id) &&
                    write_record_integer(output, kRecordSequenceOffset,
                                         common.sequence) &&
                    write_record_integer(output, kRecordFlagsOffset,
                                         common.flags);
    if (!ok) {
        return journal_failure<void>(operation,
                                     L"common record layout overflow");
    }
    return Result<void>::success();
}

struct ParsedCommonRecord final {
    JournalRecordCommon common{};
    std::uint64_t record_length{0};
    std::uint32_t record_crc32c{0};
};

Result<ParsedCommonRecord> parse_common_record(
    const std::span<const std::byte> input,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound,
    const JournalRecordType expected_type,
    const std::uint64_t expected_length,
    std::wstring_view operation) {
    if (input.size() < kRecordFlagsOffset + sizeof(std::uint64_t)) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"common record header is truncated");
    }
    if (!std::equal(kJournalRecordMagic.begin(), kJournalRecordMagic.end(),
                    input.begin())) {
        return journal_failure<ParsedCommonRecord>(operation,
                                                    L"record magic mismatch");
    }

    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t header_length = 0;
    std::uint16_t type_wire = 0;
    ParsedCommonRecord parsed{};
    if (!read_record_integer(input, kMajorOffset, major) ||
        !read_record_integer(input, kMinorOffset, minor) ||
        !read_record_integer(input, kRecordHeaderLengthOffset,
                             header_length) ||
        !read_record_integer(input, kRecordTypeOffset, type_wire) ||
        !read_record_integer(input, kRecordLengthOffset,
                             parsed.record_length) ||
        !read_record_integer(input, kRecordCrcOffset,
                             parsed.record_crc32c) ||
        !read_record_integer(input, kRecordGenerationOffset,
                             parsed.common.checkpoint_generation) ||
        !read_record_integer(input, kRecordOperationIdOffset,
                             parsed.common.operation_id) ||
        !read_record_integer(input, kRecordSequenceOffset,
                             parsed.common.sequence) ||
        !read_record_integer(input, kRecordFlagsOffset,
                             parsed.common.flags)) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"common record fields are truncated");
    }
    (void)minor;
    if (major != kM3FormatMajor || header_length != kJournalAlignment) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"unsupported record version");
    }
    if (type_wire != static_cast<std::uint16_t>(expected_type)) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"unexpected record type");
    }
    parsed.common.type = expected_type;
    if (parsed.record_length != expected_length ||
        input.size() != expected_length) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"record physical length mismatch");
    }
    const auto location = validate_record_location(
        record_offset, parsed.record_length, committed_bound, operation);
    if (!location.ok()) {
        return Result<ParsedCommonRecord>::failure(location.error());
    }
    if (parsed.common.operation_id == 0 ||
        (parsed.common.flags & kJournalMandatoryRecordFlags) != 0) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"record identity or mandatory flags are invalid");
    }
    const auto computed_crc = compute_record_crc(input);
    if (!computed_crc.ok()) {
        return Result<ParsedCommonRecord>::failure(computed_crc.error());
    }
    if (computed_crc.value() != parsed.record_crc32c) {
        return journal_failure<ParsedCommonRecord>(operation,
                                                    L"record CRC32C mismatch");
    }
    if (require_zero(input.subspan(22, 2)) != ByteCodecError::none ||
        require_zero(input.subspan(36, 4)) != ByteCodecError::none) {
        return journal_failure<ParsedCommonRecord>(
            operation, L"common record reserved bytes are nonzero");
    }
    return Result<ParsedCommonRecord>::success(parsed);
}

Result<void> validate_descriptor_range(
    const std::uint64_t offset, const std::uint64_t length,
    const std::uint64_t expected_length,
    const std::uint64_t parent_offset,
    const std::uint64_t committed_bound,
    std::wstring_view operation) {
    if (offset < kJournalAlignment ||
        offset % kJournalAlignment != 0 ||
        length != expected_length || offset > parent_offset ||
        length > parent_offset - offset || offset > committed_bound ||
        length > committed_bound - offset) {
        return journal_failure<void>(operation,
                                     L"descriptor range is invalid");
    }
    return Result<void>::success();
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > UINT64_MAX - left) return false;
    output = left + right;
    return true;
}

template <std::size_t Size>
std::span<const std::byte> domain_bytes(const char (&domain)[Size]) noexcept {
    return {reinterpret_cast<const std::byte*>(domain), Size - 1};
}

std::array<std::byte, 8> little_u64(std::uint64_t value) noexcept {
    std::array<std::byte, 8> bytes{};
    auto offset = std::size_t{0};
    static_cast<void>(write_u64_le(bytes, offset, value));
    return bytes;
}

std::array<std::byte, 4> little_u32(std::uint32_t value) noexcept {
    std::array<std::byte, 4> bytes{};
    auto offset = std::size_t{0};
    static_cast<void>(write_u32_le(bytes, offset, value));
    return bytes;
}

Result<void> update_digest(Sha256Stream& stream,
                           std::span<const std::byte> bytes) {
    auto result = stream.update(bytes);
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

Result<void> validate_header(const JournalHeader& header,
                             std::wstring_view operation) {
    if (header.image_uuid.is_nil()) {
        return journal_failure<void>(operation, L"image UUID is nil");
    }
    if (all_zero(header.journal_identity.journal_uuid) ||
        std::equal(header.image_uuid.bytes.begin(),
                   header.image_uuid.bytes.end(),
                   header.journal_identity.journal_uuid.begin())) {
        return journal_failure<void>(operation,
                                    L"journal UUID is invalid");
    }
    if (header.epoch == 0 || header.epoch == kInvalidGeneration ||
        header.base_checkpoint_generation == kInvalidGeneration) {
        return journal_failure<void>(operation,
                                    L"journal generation is invalid");
    }
    if (validate_geometry(header.geometry) != GeometryStatus::Ok) {
        return journal_failure<void>(operation, L"journal geometry is invalid");
    }
    const auto expected_geometry = compute_geometry_digest(header.geometry);
    if (!expected_geometry.ok()) {
        return Result<void>::failure(expected_geometry.error());
    }
    if (header.geometry_digest != expected_geometry.value() ||
        all_zero(header.base_identity_digest)) {
        return journal_failure<void>(operation,
                                    L"journal digest metadata is invalid");
    }
    if ((header.feature_flags & kJournalMandatoryFeatureMask) != 0) {
        return journal_failure<void>(operation,
                                    L"unknown mandatory header feature");
    }

    switch (header.predecessor_kind) {
    case JournalPredecessorKind::Genesis: {
        if (header.epoch != 1 || header.base_checkpoint_generation != 0 ||
            header.predecessor_epoch != 0 ||
            !all_zero(header.predecessor_journal_identity.journal_uuid)) {
            return journal_failure<void>(operation,
                                        L"genesis predecessor is invalid");
        }
        const auto genesis = compute_genesis_digest(
            header.image_uuid, header.geometry_digest,
            header.base_identity_digest);
        if (!genesis.ok()) return Result<void>::failure(genesis.error());
        if (header.predecessor_digest != genesis.value()) {
            return journal_failure<void>(operation,
                                        L"genesis digest mismatch");
        }
        break;
    }
    case JournalPredecessorKind::Checkpoint:
        if (header.epoch <= 1 ||
            header.predecessor_epoch != header.epoch - 1 ||
            all_zero(header.predecessor_journal_identity.journal_uuid) ||
            header.predecessor_journal_identity == header.journal_identity ||
            all_zero(header.predecessor_digest)) {
            return journal_failure<void>(operation,
                                        L"checkpoint predecessor is invalid");
        }
        break;
    default:
        return journal_failure<void>(operation,
                                    L"unknown predecessor kind");
    }
    return Result<void>::success();
}

}  // namespace

Result<Sha256Digest> compute_geometry_digest(
    const ImageGeometry& geometry) {
    if (validate_geometry(geometry) != GeometryStatus::Ok) {
        return journal_failure<Sha256Digest>(
            L"compute_geometry_digest", L"image geometry is invalid");
    }
    const auto virtual_bytes = little_u64(geometry.virtual_bytes);
    const auto logical = little_u32(geometry.logical_sector_size);
    const auto physical = little_u32(geometry.physical_sector_size);
    const auto m2 = little_u64(kM2BlockSize);
    const auto vhdx = little_u32(kVhdxDataBlockBytes);
    Sha256Stream stream;
    for (const auto chunk : {
             domain_bytes("MYRD-GEOMETRY-V1"),
             std::span<const std::byte>{virtual_bytes},
             std::span<const std::byte>{logical},
             std::span<const std::byte>{physical},
             std::span<const std::byte>{m2},
             std::span<const std::byte>{vhdx}}) {
        auto update = update_digest(stream, chunk);
        if (!update.ok()) {
            return Result<Sha256Digest>::failure(update.error());
        }
    }
    return stream.finalize_digest();
}

Result<Sha256Digest> compute_base_identity_digest(
    const VhdxFileIdentity& identity) {
    if (all_zero(identity.file_write_guid) ||
        all_zero(identity.data_write_guid)) {
        return journal_failure<Sha256Digest>(
            L"compute_base_identity_digest", L"VHDX identity is nil");
    }
    Sha256Stream stream;
    for (const auto chunk : {
             domain_bytes("MYRD-BASE-ID-V1"),
             std::span<const std::byte>{identity.file_write_guid},
             std::span<const std::byte>{identity.data_write_guid}}) {
        auto update = update_digest(stream, chunk);
        if (!update.ok()) {
            return Result<Sha256Digest>::failure(update.error());
        }
    }
    return stream.finalize_digest();
}

Result<Sha256Digest> compute_genesis_digest(
    const ImageUuid& image_uuid,
    const Sha256Digest& geometry_digest,
    const Sha256Digest& base_identity_digest) {
    if (image_uuid.is_nil() || all_zero(geometry_digest) ||
        all_zero(base_identity_digest)) {
        return journal_failure<Sha256Digest>(
            L"compute_genesis_digest", L"genesis input is invalid");
    }
    const auto generation_zero = little_u64(0);
    Sha256Stream stream;
    for (const auto chunk : {
             domain_bytes("MYRD-GENESIS-V1"),
             std::span<const std::byte>{image_uuid.bytes},
             std::span<const std::byte>{geometry_digest},
             std::span<const std::byte>{generation_zero},
             std::span<const std::byte>{base_identity_digest}}) {
        auto update = update_digest(stream, chunk);
        if (!update.ok()) {
            return Result<Sha256Digest>::failure(update.error());
        }
    }
    return stream.finalize_digest();
}

Result<JournalHeaderBytes> encode_journal_header(
    const JournalHeader& header) {
    const auto validation = validate_header(header, L"encode_journal_header");
    if (!validation.ok()) {
        return Result<JournalHeaderBytes>::failure(validation.error());
    }

    JournalHeaderBytes bytes{};
    const bool ok =
        copy_to(bytes, 0, kJournalHeaderMagic) &&
        write_integer(bytes, kMajorOffset, kM3FormatMajor) &&
        write_integer(bytes, kMinorOffset, kM3FormatMinor) &&
        write_integer(bytes, kHeaderLengthOffset,
                      static_cast<std::uint32_t>(kJournalAlignment)) &&
        copy_to(bytes, kImageUuidOffset, header.image_uuid.bytes) &&
        copy_to(bytes, kJournalUuidOffset,
                header.journal_identity.journal_uuid) &&
        write_integer(bytes, kEpochOffset, header.epoch) &&
        write_integer(bytes, kVirtualBytesOffset,
                      header.geometry.virtual_bytes) &&
        write_integer(bytes, kLogicalSectorOffset,
                      header.geometry.logical_sector_size) &&
        write_integer(bytes, kPhysicalSectorOffset,
                      header.geometry.physical_sector_size) &&
        write_integer(bytes, kM2BlockSizeOffset,
                      static_cast<std::uint32_t>(kM2BlockSize)) &&
        write_integer(bytes, kVhdxBlockSizeOffset, kVhdxDataBlockBytes) &&
        write_integer(bytes, kBaseGenerationOffset,
                      header.base_checkpoint_generation) &&
        write_integer(bytes, kPredecessorKindOffset,
                      static_cast<std::uint32_t>(header.predecessor_kind)) &&
        write_integer(bytes, kPredecessorEpochOffset,
                      header.predecessor_epoch) &&
        copy_to(bytes, kPredecessorUuidOffset,
                header.predecessor_journal_identity.journal_uuid) &&
        copy_to(bytes, kPredecessorDigestOffset,
                header.predecessor_digest) &&
        copy_to(bytes, kGeometryDigestOffset, header.geometry_digest) &&
        copy_to(bytes, kBaseIdentityDigestOffset,
                header.base_identity_digest) &&
        write_integer(bytes, kFeatureFlagsOffset, header.feature_flags);
    if (!ok) {
        return journal_failure<JournalHeaderBytes>(
            L"encode_journal_header", L"journal header layout overflow");
    }
    const auto crc = crc32c(bytes);
    if (!write_integer(bytes, kHeaderCrcOffset, crc)) {
        return journal_failure<JournalHeaderBytes>(
            L"encode_journal_header", L"journal CRC field overflow");
    }
    return Result<JournalHeaderBytes>::success(std::move(bytes));
}

Result<JournalHeader> decode_journal_header(
    std::span<const std::byte> input) {
    if (input.size() != kJournalAlignment) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal header must be 4096 bytes");
    }
    if (!std::equal(kJournalHeaderMagic.begin(), kJournalHeaderMagic.end(),
                    input.begin())) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal header magic mismatch");
    }
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t length = 0;
    std::uint32_t stored_crc = 0;
    if (!read_integer(input, kMajorOffset, major) ||
        !read_integer(input, kMinorOffset, minor) ||
        !read_integer(input, kHeaderLengthOffset, length) ||
        !read_integer(input, kHeaderCrcOffset, stored_crc)) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal fixed header is truncated");
    }
    if (major != kM3FormatMajor || length != kJournalAlignment) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"unsupported journal header version");
    }
    (void)minor;

    JournalHeaderBytes crc_bytes{};
    std::copy(input.begin(), input.end(), crc_bytes.begin());
    std::fill_n(crc_bytes.begin() + kHeaderCrcOffset,
                sizeof(stored_crc), std::byte{0});
    if (crc32c(crc_bytes) != stored_crc) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal header CRC32C mismatch");
    }
    if (require_zero(input.subspan(100, 4)) != ByteCodecError::none ||
        require_zero(input.subspan(kSemanticBytes)) != ByteCodecError::none) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal reserved bytes are nonzero");
    }

    JournalHeader header{};
    std::uint32_t m2_block_size = 0;
    std::uint32_t vhdx_block_size = 0;
    std::uint32_t predecessor_kind = 0;
    const bool ok =
        copy_from(input, kImageUuidOffset, header.image_uuid.bytes) &&
        copy_from(input, kJournalUuidOffset,
                  header.journal_identity.journal_uuid) &&
        read_integer(input, kEpochOffset, header.epoch) &&
        read_integer(input, kVirtualBytesOffset,
                     header.geometry.virtual_bytes) &&
        read_integer(input, kLogicalSectorOffset,
                     header.geometry.logical_sector_size) &&
        read_integer(input, kPhysicalSectorOffset,
                     header.geometry.physical_sector_size) &&
        read_integer(input, kM2BlockSizeOffset, m2_block_size) &&
        read_integer(input, kVhdxBlockSizeOffset, vhdx_block_size) &&
        read_integer(input, kBaseGenerationOffset,
                     header.base_checkpoint_generation) &&
        read_integer(input, kPredecessorKindOffset, predecessor_kind) &&
        read_integer(input, kPredecessorEpochOffset,
                     header.predecessor_epoch) &&
        copy_from(input, kPredecessorUuidOffset,
                  header.predecessor_journal_identity.journal_uuid) &&
        copy_from(input, kPredecessorDigestOffset,
                  header.predecessor_digest) &&
        copy_from(input, kGeometryDigestOffset, header.geometry_digest) &&
        copy_from(input, kBaseIdentityDigestOffset,
                  header.base_identity_digest) &&
        read_integer(input, kFeatureFlagsOffset, header.feature_flags);
    if (!ok || m2_block_size != kM2BlockSize ||
        vhdx_block_size != kVhdxDataBlockBytes ||
        predecessor_kind >
            static_cast<std::uint32_t>(JournalPredecessorKind::Checkpoint)) {
        return journal_failure<JournalHeader>(
            L"decode_journal_header", L"journal header field is invalid");
    }
    header.predecessor_kind =
        static_cast<JournalPredecessorKind>(predecessor_kind);
    const auto validation = validate_header(header, L"decode_journal_header");
    if (!validation.ok()) {
        return Result<JournalHeader>::failure(validation.error());
    }
    return Result<JournalHeader>::success(std::move(header));
}

Result<std::uint32_t> journal_block_logical_length(
    const ImageGeometry& geometry, const std::uint64_t block_index) {
    if (validate_geometry(geometry) != GeometryStatus::Ok) {
        return journal_failure<std::uint32_t>(
            L"journal_block_logical_length", L"image geometry is invalid");
    }
    const auto block_count =
        (geometry.virtual_bytes - 1) / kM2BlockSize + 1;
    if (block_index >= block_count) {
        return journal_failure<std::uint32_t>(
            L"journal_block_logical_length", L"block index is out of range");
    }
    const auto block_start = block_index * kM2BlockSize;
    const auto remaining = geometry.virtual_bytes - block_start;
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(
        (std::min)(remaining, kM2BlockSize)));
}

Result<std::vector<std::byte>> encode_block_record(
    const BlockJournalRecord& record,
    const std::span<const std::byte> payload,
    const ImageGeometry& geometry) {
    const auto validation = validate_block_metadata(
        record, geometry, L"encode_block_record");
    if (!validation.ok()) {
        return Result<std::vector<std::byte>>::failure(validation.error());
    }
    const auto logical = journal_block_logical_length(
        geometry, record.block_index);
    if (!logical.ok()) {
        return Result<std::vector<std::byte>>::failure(logical.error());
    }
    const bool data = record.common.type == JournalRecordType::Data;
    if ((data && payload.size() != kM2BlockSize) ||
        (!data && !payload.empty())) {
        return journal_failure<std::vector<std::byte>>(
            L"encode_block_record", L"block payload size is invalid");
    }
    if (data && require_zero(payload.subspan(logical.value())) !=
                    ByteCodecError::none) {
        return journal_failure<std::vector<std::byte>>(
            L"encode_block_record", L"final block payload tail is nonzero");
    }

    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(
            physical_length(record.common.type)));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<std::byte>>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"encode_block_record", L"allocating record bytes failed", 0});
    }
    auto output = std::span<std::byte>{bytes};
    auto offset = std::size_t{0};
    bool ok = write_bytes(output, offset, kJournalRecordMagic) ==
                  ByteCodecError::none &&
              write_record_integer(output, kMajorOffset, kM3FormatMajor) &&
              write_record_integer(output, kMinorOffset, kM3FormatMinor) &&
              write_record_integer(
                  output, kRecordHeaderLengthOffset,
                  static_cast<std::uint32_t>(kJournalAlignment)) &&
              write_record_integer(
                  output, kRecordTypeOffset,
                  static_cast<std::uint16_t>(record.common.type)) &&
              write_record_integer(output, kRecordLengthOffset,
                                   physical_length(record.common.type)) &&
              write_record_integer(output, kRecordGenerationOffset,
                                   record.common.checkpoint_generation) &&
              write_record_integer(output, kRecordOperationIdOffset,
                                   record.common.operation_id) &&
              write_record_integer(output, kRecordSequenceOffset,
                                   record.common.sequence) &&
              write_record_integer(output, kRecordFlagsOffset,
                                   record.common.flags) &&
              write_record_integer(output, kBlockIndexOffset,
                                   record.block_index) &&
              write_record_integer(output, kSnapshotVersionOffset,
                                   record.snapshot_version) &&
              write_record_integer(output, kSourceGenerationOffset,
                                   record.source_generation) &&
              write_record_integer(output, kDirtyEpochOffset,
                                   record.dirty_epoch);
    if (data) {
        ok = ok &&
             write_record_integer(output, kLogicalPayloadLengthOffset,
                                  logical.value()) &&
             write_record_integer(
                 output, kStoredPayloadLengthOffset,
                 static_cast<std::uint32_t>(kM2BlockSize)) &&
             write_record_integer(output, kPayloadCrcOffset,
                                  crc32c(payload));
        std::copy(payload.begin(), payload.end(),
                  bytes.begin() + kJournalAlignment);
    }
    if (!ok) {
        return journal_failure<std::vector<std::byte>>(
            L"encode_block_record", L"block record layout overflow");
    }
    if (!write_record_integer(output, kRecordCrcOffset, crc32c(bytes))) {
        return journal_failure<std::vector<std::byte>>(
            L"encode_block_record", L"block record CRC field overflow");
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<DecodedBlockJournalRecord> decode_block_record(
    const std::span<const std::byte> input,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound,
    const ImageGeometry& geometry) {
    constexpr auto kMinimumRecordHeader = kBlockSemanticBytes;
    if (input.size() < kMinimumRecordHeader) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record header is truncated");
    }
    if (!std::equal(kJournalRecordMagic.begin(), kJournalRecordMagic.end(),
                    input.begin())) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record magic mismatch");
    }
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t header_length = 0;
    std::uint16_t type_wire = 0;
    std::uint64_t record_length = 0;
    std::uint32_t stored_crc = 0;
    if (!read_record_integer(input, kMajorOffset, major) ||
        !read_record_integer(input, kMinorOffset, minor) ||
        !read_record_integer(input, kRecordHeaderLengthOffset,
                             header_length) ||
        !read_record_integer(input, kRecordTypeOffset, type_wire) ||
        !read_record_integer(input, kRecordLengthOffset, record_length) ||
        !read_record_integer(input, kRecordCrcOffset, stored_crc)) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record fixed header is truncated");
    }
    if (major != kM3FormatMajor || header_length != kJournalAlignment) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"unsupported record version");
    }
    (void)minor;
    if (type_wire < static_cast<std::uint16_t>(JournalRecordType::Data) ||
        type_wire > static_cast<std::uint16_t>(JournalRecordType::Discard)) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"unknown block record type");
    }
    const auto type = static_cast<JournalRecordType>(type_wire);
    const auto expected_length = physical_length(type);
    if (record_length != expected_length || input.size() != expected_length) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record physical length mismatch");
    }
    const auto location = validate_record_location(
        record_offset, record_length, committed_bound,
        L"decode_block_record");
    if (!location.ok()) {
        return Result<DecodedBlockJournalRecord>::failure(location.error());
    }

    const auto computed_crc = compute_record_crc(input);
    if (!computed_crc.ok()) {
        return Result<DecodedBlockJournalRecord>::failure(
            computed_crc.error());
    }
    if (computed_crc.value() != stored_crc) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record CRC32C mismatch");
    }
    if (require_zero(input.subspan(22, 2)) != ByteCodecError::none ||
        require_zero(input.subspan(36, 4)) != ByteCodecError::none ||
        require_zero(input.subspan(kBlockSemanticBytes,
                                   kJournalAlignment -
                                       kBlockSemanticBytes)) !=
            ByteCodecError::none) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"record reserved bytes are nonzero");
    }

    DecodedBlockJournalRecord decoded{};
    decoded.metadata.common.type = type;
    std::uint32_t stored_payload_length = 0;
    bool ok =
        read_record_integer(input, kRecordGenerationOffset,
                            decoded.metadata.common.checkpoint_generation) &&
        read_record_integer(input, kRecordOperationIdOffset,
                            decoded.metadata.common.operation_id) &&
        read_record_integer(input, kRecordSequenceOffset,
                            decoded.metadata.common.sequence) &&
        read_record_integer(input, kRecordFlagsOffset,
                            decoded.metadata.common.flags) &&
        read_record_integer(input, kBlockIndexOffset,
                            decoded.metadata.block_index) &&
        read_record_integer(input, kSnapshotVersionOffset,
                            decoded.metadata.snapshot_version) &&
        read_record_integer(input, kSourceGenerationOffset,
                            decoded.metadata.source_generation) &&
        read_record_integer(input, kDirtyEpochOffset,
                            decoded.metadata.dirty_epoch) &&
        read_record_integer(input, kLogicalPayloadLengthOffset,
                            decoded.logical_payload_length) &&
        read_record_integer(input, kStoredPayloadLengthOffset,
                            stored_payload_length) &&
        read_record_integer(input, kPayloadCrcOffset,
                            decoded.payload_crc32c);
    if (!ok) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"block record fields are truncated");
    }
    const auto validation = validate_block_metadata(
        decoded.metadata, geometry, L"decode_block_record");
    if (!validation.ok()) {
        return Result<DecodedBlockJournalRecord>::failure(validation.error());
    }
    const auto logical = journal_block_logical_length(
        geometry, decoded.metadata.block_index);
    if (!logical.ok()) {
        return Result<DecodedBlockJournalRecord>::failure(logical.error());
    }
    if (type == JournalRecordType::Data) {
        if (decoded.logical_payload_length != logical.value() ||
            stored_payload_length != kM2BlockSize) {
            return journal_failure<DecodedBlockJournalRecord>(
                L"decode_block_record", L"DATA payload lengths are invalid");
        }
        decoded.payload = input.subspan(kJournalAlignment,
                                        kM2BlockSize);
        if (crc32c(decoded.payload) != decoded.payload_crc32c) {
            return journal_failure<DecodedBlockJournalRecord>(
                L"decode_block_record", L"DATA payload CRC32C mismatch");
        }
        if (require_zero(decoded.payload.subspan(
                decoded.logical_payload_length)) != ByteCodecError::none) {
            return journal_failure<DecodedBlockJournalRecord>(
                L"decode_block_record", L"final block tail is nonzero");
        }
    } else if (decoded.logical_payload_length != 0 ||
               stored_payload_length != 0 || decoded.payload_crc32c != 0) {
        return journal_failure<DecodedBlockJournalRecord>(
            L"decode_block_record", L"zero-kind payload fields are nonzero");
    }
    return Result<DecodedBlockJournalRecord>::success(decoded);
}

namespace {

Result<void> validate_index_node(const JournalIndexNode& node,
                                 const std::uint64_t record_offset,
                                 const std::uint64_t committed_bound,
                                 std::wstring_view operation) {
    if (node.common.type != JournalRecordType::IndexNode ||
        node.common.checkpoint_generation == 0 ||
        node.common.checkpoint_generation == kInvalidGeneration ||
        node.common.operation_id == 0 ||
        (node.common.flags & kJournalMandatoryRecordFlags) != 0 ||
        node.level >= kMaxIndexDepth) {
        return journal_failure<void>(operation,
                                     L"index node identity is invalid");
    }
    const auto location = validate_record_location(
        record_offset, kIndexNodeBytes, committed_bound, operation);
    if (!location.ok()) return location;

    const bool leaf = node.level == 0;
    const auto count = leaf ? node.leaf_entries.size()
                            : node.internal_entries.size();
    if (count == 0 || count > kJournalIndexMaxEntries ||
        (leaf && !node.internal_entries.empty()) ||
        (!leaf && !node.leaf_entries.empty())) {
        return journal_failure<void>(operation,
                                     L"index node entry set is invalid");
    }

    std::uint64_t previous_max = 0;
    bool first = true;
    if (leaf) {
        for (const auto& entry : node.leaf_entries) {
            if (!block_type(entry.record_type) ||
                entry.record_generation == 0 ||
                entry.record_generation == kInvalidGeneration ||
                entry.record_generation >
                    node.common.checkpoint_generation ||
                (!first && entry.block_index <= previous_max) ||
                ((entry.record_type == JournalRecordType::Zero ||
                  entry.record_type == JournalRecordType::Discard) &&
                 entry.payload_crc32c != 0)) {
                return journal_failure<void>(operation,
                                             L"leaf entry is invalid");
            }
            const auto range = validate_descriptor_range(
                entry.record_offset, entry.record_length,
                physical_length(entry.record_type), record_offset,
                committed_bound, operation);
            if (!range.ok()) return range;
            previous_max = entry.block_index;
            first = false;
        }
    } else {
        for (const auto& entry : node.internal_entries) {
            if (entry.minimum_key > entry.maximum_key ||
                (!first && entry.minimum_key <= previous_max) ||
                entry.child_generation == 0 ||
                entry.child_generation == kInvalidGeneration ||
                entry.child_generation >
                    node.common.checkpoint_generation) {
                return journal_failure<void>(operation,
                                             L"internal entry is invalid");
            }
            const auto range = validate_descriptor_range(
                entry.child_offset, entry.child_length,
                kIndexNodeBytes, record_offset, committed_bound, operation);
            if (!range.ok()) return range;
            previous_max = entry.maximum_key;
            first = false;
        }
    }
    return Result<void>::success();
}

}  // namespace

Result<std::array<std::byte, kIndexNodeBytes>> encode_index_node(
    const JournalIndexNode& node,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound) {
    const auto validation = validate_index_node(
        node, record_offset, committed_bound, L"encode_index_node");
    if (!validation.ok()) {
        return Result<std::array<std::byte, kIndexNodeBytes>>::failure(
            validation.error());
    }

    std::array<std::byte, kIndexNodeBytes> bytes{};
    auto output = std::span<std::byte>{bytes};
    const auto common = write_common_record(
        output, node.common, kIndexNodeBytes, L"encode_index_node");
    if (!common.ok()) {
        return Result<std::array<std::byte, kIndexNodeBytes>>::failure(
            common.error());
    }
    const bool leaf = node.level == 0;
    const auto count = static_cast<std::uint16_t>(
        leaf ? node.leaf_entries.size() : node.internal_entries.size());
    const auto minimum_key = leaf ? node.leaf_entries.front().block_index
                                  : node.internal_entries.front().minimum_key;
    const auto maximum_key = leaf ? node.leaf_entries.back().block_index
                                  : node.internal_entries.back().maximum_key;
    bool ok =
        write_record_integer(output, kIndexLevelOffset, node.level) &&
        write_record_integer(output, kIndexEntryCountOffset, count) &&
        write_record_integer(output, kIndexEntryKindOffset,
                             static_cast<std::uint16_t>(leaf ? 0 : 1)) &&
        write_record_integer(output, kIndexEntryWidthOffset,
                             kJournalIndexEntryBytes) &&
        write_record_integer(output, kIndexMinimumKeyOffset, minimum_key) &&
        write_record_integer(output, kIndexMaximumKeyOffset, maximum_key) &&
        write_record_integer(output, kIndexNodeGenerationOffset,
                             node.common.checkpoint_generation);
    for (std::size_t index = 0; ok && index < count; ++index) {
        const auto base = kIndexEntriesOffset +
                          index * kJournalIndexEntryBytes;
        if (leaf) {
            const auto& entry = node.leaf_entries[index];
            ok = write_record_integer(output, base, entry.block_index) &&
                 write_record_integer(
                     output, base + 8,
                     static_cast<std::uint16_t>(entry.record_type)) &&
                 write_record_integer(output, base + 12,
                                      entry.payload_crc32c) &&
                 write_record_integer(output, base + 16,
                                      entry.record_offset) &&
                 write_record_integer(output, base + 24,
                                      entry.record_length) &&
                 write_record_integer(output, base + 32,
                                      entry.record_generation);
        } else {
            const auto& entry = node.internal_entries[index];
            ok = write_record_integer(output, base, entry.minimum_key) &&
                 write_record_integer(output, base + 8,
                                      entry.maximum_key) &&
                 write_record_integer(output, base + 16,
                                      entry.child_offset) &&
                 write_record_integer(output, base + 24,
                                      entry.child_length) &&
                 write_record_integer(output, base + 32,
                                      entry.child_crc32c) &&
                 write_record_integer(output, base + 40,
                                      entry.child_generation);
        }
    }
    if (!ok || !write_record_integer(output, kRecordCrcOffset,
                                     crc32c(bytes))) {
        return journal_failure<std::array<std::byte, kIndexNodeBytes>>(
            L"encode_index_node", L"index node layout overflow");
    }
    return Result<std::array<std::byte, kIndexNodeBytes>>::success(
        std::move(bytes));
}

Result<DecodedJournalIndexNode> decode_index_node(
    const std::span<const std::byte> input,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound) {
    const auto parsed = parse_common_record(
        input, record_offset, committed_bound, JournalRecordType::IndexNode,
        kIndexNodeBytes, L"decode_index_node");
    if (!parsed.ok()) {
        return Result<DecodedJournalIndexNode>::failure(parsed.error());
    }
    if (parsed.value().common.checkpoint_generation == 0 ||
        parsed.value().common.checkpoint_generation == kInvalidGeneration) {
        return journal_failure<DecodedJournalIndexNode>(
            L"decode_index_node", L"index generation is invalid");
    }

    DecodedJournalIndexNode decoded{};
    decoded.node.common = parsed.value().common;
    decoded.record_crc32c = parsed.value().record_crc32c;
    std::uint16_t count = 0;
    std::uint16_t kind = 0;
    std::uint16_t width = 0;
    std::uint64_t node_generation = 0;
    if (!read_record_integer(input, kIndexLevelOffset,
                             decoded.node.level) ||
        !read_record_integer(input, kIndexEntryCountOffset, count) ||
        !read_record_integer(input, kIndexEntryKindOffset, kind) ||
        !read_record_integer(input, kIndexEntryWidthOffset, width) ||
        !read_record_integer(input, kIndexMinimumKeyOffset,
                             decoded.minimum_key) ||
        !read_record_integer(input, kIndexMaximumKeyOffset,
                             decoded.maximum_key) ||
        !read_record_integer(input, kIndexNodeGenerationOffset,
                             node_generation)) {
        return journal_failure<DecodedJournalIndexNode>(
            L"decode_index_node", L"index node fields are truncated");
    }
    const bool leaf = decoded.node.level == 0;
    if (decoded.node.level >= kMaxIndexDepth || count == 0 ||
        count > kJournalIndexMaxEntries || width != kJournalIndexEntryBytes ||
        kind != static_cast<std::uint16_t>(leaf ? 0 : 1) ||
        node_generation != decoded.node.common.checkpoint_generation) {
        return journal_failure<DecodedJournalIndexNode>(
            L"decode_index_node", L"index node header is invalid");
    }
    const auto used_end = kIndexEntriesOffset +
                          static_cast<std::size_t>(count) *
                              kJournalIndexEntryBytes;
    if (require_zero(input.subspan(104, 24)) != ByteCodecError::none ||
        require_zero(input.subspan(used_end)) != ByteCodecError::none) {
        return journal_failure<DecodedJournalIndexNode>(
            L"decode_index_node", L"index node reserved bytes are nonzero");
    }

    try {
        if (leaf) decoded.node.leaf_entries.reserve(count);
        else decoded.node.internal_entries.reserve(count);
    } catch (const std::bad_alloc&) {
        return Result<DecodedJournalIndexNode>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"decode_index_node", L"allocating index entries failed", 0});
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto base = kIndexEntriesOffset +
                          index * kJournalIndexEntryBytes;
        if (leaf) {
            JournalIndexLeafEntry entry{};
            std::uint16_t type = 0;
            if (!read_record_integer(input, base, entry.block_index) ||
                !read_record_integer(input, base + 8, type) ||
                !read_record_integer(input, base + 12,
                                     entry.payload_crc32c) ||
                !read_record_integer(input, base + 16,
                                     entry.record_offset) ||
                !read_record_integer(input, base + 24,
                                     entry.record_length) ||
                !read_record_integer(input, base + 32,
                                     entry.record_generation) ||
                require_zero(input.subspan(base + 10, 2)) !=
                    ByteCodecError::none ||
                require_zero(input.subspan(base + 40, 8)) !=
                    ByteCodecError::none ||
                type < static_cast<std::uint16_t>(JournalRecordType::Data) ||
                type > static_cast<std::uint16_t>(
                           JournalRecordType::Discard)) {
                return journal_failure<DecodedJournalIndexNode>(
                    L"decode_index_node", L"leaf entry encoding is invalid");
            }
            entry.record_type = static_cast<JournalRecordType>(type);
            decoded.node.leaf_entries.push_back(entry);
        } else {
            JournalIndexInternalEntry entry{};
            if (!read_record_integer(input, base, entry.minimum_key) ||
                !read_record_integer(input, base + 8, entry.maximum_key) ||
                !read_record_integer(input, base + 16,
                                     entry.child_offset) ||
                !read_record_integer(input, base + 24,
                                     entry.child_length) ||
                !read_record_integer(input, base + 32,
                                     entry.child_crc32c) ||
                !read_record_integer(input, base + 40,
                                     entry.child_generation) ||
                require_zero(input.subspan(base + 36, 4)) !=
                    ByteCodecError::none) {
                return journal_failure<DecodedJournalIndexNode>(
                    L"decode_index_node",
                    L"internal entry encoding is invalid");
            }
            decoded.node.internal_entries.push_back(entry);
        }
    }
    const auto validation = validate_index_node(
        decoded.node, record_offset, committed_bound,
        L"decode_index_node");
    if (!validation.ok()) {
        return Result<DecodedJournalIndexNode>::failure(validation.error());
    }
    const auto derived_min = leaf
        ? decoded.node.leaf_entries.front().block_index
        : decoded.node.internal_entries.front().minimum_key;
    const auto derived_max = leaf
        ? decoded.node.leaf_entries.back().block_index
        : decoded.node.internal_entries.back().maximum_key;
    if (decoded.minimum_key != derived_min ||
        decoded.maximum_key != derived_max) {
        return journal_failure<DecodedJournalIndexNode>(
            L"decode_index_node", L"index node key range mismatch");
    }
    return Result<DecodedJournalIndexNode>::success(std::move(decoded));
}

namespace {

Result<void> validate_commit(const CheckpointCommitRecord& commit,
                             const JournalHeader& header,
                             const std::uint64_t record_offset,
                             const std::uint64_t committed_bound,
                             std::wstring_view operation) {
    if (commit.common.type != JournalRecordType::CheckpointCommit ||
        commit.common.operation_id == 0 ||
        (commit.common.flags & kJournalMandatoryRecordFlags) != 0 ||
        commit.image_uuid != header.image_uuid ||
        commit.journal_identity != header.journal_identity ||
        commit.journal_epoch != header.epoch ||
        commit.geometry_digest != header.geometry_digest ||
        commit.common.checkpoint_generation != commit.new_generation ||
        all_zero(commit.previous_digest) ||
        all_zero(commit.new_chain_digest)) {
        return journal_failure<void>(operation,
                                     L"checkpoint identity is invalid");
    }
    const auto location = validate_record_location(
        record_offset, kFixedRecordBytes, committed_bound, operation);
    if (!location.ok()) return location;
    if (record_offset > UINT64_MAX - kFixedRecordBytes ||
        record_offset + kFixedRecordBytes != commit.journal_committed_end ||
        commit.journal_committed_end != committed_bound ||
        commit.checkpoint_begin_offset < kJournalAlignment ||
        commit.checkpoint_begin_offset % kJournalAlignment != 0 ||
        commit.checkpoint_begin_offset > record_offset) {
        return journal_failure<void>(operation,
                                     L"checkpoint terminal range is invalid");
    }

    std::uint64_t count = 0;
    if (!checked_add(commit.data_count, commit.zero_count, count) ||
        !checked_add(count, commit.discard_count, count) ||
        !checked_add(count, commit.index_node_count, count) ||
        count != commit.input_record_count ||
        commit.common.sequence != commit.input_record_count) {
        return journal_failure<void>(operation,
                                     L"checkpoint record counts are invalid");
    }

    const bool empty_root = commit.root.offset == 0 &&
                            commit.root.length == 0 &&
                            commit.root.crc32c == 0;
    const bool partially_empty_root =
        (commit.root.offset == 0 || commit.root.length == 0) && !empty_root;
    if (partially_empty_root) {
        return journal_failure<void>(operation,
                                     L"checkpoint root is partially empty");
    }
    if (!empty_root) {
        const auto root_range = validate_descriptor_range(
            commit.root.offset, commit.root.length, kIndexNodeBytes,
            record_offset, committed_bound, operation);
        if (!root_range.ok()) return root_range;
    }

    const bool genesis = commit.previous_generation == 0 &&
                         commit.new_generation == 0;
    if (genesis) {
        if (header.epoch != 1 || header.base_checkpoint_generation != 0 ||
            commit.checkpoint_begin_offset != kJournalAlignment ||
            record_offset != kJournalAlignment ||
            commit.input_record_count != 0 || !empty_root ||
            commit.previous_digest != header.predecessor_digest ||
            commit.new_chain_digest != header.predecessor_digest) {
            return journal_failure<void>(operation,
                                         L"genesis commit is invalid");
        }
    } else {
        if (commit.previous_generation == kInvalidGeneration ||
            commit.new_generation == 0 ||
            commit.new_generation == kInvalidGeneration ||
            commit.previous_generation >= kInvalidGeneration - 1 ||
            commit.new_generation != commit.previous_generation + 1 ||
            commit.previous_generation < header.base_checkpoint_generation) {
            return journal_failure<void>(operation,
                                         L"checkpoint generation is invalid");
        }
    }
    return Result<void>::success();
}

}  // namespace

Result<std::array<std::byte, kJournalAlignment>> encode_checkpoint_commit(
    const CheckpointCommitRecord& commit,
    const JournalHeader& header,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound) {
    const auto validation = validate_commit(
        commit, header, record_offset, committed_bound,
        L"encode_checkpoint_commit");
    if (!validation.ok()) {
        return Result<std::array<std::byte, kJournalAlignment>>::failure(
            validation.error());
    }
    std::array<std::byte, kJournalAlignment> bytes{};
    auto output = std::span<std::byte>{bytes};
    const auto common = write_common_record(
        output, commit.common, kFixedRecordBytes,
        L"encode_checkpoint_commit");
    if (!common.ok()) {
        return Result<std::array<std::byte, kJournalAlignment>>::failure(
            common.error());
    }
    std::copy(commit.image_uuid.bytes.begin(), commit.image_uuid.bytes.end(),
              bytes.begin() + kCommitImageUuidOffset);
    std::copy(commit.journal_identity.journal_uuid.begin(),
              commit.journal_identity.journal_uuid.end(),
              bytes.begin() + kCommitJournalUuidOffset);
    std::copy(commit.previous_digest.begin(), commit.previous_digest.end(),
              bytes.begin() + kCommitPreviousDigestOffset);
    std::copy(commit.new_chain_digest.begin(),
              commit.new_chain_digest.end(),
              bytes.begin() + kCommitNewDigestOffset);
    std::copy(commit.geometry_digest.begin(), commit.geometry_digest.end(),
              bytes.begin() + kCommitGeometryDigestOffset);
    const bool ok =
        write_record_integer(output, kCommitJournalEpochOffset,
                             commit.journal_epoch) &&
        write_record_integer(output, kCommitPreviousGenerationOffset,
                             commit.previous_generation) &&
        write_record_integer(output, kCommitNewGenerationOffset,
                             commit.new_generation) &&
        write_record_integer(output, kCommitDataCountOffset,
                             commit.data_count) &&
        write_record_integer(output, kCommitZeroCountOffset,
                             commit.zero_count) &&
        write_record_integer(output, kCommitDiscardCountOffset,
                             commit.discard_count) &&
        write_record_integer(output, kCommitIndexCountOffset,
                             commit.index_node_count) &&
        write_record_integer(output, kCommitRootOffsetOffset,
                             commit.root.offset) &&
        write_record_integer(output, kCommitRootLengthOffset,
                             commit.root.length) &&
        write_record_integer(output, kCommitRootCrcOffset,
                             commit.root.crc32c) &&
        write_record_integer(output, kCommitBeginOffset,
                             commit.checkpoint_begin_offset) &&
        write_record_integer(output, kCommitEndOffset,
                             commit.journal_committed_end) &&
        write_record_integer(output, kCommitInputRecordCountOffset,
                             commit.input_record_count) &&
        write_record_integer(output, kRecordCrcOffset, crc32c(bytes));
    if (!ok) {
        return journal_failure<std::array<std::byte, kJournalAlignment>>(
            L"encode_checkpoint_commit", L"commit layout overflow");
    }
    return Result<std::array<std::byte, kJournalAlignment>>::success(
        std::move(bytes));
}

Result<CheckpointCommitRecord> decode_checkpoint_commit(
    const std::span<const std::byte> input,
    const JournalHeader& header,
    const std::uint64_t record_offset,
    const std::uint64_t committed_bound) {
    const auto parsed = parse_common_record(
        input, record_offset, committed_bound,
        JournalRecordType::CheckpointCommit, kFixedRecordBytes,
        L"decode_checkpoint_commit");
    if (!parsed.ok()) {
        return Result<CheckpointCommitRecord>::failure(parsed.error());
    }
    if (require_zero(input.subspan(244, 4)) != ByteCodecError::none ||
        require_zero(input.subspan(kCommitSemanticBytes)) !=
            ByteCodecError::none) {
        return journal_failure<CheckpointCommitRecord>(
            L"decode_checkpoint_commit",
            L"commit reserved bytes are nonzero");
    }

    CheckpointCommitRecord commit{};
    commit.common = parsed.value().common;
    std::copy_n(input.begin() + kCommitImageUuidOffset, 16,
                commit.image_uuid.bytes.begin());
    std::copy_n(input.begin() + kCommitJournalUuidOffset, 16,
                commit.journal_identity.journal_uuid.begin());
    std::copy_n(input.begin() + kCommitPreviousDigestOffset, 32,
                commit.previous_digest.begin());
    std::copy_n(input.begin() + kCommitNewDigestOffset, 32,
                commit.new_chain_digest.begin());
    std::copy_n(input.begin() + kCommitGeometryDigestOffset, 32,
                commit.geometry_digest.begin());
    const bool ok =
        read_record_integer(input, kCommitJournalEpochOffset,
                            commit.journal_epoch) &&
        read_record_integer(input, kCommitPreviousGenerationOffset,
                            commit.previous_generation) &&
        read_record_integer(input, kCommitNewGenerationOffset,
                            commit.new_generation) &&
        read_record_integer(input, kCommitDataCountOffset,
                            commit.data_count) &&
        read_record_integer(input, kCommitZeroCountOffset,
                            commit.zero_count) &&
        read_record_integer(input, kCommitDiscardCountOffset,
                            commit.discard_count) &&
        read_record_integer(input, kCommitIndexCountOffset,
                            commit.index_node_count) &&
        read_record_integer(input, kCommitRootOffsetOffset,
                            commit.root.offset) &&
        read_record_integer(input, kCommitRootLengthOffset,
                            commit.root.length) &&
        read_record_integer(input, kCommitRootCrcOffset,
                            commit.root.crc32c) &&
        read_record_integer(input, kCommitBeginOffset,
                            commit.checkpoint_begin_offset) &&
        read_record_integer(input, kCommitEndOffset,
                            commit.journal_committed_end) &&
        read_record_integer(input, kCommitInputRecordCountOffset,
                            commit.input_record_count);
    if (!ok) {
        return journal_failure<CheckpointCommitRecord>(
            L"decode_checkpoint_commit", L"commit fields are truncated");
    }
    const auto validation = validate_commit(
        commit, header, record_offset, committed_bound,
        L"decode_checkpoint_commit");
    if (!validation.ok()) {
        return Result<CheckpointCommitRecord>::failure(validation.error());
    }
    return Result<CheckpointCommitRecord>::success(std::move(commit));
}

namespace {

Result<JournalRecordType> peek_record_type(
    const std::span<const std::byte> bytes,
    std::wstring_view operation) {
    if (bytes.size() < kRecordTypeOffset + sizeof(std::uint16_t) ||
        !std::equal(kJournalRecordMagic.begin(), kJournalRecordMagic.end(),
                    bytes.begin())) {
        return journal_failure<JournalRecordType>(
            operation, L"record prefix is invalid");
    }
    std::uint16_t type = 0;
    if (!read_record_integer(bytes, kRecordTypeOffset, type) ||
        type < static_cast<std::uint16_t>(JournalRecordType::Data) ||
        type > static_cast<std::uint16_t>(
                   JournalRecordType::CheckpointCommit)) {
        return journal_failure<JournalRecordType>(
            operation, L"record type is unknown");
    }
    return Result<JournalRecordType>::success(
        static_cast<JournalRecordType>(type));
}

Result<void> validate_digest_context(
    const CheckpointDigestContext& context,
    std::wstring_view operation) {
    const bool genesis = context.previous_generation == 0 &&
                         context.new_generation == 0;
    const bool next = context.previous_generation <
                          kInvalidGeneration - 1 &&
                      context.new_generation ==
                          context.previous_generation + 1;
    if (all_zero(context.previous_digest) || context.image_uuid.is_nil() ||
        context.journal_epoch == 0 ||
        context.journal_epoch == kInvalidGeneration ||
        all_zero(context.geometry_digest) || (!genesis && !next)) {
        return journal_failure<void>(operation,
                                     L"checkpoint digest context is invalid");
    }
    return Result<void>::success();
}

}  // namespace

Result<JournalRecordType> decode_journal_record_type_prefix(
    const std::span<const std::byte> bytes) {
    return peek_record_type(bytes, L"decode_journal_record_type_prefix");
}

Result<std::uint64_t> journal_record_physical_length(
    const JournalRecordType type) {
    if (!block_type(type) && type != JournalRecordType::IndexNode &&
        type != JournalRecordType::CheckpointCommit) {
        return journal_failure<std::uint64_t>(
            L"journal_record_physical_length", L"record type is unknown");
    }
    return Result<std::uint64_t>::success(physical_length(type));
}

struct CheckpointChainDigest::Impl final {
    Sha256Stream stream{};
    bool finalized{false};
};

CheckpointChainDigest::CheckpointChainDigest(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

CheckpointChainDigest::CheckpointChainDigest(
    CheckpointChainDigest&&) noexcept = default;
CheckpointChainDigest& CheckpointChainDigest::operator=(
    CheckpointChainDigest&&) noexcept = default;
CheckpointChainDigest::~CheckpointChainDigest() = default;

Result<CheckpointChainDigest> CheckpointChainDigest::begin(
    const CheckpointDigestContext& context) {
    const auto validation = validate_digest_context(
        context, L"CheckpointChainDigest::begin");
    if (!validation.ok()) {
        return Result<CheckpointChainDigest>::failure(validation.error());
    }
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>();
    } catch (const std::bad_alloc&) {
        return Result<CheckpointChainDigest>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"CheckpointChainDigest::begin",
            L"allocating digest state failed", 0});
    }
    const auto epoch = little_u64(context.journal_epoch);
    const auto previous = little_u64(context.previous_generation);
    const auto next = little_u64(context.new_generation);
    for (const auto chunk : {
             domain_bytes("MYRD-CHECKPOINT-V1"),
             std::span<const std::byte>{context.previous_digest},
             std::span<const std::byte>{context.image_uuid.bytes},
             std::span<const std::byte>{epoch},
             std::span<const std::byte>{previous},
             std::span<const std::byte>{next},
             std::span<const std::byte>{context.geometry_digest}}) {
        const auto update = impl->stream.update(chunk);
        if (!update.ok()) {
            return Result<CheckpointChainDigest>::failure(update.error());
        }
    }
    return Result<CheckpointChainDigest>::success(
        CheckpointChainDigest{std::move(impl)});
}

Result<void> CheckpointChainDigest::append_non_commit_record(
    const std::span<const std::byte> bytes) {
    if (!impl_ || impl_->finalized) {
        return journal_failure<void>(
            L"CheckpointChainDigest::append_non_commit_record",
            L"digest stream is not writable");
    }
    const auto type = peek_record_type(
        bytes, L"CheckpointChainDigest::append_non_commit_record");
    if (!type.ok()) return Result<void>::failure(type.error());
    if (type.value() == JournalRecordType::CheckpointCommit) {
        return journal_failure<void>(
            L"CheckpointChainDigest::append_non_commit_record",
            L"commit cannot be appended as an input record");
    }
    const auto expected = physical_length(type.value());
    const auto parsed = parse_common_record(
        bytes, kJournalAlignment, kJournalAlignment + expected,
        type.value(), expected,
        L"CheckpointChainDigest::append_non_commit_record");
    if (!parsed.ok()) {
        return Result<void>::failure(parsed.error());
    }
    return impl_->stream.update(bytes);
}

Result<Sha256Digest> CheckpointChainDigest::finalize(
    const std::span<const std::byte> commit_bytes) {
    if (!impl_ || impl_->finalized) {
        return journal_failure<Sha256Digest>(
            L"CheckpointChainDigest::finalize",
            L"digest stream is not finalizable");
    }
    const auto type = peek_record_type(
        commit_bytes, L"CheckpointChainDigest::finalize");
    if (!type.ok()) return Result<Sha256Digest>::failure(type.error());
    if (type.value() != JournalRecordType::CheckpointCommit) {
        return journal_failure<Sha256Digest>(
            L"CheckpointChainDigest::finalize",
            L"final record is not a complete commit");
    }
    const auto parsed = parse_common_record(
        commit_bytes, kJournalAlignment,
        2 * static_cast<std::uint64_t>(kJournalAlignment),
        JournalRecordType::CheckpointCommit, kJournalAlignment,
        L"CheckpointChainDigest::finalize");
    if (!parsed.ok()) {
        return Result<Sha256Digest>::failure(parsed.error());
    }
    std::array<std::byte, kJournalAlignment> canonical{};
    std::copy(commit_bytes.begin(), commit_bytes.end(), canonical.begin());
    std::fill_n(canonical.begin() + kRecordCrcOffset,
                sizeof(std::uint32_t), std::byte{0});
    std::fill_n(canonical.begin() + kCommitNewDigestOffset,
                Sha256Digest{}.size(), std::byte{0});
    const auto update = impl_->stream.update(canonical);
    if (!update.ok()) {
        impl_->finalized = true;
        return Result<Sha256Digest>::failure(update.error());
    }
    impl_->finalized = true;
    return impl_->stream.finalize_digest();
}

struct CheckpointSequenceValidator::Impl final {
    JournalHeader header{};
    CheckpointGeneration previous_generation{0};
    CheckpointGeneration new_generation{0};
    Sha256Digest previous_digest{};
    std::uint64_t checkpoint_begin_offset{0};
    std::uint64_t committed_bound{0};
    std::uint64_t max_input_records{0};
    std::uint64_t next_offset{0};
    std::uint64_t expected_sequence{0};
    std::uint64_t operation_id{0};
    std::uint64_t data_count{0};
    std::uint64_t zero_count{0};
    std::uint64_t discard_count{0};
    std::uint64_t index_count{0};
    bool saw_index{false};
    bool saw_commit{false};
    CheckpointChainDigest digest;
    std::unordered_set<std::uint64_t> block_indices{};
    std::unordered_map<std::uint64_t, std::uint32_t> current_nodes{};
    std::optional<CheckpointCommitRecord> commit{};

    Impl(const JournalHeader& source_header,
         CheckpointGeneration source_previous_generation,
         CheckpointGeneration source_new_generation,
         const Sha256Digest& source_previous_digest,
         std::uint64_t source_begin,
         std::uint64_t source_bound,
         std::uint64_t source_max_records,
         CheckpointChainDigest source_digest)
        : header(source_header),
          previous_generation(source_previous_generation),
          new_generation(source_new_generation),
          previous_digest(source_previous_digest),
          checkpoint_begin_offset(source_begin),
          committed_bound(source_bound),
          max_input_records(source_max_records),
          next_offset(source_begin),
          digest(std::move(source_digest)) {}
};

CheckpointSequenceValidator::CheckpointSequenceValidator(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CheckpointSequenceValidator::CheckpointSequenceValidator(
    CheckpointSequenceValidator&&) noexcept = default;
CheckpointSequenceValidator& CheckpointSequenceValidator::operator=(
    CheckpointSequenceValidator&&) noexcept = default;
CheckpointSequenceValidator::~CheckpointSequenceValidator() = default;

Result<CheckpointSequenceValidator> CheckpointSequenceValidator::begin(
    const JournalHeader& header,
    const CheckpointGeneration previous_generation,
    const CheckpointGeneration new_generation,
    const Sha256Digest& previous_digest,
    const std::uint64_t checkpoint_begin_offset,
    const std::uint64_t committed_bound,
    const std::uint64_t max_input_records) {
    const auto header_validation = validate_header(
        header, L"CheckpointSequenceValidator::begin");
    if (!header_validation.ok()) {
        return Result<CheckpointSequenceValidator>::failure(
            header_validation.error());
    }
    CheckpointDigestContext context{previous_digest, header.image_uuid,
                                    header.epoch, previous_generation,
                                    new_generation,
                                    header.geometry_digest};
    auto digest = CheckpointChainDigest::begin(context);
    if (!digest.ok()) {
        return Result<CheckpointSequenceValidator>::failure(digest.error());
    }
    if (checkpoint_begin_offset < kJournalAlignment ||
        checkpoint_begin_offset % kJournalAlignment != 0 ||
        checkpoint_begin_offset > committed_bound ||
        max_input_records == 0) {
        return journal_failure<CheckpointSequenceValidator>(
            L"CheckpointSequenceValidator::begin",
            L"checkpoint sequence bounds are invalid");
    }
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(
            header, previous_generation, new_generation, previous_digest,
            checkpoint_begin_offset, committed_bound, max_input_records,
            digest.move_value());
    } catch (const std::bad_alloc&) {
        return Result<CheckpointSequenceValidator>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"CheckpointSequenceValidator::begin",
            L"allocating sequence state failed", 0});
    }
    return Result<CheckpointSequenceValidator>::success(
        CheckpointSequenceValidator{std::move(impl)});
}

Result<void> CheckpointSequenceValidator::append(
    const std::span<const std::byte> bytes,
    const std::uint64_t record_offset) {
    if (!impl_ || impl_->saw_commit || record_offset != impl_->next_offset) {
        return journal_failure<void>(
            L"CheckpointSequenceValidator::append",
            L"record is noncontiguous or follows the commit");
    }
    if (impl_->expected_sequence == UINT64_MAX) {
        return journal_failure<void>(
            L"CheckpointSequenceValidator::append",
            L"record sequence overflow");
    }
    const auto type = peek_record_type(
        bytes, L"CheckpointSequenceValidator::append");
    if (!type.ok()) return Result<void>::failure(type.error());
    const auto expected_length = physical_length(type.value());
    if (bytes.size() != expected_length ||
        record_offset > impl_->committed_bound ||
        expected_length > impl_->committed_bound - record_offset) {
        return journal_failure<void>(
            L"CheckpointSequenceValidator::append",
            L"record range is outside the sequence bound");
    }
    const auto parsed = parse_common_record(
        bytes, record_offset, impl_->committed_bound, type.value(),
        expected_length, L"CheckpointSequenceValidator::append");
    if (!parsed.ok()) return Result<void>::failure(parsed.error());
    const auto& common = parsed.value().common;
    if (common.checkpoint_generation != impl_->new_generation ||
        common.sequence != impl_->expected_sequence ||
        (impl_->operation_id != 0 &&
         common.operation_id != impl_->operation_id)) {
        return journal_failure<void>(
            L"CheckpointSequenceValidator::append",
            L"record generation, sequence or operation is inconsistent");
    }
    if (impl_->operation_id == 0) impl_->operation_id = common.operation_id;

    if (block_type(type.value())) {
        if (impl_->saw_index ||
            impl_->expected_sequence >= impl_->max_input_records) {
            return journal_failure<void>(
                L"CheckpointSequenceValidator::append",
                L"block record order or count is invalid");
        }
        const auto decoded = decode_block_record(
            bytes, record_offset, impl_->committed_bound,
            impl_->header.geometry);
        if (!decoded.ok()) return Result<void>::failure(decoded.error());
        try {
            if (!impl_->block_indices.insert(
                    decoded.value().metadata.block_index).second) {
                return journal_failure<void>(
                    L"CheckpointSequenceValidator::append",
                    L"duplicate block update in checkpoint");
            }
        } catch (const std::bad_alloc&) {
            return Result<void>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"CheckpointSequenceValidator::append",
                L"allocating block dedup state failed", 0});
        }
        if (type.value() == JournalRecordType::Data) ++impl_->data_count;
        else if (type.value() == JournalRecordType::Zero) ++impl_->zero_count;
        else ++impl_->discard_count;
        const auto digest = impl_->digest.append_non_commit_record(bytes);
        if (!digest.ok()) return digest;
    } else if (type.value() == JournalRecordType::IndexNode) {
        if (impl_->expected_sequence >= impl_->max_input_records) {
            return journal_failure<void>(
                L"CheckpointSequenceValidator::append",
                L"index record count exceeds the configured bound");
        }
        const auto decoded = decode_index_node(
            bytes, record_offset, impl_->committed_bound);
        if (!decoded.ok()) return Result<void>::failure(decoded.error());
        impl_->saw_index = true;
        ++impl_->index_count;
        try {
            impl_->current_nodes.emplace(record_offset,
                                         decoded.value().record_crc32c);
        } catch (const std::bad_alloc&) {
            return Result<void>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"CheckpointSequenceValidator::append",
                L"allocating node descriptor state failed", 0});
        }
        const auto digest = impl_->digest.append_non_commit_record(bytes);
        if (!digest.ok()) return digest;
    } else {
        const auto decoded = decode_checkpoint_commit(
            bytes, impl_->header, record_offset, impl_->committed_bound);
        if (!decoded.ok()) return Result<void>::failure(decoded.error());
        const auto& commit = decoded.value();
        if (commit.previous_generation != impl_->previous_generation ||
            commit.new_generation != impl_->new_generation ||
            commit.previous_digest != impl_->previous_digest ||
            commit.checkpoint_begin_offset !=
                impl_->checkpoint_begin_offset ||
            commit.input_record_count != impl_->expected_sequence ||
            commit.data_count != impl_->data_count ||
            commit.zero_count != impl_->zero_count ||
            commit.discard_count != impl_->discard_count ||
            commit.index_node_count != impl_->index_count) {
            return journal_failure<void>(
                L"CheckpointSequenceValidator::append",
                L"commit does not describe the streamed checkpoint");
        }
        if (commit.root.offset >= impl_->checkpoint_begin_offset &&
            commit.root.offset != 0) {
            const auto node = impl_->current_nodes.find(commit.root.offset);
            if (node == impl_->current_nodes.end() ||
                node->second != commit.root.crc32c) {
                return journal_failure<void>(
                    L"CheckpointSequenceValidator::append",
                    L"commit root does not name a streamed index node");
            }
        }
        const bool genesis = impl_->previous_generation == 0 &&
                             impl_->new_generation == 0;
        if (!genesis) {
            const auto digest = impl_->digest.finalize(bytes);
            if (!digest.ok()) return Result<void>::failure(digest.error());
            if (digest.value() != commit.new_chain_digest) {
                return journal_failure<void>(
                    L"CheckpointSequenceValidator::append",
                    L"checkpoint chain digest mismatch");
            }
        }
        impl_->commit = commit;
        impl_->saw_commit = true;
    }
    impl_->next_offset += expected_length;
    ++impl_->expected_sequence;
    return Result<void>::success();
}

Result<CheckpointCommitRecord> CheckpointSequenceValidator::finalize() {
    if (!impl_ || !impl_->saw_commit || !impl_->commit.has_value() ||
        impl_->next_offset != impl_->committed_bound) {
        return journal_failure<CheckpointCommitRecord>(
            L"CheckpointSequenceValidator::finalize",
            L"checkpoint sequence is incomplete");
    }
    return Result<CheckpointCommitRecord>::success(*impl_->commit);
}

}  // namespace myrd::persistence
