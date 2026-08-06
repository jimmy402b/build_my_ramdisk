#include "src/persistence/manifest_format.h"

#include "src/common/crc32c.h"
#include "src/persistence/byte_codec.h"
#include "src/persistence/image_paths.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace myrd::persistence {
namespace {

constexpr std::size_t kMajorOffset = 8;
constexpr std::size_t kMinorOffset = 12;
constexpr std::size_t kSlotLengthOffset = 16;
constexpr std::size_t kSlotIndexOffset = 20;
constexpr std::size_t kCrcOffset = 24;
constexpr std::size_t kImageUuidOffset = 32;
constexpr std::size_t kCheckpointGenerationOffset = 48;
constexpr std::size_t kVirtualBytesOffset = 56;
constexpr std::size_t kLogicalSectorOffset = 64;
constexpr std::size_t kPhysicalSectorOffset = 68;
constexpr std::size_t kM2BlockSizeOffset = 72;
constexpr std::size_t kBaseNameLengthOffset = 80;
constexpr std::size_t kJournalNameLengthOffset = 82;
constexpr std::size_t kBaseNameOffset = 88;
constexpr std::size_t kNameAreaBytes = 256;
constexpr std::size_t kJournalNameOffset = 344;
constexpr std::size_t kBaseFileWriteGuidOffset = 600;
constexpr std::size_t kBaseDataWriteGuidOffset = 616;
constexpr std::size_t kBaseExpectedLengthOffset = 632;
constexpr std::size_t kBaseGenerationOffset = 640;
constexpr std::size_t kJournalUuidOffset = 648;
constexpr std::size_t kJournalEpochOffset = 664;
constexpr std::size_t kJournalCommittedLengthOffset = 672;
constexpr std::size_t kCommitOffsetOffset = 680;
constexpr std::size_t kCommitLengthOffset = 688;
constexpr std::size_t kCommitCrcOffset = 696;
constexpr std::size_t kCommitDigestOffset = 704;
constexpr std::size_t kRootOffsetOffset = 736;
constexpr std::size_t kRootLengthOffset = 744;
constexpr std::size_t kRootCrcOffset = 752;
constexpr std::size_t kFeatureFlagsOffset = 760;
constexpr std::size_t kCreatedTimeOffset = 768;
constexpr std::size_t kCommittedTimeOffset = 776;
constexpr std::size_t kSemanticBytes = 784;

template <typename T>
Result<T> format_failure(std::wstring_view operation,
                         std::wstring_view message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
        std::wstring{operation}, std::wstring{message}, 0});
}

bool is_nil(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

bool aligned(std::uint64_t value) noexcept {
    return value % kJournalAlignment == 0;
}

bool range_within(std::uint64_t offset, std::uint64_t length,
                  std::uint64_t limit) noexcept {
    return offset <= limit && length <= limit - offset;
}

Result<void> validate_name(std::string_view name,
                           std::wstring_view operation) {
    if (name.empty() || name.size() > kMaxRelativeNameBytes) {
        return format_failure<void>(operation, L"relative name length is invalid");
    }
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(name.data()), name.size()};
    if (validate_bounded_utf8(bytes, kMaxRelativeNameBytes) !=
        ByteCodecError::none) {
        return format_failure<void>(operation,
                                    L"relative name is not valid bounded UTF-8");
    }
    if (!validate_image_component(name).ok()) {
        return format_failure<void>(operation,
                                    L"relative name is not a safe component");
    }
    return Result<void>::success();
}

Result<void> validate_slot(const ManifestSlot& slot,
                           std::wstring_view operation) {
    if (slot.slot_index > 1) {
        return format_failure<void>(operation, L"slot index must be A or B");
    }
    if (slot.image_uuid.is_nil()) {
        return format_failure<void>(operation, L"image UUID is nil");
    }
    if (validate_geometry(slot.geometry) != GeometryStatus::Ok) {
        return format_failure<void>(operation, L"image geometry is invalid");
    }
    if (slot.generation.checkpoint_generation == kInvalidGeneration ||
        slot.generation.base_generation == kInvalidGeneration ||
        slot.generation.base_generation >
            slot.generation.checkpoint_generation) {
        return format_failure<void>(operation,
                                    L"manifest generation relation is invalid");
    }
    if (slot.generation.journal.epoch == 0 ||
        slot.generation.journal.epoch == kInvalidGeneration) {
        return format_failure<void>(operation, L"journal epoch is invalid");
    }
    if (slot.base_expected_length == 0) {
        return format_failure<void>(operation, L"base expected length is zero");
    }
    if (is_nil(slot.base_identity.file_write_guid) ||
        is_nil(slot.base_identity.data_write_guid) ||
        is_nil(slot.journal_identity.journal_uuid)) {
        return format_failure<void>(operation, L"file identity is nil");
    }
    auto base_name = validate_name(slot.base_relative_name, operation);
    if (!base_name.ok()) return base_name;
    auto journal_name = validate_name(slot.journal_relative_name, operation);
    if (!journal_name.ok()) return journal_name;

    const auto& journal = slot.generation.journal;
    if (journal.committed_length < 2 * kJournalAlignment ||
        !aligned(journal.committed_length)) {
        return format_failure<void>(operation,
                                    L"journal committed length is invalid");
    }
    if (journal.commit.offset < kJournalAlignment ||
        journal.commit.length != kJournalAlignment ||
        !aligned(journal.commit.offset) ||
        !range_within(journal.commit.offset, journal.commit.length,
                      journal.committed_length) ||
        journal.commit.offset + journal.commit.length !=
            journal.committed_length) {
        return format_failure<void>(operation,
                                    L"commit descriptor range is invalid");
    }
    const bool empty_root = journal.root.offset == 0 && journal.root.length == 0;
    if (!empty_root &&
        (journal.root.offset < kJournalAlignment ||
         journal.root.length != kIndexNodeBytes ||
         !aligned(journal.root.offset) ||
         !range_within(journal.root.offset, journal.root.length,
                       journal.commit.offset))) {
        return format_failure<void>(operation,
                                    L"root descriptor range is invalid");
    }
    if ((journal.root.offset == 0) != (journal.root.length == 0)) {
        return format_failure<void>(operation,
                                    L"root descriptor is partially empty");
    }
    if (empty_root && journal.root.crc32c != 0) {
        return format_failure<void>(operation,
                                    L"empty root descriptor has a CRC");
    }
    if ((slot.feature_flags & kManifestMandatoryFeatureMask) != 0) {
        return format_failure<void>(operation,
                                    L"unknown mandatory feature flag");
    }
    return Result<void>::success();
}

template <typename Integer>
bool write_integer(ManifestSlotBytes& bytes, std::size_t offset,
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
bool read_integer(std::span<const std::byte> bytes, std::size_t offset,
                  Integer& value) noexcept {
    auto local = offset;
    if constexpr (sizeof(Integer) == 2) {
        return read_u16_le(bytes, local, value) == ByteCodecError::none;
    } else if constexpr (sizeof(Integer) == 4) {
        return read_u32_le(bytes, local, value) == ByteCodecError::none;
    } else {
        return read_u64_le(bytes, local, value) == ByteCodecError::none;
    }
}

bool copy_to(ManifestSlotBytes& bytes, std::size_t offset,
             std::span<const std::byte> source) noexcept {
    auto local = offset;
    return write_bytes(bytes, local, source) == ByteCodecError::none;
}

bool copy_from(std::span<const std::byte> bytes, std::size_t offset,
               std::span<std::byte> destination) noexcept {
    auto local = offset;
    return read_bytes(bytes, local, destination) == ByteCodecError::none;
}

bool reserved_zero(std::span<const std::byte> bytes,
                   std::size_t offset, std::size_t length) noexcept {
    return checked_range(offset, length, bytes.size()) == ByteCodecError::none &&
           require_zero(bytes.subspan(offset, length)) == ByteCodecError::none;
}

}  // namespace

Result<ManifestSlotBytes> encode_manifest_slot(const ManifestSlot& slot) {
    auto validation = validate_slot(slot, L"encode_manifest_slot");
    if (!validation.ok()) {
        return Result<ManifestSlotBytes>::failure(validation.error());
    }

    ManifestSlotBytes bytes{};
    bool ok = copy_to(bytes, 0, kManifestMagic) &&
              write_integer(bytes, kMajorOffset, kM3FormatMajor) &&
              write_integer(bytes, kMinorOffset, kM3FormatMinor) &&
              write_integer(bytes, kSlotLengthOffset,
                            static_cast<std::uint32_t>(kManifestSlotBytes));
    bytes[kSlotIndexOffset] = static_cast<std::byte>(slot.slot_index);
    ok = ok && copy_to(bytes, kImageUuidOffset, slot.image_uuid.bytes) &&
         write_integer(bytes, kCheckpointGenerationOffset,
                       slot.generation.checkpoint_generation) &&
         write_integer(bytes, kVirtualBytesOffset, slot.geometry.virtual_bytes) &&
         write_integer(bytes, kLogicalSectorOffset,
                       slot.geometry.logical_sector_size) &&
         write_integer(bytes, kPhysicalSectorOffset,
                       slot.geometry.physical_sector_size) &&
         write_integer(bytes, kM2BlockSizeOffset,
                       static_cast<std::uint32_t>(kM2BlockSize)) &&
         write_integer(bytes, kBaseNameLengthOffset,
                       static_cast<std::uint16_t>(slot.base_relative_name.size())) &&
         write_integer(bytes, kJournalNameLengthOffset,
                       static_cast<std::uint16_t>(slot.journal_relative_name.size()));
    ok = ok && copy_to(bytes, kBaseNameOffset,
        {reinterpret_cast<const std::byte*>(slot.base_relative_name.data()),
         slot.base_relative_name.size()}) &&
         copy_to(bytes, kJournalNameOffset,
        {reinterpret_cast<const std::byte*>(slot.journal_relative_name.data()),
         slot.journal_relative_name.size()}) &&
         copy_to(bytes, kBaseFileWriteGuidOffset,
                 slot.base_identity.file_write_guid) &&
         copy_to(bytes, kBaseDataWriteGuidOffset,
                 slot.base_identity.data_write_guid) &&
         write_integer(bytes, kBaseExpectedLengthOffset,
                       slot.base_expected_length) &&
         write_integer(bytes, kBaseGenerationOffset,
                       slot.generation.base_generation) &&
         copy_to(bytes, kJournalUuidOffset,
                 slot.journal_identity.journal_uuid) &&
         write_integer(bytes, kJournalEpochOffset,
                       slot.generation.journal.epoch) &&
         write_integer(bytes, kJournalCommittedLengthOffset,
                       slot.generation.journal.committed_length) &&
         write_integer(bytes, kCommitOffsetOffset,
                       slot.generation.journal.commit.offset) &&
         write_integer(bytes, kCommitLengthOffset,
                       slot.generation.journal.commit.length) &&
         write_integer(bytes, kCommitCrcOffset,
                       slot.generation.journal.commit.crc32c) &&
         copy_to(bytes, kCommitDigestOffset,
                 slot.generation.journal.commit.chain_digest) &&
         write_integer(bytes, kRootOffsetOffset,
                       slot.generation.journal.root.offset) &&
         write_integer(bytes, kRootLengthOffset,
                       slot.generation.journal.root.length) &&
         write_integer(bytes, kRootCrcOffset,
                       slot.generation.journal.root.crc32c) &&
         write_integer(bytes, kFeatureFlagsOffset, slot.feature_flags) &&
         write_integer(bytes, kCreatedTimeOffset, slot.created_time_100ns) &&
         write_integer(bytes, kCommittedTimeOffset,
                       slot.committed_time_100ns);
    if (!ok) {
        return format_failure<ManifestSlotBytes>(
            L"encode_manifest_slot", L"fixed manifest layout overflow");
    }

    const auto crc = crc32c(bytes);
    if (!write_integer(bytes, kCrcOffset, crc)) {
        return format_failure<ManifestSlotBytes>(
            L"encode_manifest_slot", L"CRC field overflow");
    }
    return Result<ManifestSlotBytes>::success(std::move(bytes));
}

Result<ManifestSlotInspection> inspect_manifest_slot(
    std::span<const std::byte> input) {
    if (input.size() != kManifestSlotBytes) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"slot must be exactly 4096 bytes");
    }
    if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(),
                    input.begin())) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"manifest magic mismatch");
    }

    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t slot_length = 0;
    std::uint32_t stored_crc = 0;
    if (!read_integer(input, kMajorOffset, major) ||
        !read_integer(input, kMinorOffset, minor) ||
        !read_integer(input, kSlotLengthOffset, slot_length) ||
        !read_integer(input, kCrcOffset, stored_crc)) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"fixed header is truncated");
    }
    if (slot_length != kManifestSlotBytes) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"declared slot length is invalid");
    }
    (void)minor;

    ManifestSlotBytes crc_bytes{};
    std::copy(input.begin(), input.end(), crc_bytes.begin());
    std::fill_n(crc_bytes.begin() + kCrcOffset, sizeof(stored_crc),
                std::byte{0});
    if (crc32c(crc_bytes) != stored_crc) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"manifest CRC32C mismatch");
    }
    if (major != kM3FormatMajor) {
        ManifestSlotInspection inspection{};
        inspection.status = ManifestSlotInspectionStatus::Unsupported;
        return Result<ManifestSlotInspection>::success(
            std::move(inspection));
    }

    if (!reserved_zero(input, 21, 3) || !reserved_zero(input, 28, 4) ||
        !reserved_zero(input, 76, 4) || !reserved_zero(input, 84, 4) ||
        !reserved_zero(input, 700, 4) || !reserved_zero(input, 756, 4) ||
        !reserved_zero(input, kSemanticBytes,
                       kManifestSlotBytes - kSemanticBytes)) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"reserved bytes are nonzero");
    }

    ManifestSlot slot{};
    slot.slot_index = std::to_integer<std::uint8_t>(input[kSlotIndexOffset]);
    std::uint32_t m2_block_size = 0;
    std::uint16_t base_name_length = 0;
    std::uint16_t journal_name_length = 0;
    bool ok = copy_from(input, kImageUuidOffset, slot.image_uuid.bytes) &&
              read_integer(input, kCheckpointGenerationOffset,
                           slot.generation.checkpoint_generation) &&
              read_integer(input, kVirtualBytesOffset,
                           slot.geometry.virtual_bytes) &&
              read_integer(input, kLogicalSectorOffset,
                           slot.geometry.logical_sector_size) &&
              read_integer(input, kPhysicalSectorOffset,
                           slot.geometry.physical_sector_size) &&
              read_integer(input, kM2BlockSizeOffset, m2_block_size) &&
              read_integer(input, kBaseNameLengthOffset, base_name_length) &&
              read_integer(input, kJournalNameLengthOffset,
                           journal_name_length);
    if (!ok || m2_block_size != kM2BlockSize ||
        base_name_length == 0 || base_name_length > kMaxRelativeNameBytes ||
        journal_name_length == 0 ||
        journal_name_length > kMaxRelativeNameBytes) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"geometry or name length is invalid");
    }
    if (!reserved_zero(input, kBaseNameOffset + base_name_length,
                       kNameAreaBytes - base_name_length) ||
        !reserved_zero(input, kJournalNameOffset + journal_name_length,
                       kNameAreaBytes - journal_name_length)) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"name padding is nonzero");
    }
    slot.base_relative_name.assign(
        reinterpret_cast<const char*>(input.data() + kBaseNameOffset),
        base_name_length);
    slot.journal_relative_name.assign(
        reinterpret_cast<const char*>(input.data() + kJournalNameOffset),
        journal_name_length);

    ok = copy_from(input, kBaseFileWriteGuidOffset,
                   slot.base_identity.file_write_guid) &&
         copy_from(input, kBaseDataWriteGuidOffset,
                   slot.base_identity.data_write_guid) &&
         read_integer(input, kBaseExpectedLengthOffset,
                      slot.base_expected_length) &&
         read_integer(input, kBaseGenerationOffset,
                      slot.generation.base_generation) &&
         copy_from(input, kJournalUuidOffset,
                   slot.journal_identity.journal_uuid) &&
         read_integer(input, kJournalEpochOffset,
                      slot.generation.journal.epoch) &&
         read_integer(input, kJournalCommittedLengthOffset,
                      slot.generation.journal.committed_length) &&
         read_integer(input, kCommitOffsetOffset,
                      slot.generation.journal.commit.offset) &&
         read_integer(input, kCommitLengthOffset,
                      slot.generation.journal.commit.length) &&
         read_integer(input, kCommitCrcOffset,
                      slot.generation.journal.commit.crc32c) &&
         copy_from(input, kCommitDigestOffset,
                   slot.generation.journal.commit.chain_digest) &&
         read_integer(input, kRootOffsetOffset,
                      slot.generation.journal.root.offset) &&
         read_integer(input, kRootLengthOffset,
                      slot.generation.journal.root.length) &&
         read_integer(input, kRootCrcOffset,
                      slot.generation.journal.root.crc32c) &&
         read_integer(input, kFeatureFlagsOffset, slot.feature_flags) &&
         read_integer(input, kCreatedTimeOffset, slot.created_time_100ns) &&
         read_integer(input, kCommittedTimeOffset,
                      slot.committed_time_100ns);
    if (!ok) {
        return format_failure<ManifestSlotInspection>(
            L"inspect_manifest_slot", L"fixed manifest fields are truncated");
    }

    ManifestSlotInspection inspection{};
    inspection.status = ManifestSlotInspectionStatus::DecodedV1;
    inspection.slot = std::move(slot);
    return Result<ManifestSlotInspection>::success(std::move(inspection));
}

Result<void> validate_manifest_slot_semantics(const ManifestSlot& slot) {
    constexpr auto operation = L"validate_manifest_slot_semantics";
    if (slot.slot_index > 1) {
        return format_failure<void>(operation, L"slot index must be A or B");
    }
    if (slot.image_uuid.is_nil()) {
        return format_failure<void>(operation, L"image UUID is nil");
    }
    if (validate_geometry(slot.geometry) != GeometryStatus::Ok) {
        return format_failure<void>(operation, L"image geometry is invalid");
    }
    if (slot.generation.checkpoint_generation == kInvalidGeneration ||
        slot.generation.base_generation == kInvalidGeneration ||
        slot.generation.base_generation >
            slot.generation.checkpoint_generation) {
        return format_failure<void>(
            operation, L"manifest generation relation is invalid");
    }
    if (slot.generation.journal.epoch == 0 ||
        slot.generation.journal.epoch == kInvalidGeneration) {
        return format_failure<void>(operation, L"journal epoch is invalid");
    }
    if (slot.base_expected_length == 0 ||
        is_nil(slot.base_identity.file_write_guid) ||
        is_nil(slot.base_identity.data_write_guid) ||
        is_nil(slot.journal_identity.journal_uuid)) {
        return format_failure<void>(operation, L"file identity is invalid");
    }
    const auto& journal = slot.generation.journal;
    if (journal.committed_length < 2 * kJournalAlignment ||
        !aligned(journal.committed_length) ||
        journal.commit.offset < kJournalAlignment ||
        journal.commit.length != kJournalAlignment ||
        !aligned(journal.commit.offset) ||
        !range_within(journal.commit.offset, journal.commit.length,
                      journal.committed_length) ||
        journal.commit.offset + journal.commit.length !=
            journal.committed_length) {
        return format_failure<void>(
            operation, L"journal commit range is invalid");
    }
    const bool empty_root = journal.root.offset == 0 &&
                            journal.root.length == 0;
    if ((!empty_root &&
         (journal.root.offset < kJournalAlignment ||
          journal.root.length != kIndexNodeBytes ||
          !aligned(journal.root.offset) ||
          !range_within(journal.root.offset, journal.root.length,
                        journal.commit.offset))) ||
        ((journal.root.offset == 0) != (journal.root.length == 0)) ||
        (empty_root && journal.root.crc32c != 0)) {
        return format_failure<void>(operation, L"journal root range is invalid");
    }
    return Result<void>::success();
}

Result<void> validate_manifest_slot_paths(const ManifestSlot& slot) {
    constexpr auto operation = L"validate_manifest_slot_paths";
    auto base = validate_name(slot.base_relative_name, operation);
    if (!base.ok()) return base;
    return validate_name(slot.journal_relative_name, operation);
}

Result<ManifestSlot> decode_manifest_slot(
    const std::span<const std::byte> input) {
    // Preserve the original strict decoder's stable error precedence for
    // existing callers. Recovery uses inspect_manifest_slot(), where CRC is
    // intentionally established before support classification.
    if (input.size() == kManifestSlotBytes &&
        std::equal(kManifestMagic.begin(), kManifestMagic.end(),
                   input.begin())) {
        std::uint32_t major{};
        std::uint32_t slot_length{};
        if (read_integer(input, kMajorOffset, major) &&
            read_integer(input, kSlotLengthOffset, slot_length) &&
            (major != kM3FormatMajor ||
             slot_length != kManifestSlotBytes)) {
            return format_failure<ManifestSlot>(
                L"decode_manifest_slot",
                L"unsupported version or slot length");
        }
    }
    auto inspected = inspect_manifest_slot(input);
    if (!inspected.ok()) {
        return Result<ManifestSlot>::failure(inspected.error());
    }
    if (inspected.value().status != ManifestSlotInspectionStatus::DecodedV1 ||
        !inspected.value().slot.has_value()) {
        return format_failure<ManifestSlot>(
            L"decode_manifest_slot", L"unsupported manifest version");
    }
    auto slot = std::move(*inspected.value().slot);
    auto validation = validate_slot(slot, L"decode_manifest_slot");
    if (!validation.ok()) {
        return Result<ManifestSlot>::failure(validation.error());
    }
    return Result<ManifestSlot>::success(std::move(slot));
}

}
