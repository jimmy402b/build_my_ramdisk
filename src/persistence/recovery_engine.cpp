#include "src/persistence/recovery_engine.h"

#include "src/common/crc32c.h"
#include "src/persistence/byte_codec.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_index.h"
#include "src/persistence/journal_reader.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::size_t kRecordTypeOffset = 20;
constexpr std::size_t kRecordLengthOffset = 24;
constexpr std::size_t kRecordCrcOffset = 32;
constexpr std::size_t kJournalMajorOffset = 8;
constexpr std::size_t kJournalHeaderLengthOffset = 16;
constexpr std::size_t kJournalHeaderCrcOffset = 20;
constexpr std::size_t kJournalUuidOffset = 40;
constexpr std::size_t kJournalEpochOffset = 56;
constexpr std::size_t kJournalFeatureFlagsOffset = 224;

Error recovery_error(const PersistenceErrorCode code,
                     const wchar_t* message,
                     const std::uint32_t native_code = 0) {
    return {
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        L"recover_complete_image", message, native_code};
}

std::size_t check_index(const ManifestCandidateCheck check) noexcept {
    return static_cast<std::size_t>(check);
}

ManifestCandidateCheckStatus status_from_error(const Error& error) noexcept {
    if (error.domain == ErrorDomain::storage) {
        if (error.code == static_cast<std::uint32_t>(
                              OperationErrorCode::cancelled)) {
            return ManifestCandidateCheckStatus::Cancelled;
        }
        if (error.code == static_cast<std::uint32_t>(
                              OperationErrorCode::timed_out)) {
            return ManifestCandidateCheckStatus::DeadlineExceeded;
        }
        if (error.code == static_cast<std::uint32_t>(
                              ImagePathErrorCode::Missing)) {
            return ManifestCandidateCheckStatus::Missing;
        }
        if (error.code == static_cast<std::uint32_t>(
                              ImagePathErrorCode::AccessDenied)) {
            return ManifestCandidateCheckStatus::AccessDenied;
        }
    }
    if (error.domain == ErrorDomain::win32) {
        if (error.native_code == ERROR_FILE_NOT_FOUND ||
            error.native_code == ERROR_PATH_NOT_FOUND) {
            return ManifestCandidateCheckStatus::Missing;
        }
        if (error.native_code == ERROR_ACCESS_DENIED) {
            return ManifestCandidateCheckStatus::AccessDenied;
        }
    }
    if (error.domain == ErrorDomain::persistence) {
        if (error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::UnsupportedVhdx)) {
            return ManifestCandidateCheckStatus::Unsupported;
        }
        if (error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::InvalidFormat) ||
            error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::ManifestCorrupt) ||
            error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::JournalCorrupt) ||
            error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::BackingDataCorrupt) ||
            error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::VerificationFailed)) {
            return ManifestCandidateCheckStatus::Corrupt;
        }
        if (error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::AccessDenied)) {
            return ManifestCandidateCheckStatus::AccessDenied;
        }
        if (error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::Cancelled)) {
            return ManifestCandidateCheckStatus::Cancelled;
        }
        if (error.code == static_cast<std::uint32_t>(
                              PersistenceErrorCode::DeadlineExceeded)) {
            return ManifestCandidateCheckStatus::DeadlineExceeded;
        }
    }
    return ManifestCandidateCheckStatus::IoError;
}

bool path_integrity_error(const Error& error) noexcept {
    if (error.domain != ErrorDomain::storage) return false;
    return error.code == static_cast<std::uint32_t>(
               ImagePathErrorCode::InvalidName) ||
           error.code == static_cast<std::uint32_t>(
               ImagePathErrorCode::PathEscape) ||
           error.code == static_cast<std::uint32_t>(
               ImagePathErrorCode::ReparsePoint) ||
           error.code == static_cast<std::uint32_t>(
               ImagePathErrorCode::HardLink) ||
           error.code == static_cast<std::uint32_t>(
               ImagePathErrorCode::IdentityMismatch);
}

void pass(ManifestCandidateEvidence& evidence,
          const ManifestCandidateCheck check) noexcept {
    evidence.checks[check_index(check)].status =
        ManifestCandidateCheckStatus::Passed;
}

void fail(ManifestCandidateEvidence& evidence,
          const ManifestCandidateCheck check,
          const ManifestCandidateCheckStatus status,
          std::optional<Error> provenance = std::nullopt) {
    auto& target = evidence.checks[check_index(check)];
    target.status = status;
    target.provenance = std::move(provenance);
}

void fail(ManifestCandidateEvidence& evidence,
          const ManifestCandidateCheck check,
          const Error& provenance) {
    fail(evidence, check, status_from_error(provenance), provenance);
}

bool is_all_zero(const std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{};
    });
}

bool same_geometry(const ImageGeometry& left,
                   const ImageGeometry& right) noexcept {
    return left.virtual_bytes == right.virtual_bytes &&
           left.logical_sector_size == right.logical_sector_size &&
           left.physical_sector_size == right.physical_sector_size;
}

std::string check_name(const std::optional<ManifestCandidateCheck> check) {
    if (!check.has_value()) return "none";
    switch (*check) {
    case ManifestCandidateCheck::SlotDecode: return "slot_decode";
    case ManifestCandidateCheck::SlotSupport: return "slot_support";
    case ManifestCandidateCheck::SlotSemantics: return "slot_semantics";
    case ManifestCandidateCheck::BasePath: return "base_path";
    case ManifestCandidateCheck::JournalPath: return "journal_path";
    case ManifestCandidateCheck::BasePresence: return "base_presence";
    case ManifestCandidateCheck::BaseOpen: return "base_open";
    case ManifestCandidateCheck::BaseIdentity: return "base_identity";
    case ManifestCandidateCheck::BaseExpectedLength: return "base_length";
    case ManifestCandidateCheck::BaseProfile: return "base_profile";
    case ManifestCandidateCheck::BaseLayout: return "base_layout";
    case ManifestCandidateCheck::JournalPresence: return "journal_presence";
    case ManifestCandidateCheck::JournalOpen: return "journal_open";
    case ManifestCandidateCheck::JournalIdentity: return "journal_identity";
    case ManifestCandidateCheck::JournalHeader: return "journal_header";
    case ManifestCandidateCheck::JournalCommittedLength:
        return "journal_committed_length";
    case ManifestCandidateCheck::JournalCommit: return "journal_commit";
    case ManifestCandidateCheck::JournalRoot: return "journal_root";
    case ManifestCandidateCheck::JournalDigest: return "journal_digest";
    case ManifestCandidateCheck::Count: break;
    }
    return "invalid";
}

std::string status_name(const ManifestCandidateCheckStatus status) {
    switch (status) {
    case ManifestCandidateCheckStatus::NotRun: return "not_run";
    case ManifestCandidateCheckStatus::Passed: return "passed";
    case ManifestCandidateCheckStatus::Missing: return "missing";
    case ManifestCandidateCheckStatus::Corrupt: return "corrupt";
    case ManifestCandidateCheckStatus::Unsupported: return "unsupported";
    case ManifestCandidateCheckStatus::AccessDenied: return "access_denied";
    case ManifestCandidateCheckStatus::IoError: return "io_error";
    case ManifestCandidateCheckStatus::Cancelled: return "cancelled";
    case ManifestCandidateCheckStatus::DeadlineExceeded:
        return "deadline_exceeded";
    case ManifestCandidateCheckStatus::Conflict: return "conflict";
    }
    return "invalid";
}

struct CandidateResources final {
    std::unique_ptr<VhdxReader> base_reader{};
    std::optional<VhdxLayout> base_layout{};
    WindowsFileIdentity base_windows_identity{};
    std::shared_ptr<IPersistenceFile> journal_file{};
    WindowsFileIdentity journal_windows_identity{};
    JournalHeader journal_header{};
    CheckpointCommitRecord journal_commit{};
    VhdxParseEvidence base_parse_evidence{};
    VhdxReaderStats base_stats_at_open{};
};

std::uint64_t read_u64_le(const std::span<const std::byte> bytes,
                          const std::size_t offset,
                          bool& ok) noexcept {
    std::uint64_t value{};
    ok = offset <= bytes.size() && sizeof(value) <= bytes.size() - offset;
    if (!ok) return 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8U);
    }
    return value;
}

std::uint16_t read_u16_le(const std::span<const std::byte> bytes,
                          const std::size_t offset,
                          bool& ok) noexcept {
    ok = offset <= bytes.size() && sizeof(std::uint16_t) <= bytes.size() - offset;
    if (!ok) return 0;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U;
}

std::uint32_t read_u32_le(const std::span<const std::byte> bytes,
                          const std::size_t offset,
                          bool& ok) noexcept {
    ok = offset <= bytes.size() && sizeof(std::uint32_t) <= bytes.size() - offset;
    if (!ok) return 0;
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8U);
    }
    return value;
}

bool crc_valid_journal_header(
    const std::span<const std::byte> bytes) {
    if (bytes.size() != kJournalAlignment) return false;
    bool crc_ok{};
    const auto stored = read_u32_le(bytes, kJournalHeaderCrcOffset, crc_ok);
    if (!crc_ok) return false;
    std::array<std::byte, kJournalAlignment> copy{};
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    std::fill_n(copy.begin() + kJournalHeaderCrcOffset,
                sizeof(std::uint32_t), std::byte{});
    return crc32c(copy) == stored;
}

bool raw_journal_identity_matches(
    const std::span<const std::byte> bytes,
    const ManifestSlot& slot) noexcept {
    if (bytes.size() < kJournalEpochOffset + sizeof(std::uint64_t)) {
        return false;
    }
    if (!std::equal(
            slot.journal_identity.journal_uuid.begin(),
            slot.journal_identity.journal_uuid.end(),
            bytes.begin() + kJournalUuidOffset)) {
        return false;
    }
    bool epoch_ok{};
    return read_u64_le(bytes, kJournalEpochOffset, epoch_ok) ==
               slot.generation.journal.epoch && epoch_ok;
}

bool raw_journal_header_is_unsupported(
    const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != kJournalAlignment ||
        !std::equal(kJournalHeaderMagic.begin(), kJournalHeaderMagic.end(),
                    bytes.begin()) ||
        !crc_valid_journal_header(bytes)) {
        return false;
    }
    bool major_ok{};
    bool length_ok{};
    bool flags_ok{};
    const auto major = read_u32_le(bytes, kJournalMajorOffset, major_ok);
    const auto length = read_u32_le(
        bytes, kJournalHeaderLengthOffset, length_ok);
    const auto flags = read_u64_le(
        bytes, kJournalFeatureFlagsOffset, flags_ok);
    return major_ok && length_ok && flags_ok &&
           length == kJournalAlignment &&
           (major != kM3FormatMajor ||
            (flags & kJournalMandatoryFeatureMask) != 0);
}

Result<CheckpointCommitRecord> validate_checkpoint_digest(
    const OperationContext& context,
    const std::shared_ptr<IPersistenceFile>& file,
    const JournalHeader& header,
    const CheckpointCommitRecord& expected,
    const PersistenceLimits& limits) {
    if (expected.checkpoint_begin_offset > expected.journal_committed_end ||
        expected.journal_committed_end - expected.checkpoint_begin_offset >
            limits.max_journal_committed_bytes ||
        expected.input_record_count > limits.max_journal_records ||
        expected.input_record_count ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<CheckpointCommitRecord>::failure(
            recovery_error(PersistenceErrorCode::JournalCorrupt,
                           L"checkpoint digest range exceeds limits"));
    }
    auto validator = CheckpointSequenceValidator::begin(
        header, expected.previous_generation, expected.new_generation,
        expected.previous_digest, expected.checkpoint_begin_offset,
        expected.journal_committed_end, limits.max_journal_records);
    if (!validator.ok()) {
        return Result<CheckpointCommitRecord>::failure(validator.error());
    }

    std::uint64_t cursor = expected.checkpoint_begin_offset;
    std::uint64_t records{};
    while (cursor < expected.journal_committed_end) {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<CheckpointCommitRecord>::failure(live.error());
        }
        std::array<std::byte, kJournalAlignment> first{};
        auto read = file->read_exact(context, cursor, first);
        if (!read.ok()) {
            return Result<CheckpointCommitRecord>::failure(read.error());
        }
        bool type_ok{};
        bool length_ok{};
        const auto type_wire = read_u16_le(first, kRecordTypeOffset, type_ok);
        const auto length = read_u64_le(first, kRecordLengthOffset, length_ok);
        if (!type_ok || !length_ok || length < kJournalAlignment ||
            length % kJournalAlignment != 0 ||
            length > limits.max_io_scratch_bytes ||
            length > (std::numeric_limits<std::size_t>::max)() ||
            cursor > expected.journal_committed_end ||
            length > expected.journal_committed_end - cursor) {
            return Result<CheckpointCommitRecord>::failure(
                recovery_error(PersistenceErrorCode::JournalCorrupt,
                               L"checkpoint record length is invalid"));
        }
        const auto type = static_cast<JournalRecordType>(type_wire);
        const auto expected_length = type == JournalRecordType::Data
            ? kDataRecordBytes : kFixedRecordBytes;
        if (length != expected_length ||
            (type != JournalRecordType::Data &&
             type != JournalRecordType::Zero &&
             type != JournalRecordType::Discard &&
             type != JournalRecordType::IndexNode &&
             type != JournalRecordType::CheckpointCommit)) {
            return Result<CheckpointCommitRecord>::failure(
                recovery_error(PersistenceErrorCode::JournalCorrupt,
                               L"checkpoint record type is invalid"));
        }
        std::vector<std::byte> bytes;
        try {
            bytes.resize(static_cast<std::size_t>(length));
        } catch (const std::bad_alloc&) {
            return Result<CheckpointCommitRecord>::failure(
                recovery_error(PersistenceErrorCode::IoError,
                               L"checkpoint verification allocation failed"));
        }
        std::copy(first.begin(), first.end(), bytes.begin());
        if (length > first.size()) {
            auto remainder = std::span<std::byte>{bytes}.subspan(first.size());
            read = file->read_exact(
                context, cursor + first.size(), remainder);
            if (!read.ok()) {
                return Result<CheckpointCommitRecord>::failure(read.error());
            }
        }
        auto appended = validator.value().append(bytes, cursor);
        if (!appended.ok()) {
            return Result<CheckpointCommitRecord>::failure(appended.error());
        }
        cursor += length;
        ++records;
        if (records > limits.max_journal_records + 1) {
            return Result<CheckpointCommitRecord>::failure(
                recovery_error(PersistenceErrorCode::JournalCorrupt,
                               L"checkpoint record count exceeds limits"));
        }
    }
    auto verified = validator.value().finalize();
    if (!verified.ok()) {
        return Result<CheckpointCommitRecord>::failure(verified.error());
    }
    return verified;
}

void validate_candidate(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const std::span<const std::byte> slot_bytes,
    const std::uint8_t physical_slot,
    const RecoveryOptions& options,
    ManifestCandidateEvidence& evidence,
    CandidateResources& resources) {
    evidence.slot_index = physical_slot;
    if (is_all_zero(slot_bytes)) {
        fail(evidence, ManifestCandidateCheck::SlotDecode,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::ManifestCorrupt,
                            L"manifest slot is empty"));
        return;
    }
    auto inspected = inspect_manifest_slot(slot_bytes);
    if (!inspected.ok()) {
        fail(evidence, ManifestCandidateCheck::SlotDecode, inspected.error());
        return;
    }
    pass(evidence, ManifestCandidateCheck::SlotDecode);
    if (inspected.value().status ==
        ManifestSlotInspectionStatus::Unsupported) {
        fail(evidence, ManifestCandidateCheck::SlotSupport,
             ManifestCandidateCheckStatus::Unsupported,
             recovery_error(PersistenceErrorCode::ManifestCorrupt,
                            L"manifest major version is unsupported"));
        return;
    }
    if (!inspected.value().slot.has_value()) {
        fail(evidence, ManifestCandidateCheck::SlotSupport,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::ManifestCorrupt,
                            L"supported manifest has no decoded fields"));
        return;
    }
    const auto inspected_m2_block_size =
        inspected.value().m2_block_size;
    evidence.slot = std::move(*inspected.value().slot);
    if ((evidence.slot->feature_flags & kManifestMandatoryFeatureMask) != 0) {
        fail(evidence, ManifestCandidateCheck::SlotSupport,
             ManifestCandidateCheckStatus::Unsupported,
             recovery_error(PersistenceErrorCode::ManifestCorrupt,
                            L"manifest mandatory feature is unsupported"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::SlotSupport);
    auto semantics = validate_manifest_slot_semantics(*evidence.slot);
    if (!semantics.ok() ||
        inspected_m2_block_size != kM2BlockSize ||
        evidence.slot->slot_index != physical_slot) {
        fail(evidence, ManifestCandidateCheck::SlotSemantics,
             ManifestCandidateCheckStatus::Corrupt,
             semantics.ok()
                 ? std::optional<Error>{recovery_error(
                       PersistenceErrorCode::ManifestCorrupt,
                       L"manifest slot index does not match its position")}
                 : std::optional<Error>{semantics.error()});
        return;
    }
    if (evidence.slot->geometry.virtual_bytes >
        options.limits.max_virtual_bytes) {
        fail(evidence, ManifestCandidateCheck::SlotSemantics,
             ManifestCandidateCheckStatus::Unsupported,
             recovery_error(PersistenceErrorCode::ManifestCorrupt,
                            L"manifest geometry exceeds configured limit"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::SlotSemantics);
    evidence.trusted_generation =
        evidence.slot->generation.checkpoint_generation;
    const auto& slot = *evidence.slot;

    const auto base_path = validate_image_component(slot.base_relative_name);
    if (!base_path.ok()) {
        fail(evidence, ManifestCandidateCheck::BasePath,
             ManifestCandidateCheckStatus::Corrupt, base_path.error());
        return;
    }
    pass(evidence, ManifestCandidateCheck::BasePath);
    const auto journal_path =
        validate_image_component(slot.journal_relative_name);
    if (!journal_path.ok()) {
        fail(evidence, ManifestCandidateCheck::JournalPath,
             ManifestCandidateCheckStatus::Corrupt, journal_path.error());
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalPath);

    auto base_capability = open_bundle_file_for_inspection(
        bundle_root, slot.base_relative_name);
    if (!base_capability.ok()) {
        const auto status = status_from_error(base_capability.error());
        fail(evidence, path_integrity_error(base_capability.error())
                 ? ManifestCandidateCheck::BasePath
                 : status == ManifestCandidateCheckStatus::Missing
                 ? ManifestCandidateCheck::BasePresence
                 : ManifestCandidateCheck::BaseOpen,
             path_integrity_error(base_capability.error())
                 ? ManifestCandidateCheckStatus::Corrupt : status,
             base_capability.error());
        if (status != ManifestCandidateCheckStatus::Missing &&
            !path_integrity_error(base_capability.error())) {
            pass(evidence, ManifestCandidateCheck::BasePresence);
        }
        return;
    }
    resources.base_windows_identity = base_capability.value().identity();
    pass(evidence, ManifestCandidateCheck::BasePresence);
    auto base_file = open_validated_persistence_file(
        base_capability.move_value());
    if (!base_file.ok()) {
        fail(evidence, ManifestCandidateCheck::BaseOpen, base_file.error());
        return;
    }
    pass(evidence, ManifestCandidateCheck::BaseOpen);
    const auto max_payloads =
        options.limits.max_virtual_bytes / kVhdxDataBlockBytes + 1;
    auto owned_base_file = base_file.move_value();
    VhdxParseEvidence parse_evidence{};
    auto base_reader = VhdxReader::open(
        context, owned_base_file,
        {{options.limits.max_bat_cache_bytes,
          options.limits.max_bat_cache_entries},
         max_payloads, max_payloads},
        &parse_evidence);
    resources.base_parse_evidence = parse_evidence;
    if (!base_reader.ok()) {
        if (!parse_evidence.identity.has_value()) {
            fail(evidence, ManifestCandidateCheck::BaseIdentity,
                 base_reader.error());
            return;
        }
        if (*parse_evidence.identity != slot.base_identity) {
            fail(evidence, ManifestCandidateCheck::BaseIdentity,
                 ManifestCandidateCheckStatus::Corrupt,
                 recovery_error(PersistenceErrorCode::BackingDataCorrupt,
                                L"VHDX identity does not match manifest"));
            return;
        }
        pass(evidence, ManifestCandidateCheck::BaseIdentity);
        if (!parse_evidence.file_length.has_value() ||
            *parse_evidence.file_length != slot.base_expected_length ||
            resources.base_windows_identity.size !=
                slot.base_expected_length) {
            fail(evidence, ManifestCandidateCheck::BaseExpectedLength,
                 ManifestCandidateCheckStatus::Corrupt,
                 recovery_error(PersistenceErrorCode::BackingDataCorrupt,
                                L"VHDX length does not match manifest"));
            return;
        }
        pass(evidence, ManifestCandidateCheck::BaseExpectedLength);
        if (parse_evidence.stage == VhdxParseStage::Profile) {
            fail(evidence, ManifestCandidateCheck::BaseProfile,
                 status_from_error(base_reader.error()),
                 base_reader.error());
            return;
        }
        pass(evidence, ManifestCandidateCheck::BaseProfile);
        fail(evidence, ManifestCandidateCheck::BaseLayout,
             status_from_error(base_reader.error()),
             base_reader.error());
        return;
    }
    const auto& layout = base_reader.value()->layout();
    resources.base_stats_at_open = base_reader.value()->stats();
    if (layout.file_identity() != slot.base_identity) {
        fail(evidence, ManifestCandidateCheck::BaseIdentity,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::BackingDataCorrupt,
                            L"VHDX identity does not match manifest"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::BaseIdentity);
    if (layout.file_length() != slot.base_expected_length ||
        resources.base_windows_identity.size != slot.base_expected_length) {
        fail(evidence, ManifestCandidateCheck::BaseExpectedLength,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::BackingDataCorrupt,
                            L"VHDX length does not match manifest"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::BaseExpectedLength);
    if (layout.block_size() != kVhdxDataBlockBytes ||
        layout.physical_sector_size() != kPhysicalSectorSize) {
        fail(evidence, ManifestCandidateCheck::BaseProfile,
             ManifestCandidateCheckStatus::Unsupported,
             recovery_error(PersistenceErrorCode::UnsupportedVhdx,
                            L"VHDX profile is unsupported"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::BaseProfile);
    if (layout.virtual_size() != slot.geometry.virtual_bytes ||
        layout.logical_sector_size() !=
            slot.geometry.logical_sector_size ||
        layout.physical_sector_size() !=
            slot.geometry.physical_sector_size) {
        fail(evidence, ManifestCandidateCheck::BaseLayout,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::BackingDataCorrupt,
                            L"VHDX geometry does not match manifest"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::BaseLayout);
    resources.base_layout = layout;
    resources.base_reader = base_reader.move_value();

    auto journal_capability = open_bundle_file_for_inspection(
        bundle_root, slot.journal_relative_name);
    if (!journal_capability.ok()) {
        const auto status = status_from_error(journal_capability.error());
        fail(evidence, path_integrity_error(journal_capability.error())
                 ? ManifestCandidateCheck::JournalPath
                 : status == ManifestCandidateCheckStatus::Missing
                 ? ManifestCandidateCheck::JournalPresence
                 : ManifestCandidateCheck::JournalOpen,
             path_integrity_error(journal_capability.error())
                 ? ManifestCandidateCheckStatus::Corrupt : status,
             journal_capability.error());
        if (status != ManifestCandidateCheckStatus::Missing &&
            !path_integrity_error(journal_capability.error())) {
            pass(evidence, ManifestCandidateCheck::JournalPresence);
        }
        return;
    }
    resources.journal_windows_identity = journal_capability.value().identity();
    pass(evidence, ManifestCandidateCheck::JournalPresence);
    auto journal_unique = open_validated_persistence_file(
        journal_capability.move_value());
    if (!journal_unique.ok()) {
        fail(evidence, ManifestCandidateCheck::JournalOpen,
             journal_unique.error());
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalOpen);
    resources.journal_file = std::shared_ptr<IPersistenceFile>(
        journal_unique.move_value());
    std::array<std::byte, kJournalAlignment> header_bytes{};
    auto read = resources.journal_file->read_exact(
        context, 0, header_bytes);
    if (!read.ok()) {
        fail(evidence, ManifestCandidateCheck::JournalIdentity,
             read.error());
        return;
    }
    auto header = decode_journal_header(header_bytes);
    if (!header.ok()) {
        const bool trusted_raw_identity =
            crc_valid_journal_header(header_bytes) &&
            raw_journal_identity_matches(header_bytes, slot);
        if (trusted_raw_identity) {
            pass(evidence, ManifestCandidateCheck::JournalIdentity);
            fail(evidence, ManifestCandidateCheck::JournalHeader,
                 raw_journal_header_is_unsupported(header_bytes)
                     ? ManifestCandidateCheckStatus::Unsupported
                     : ManifestCandidateCheckStatus::Corrupt,
                 header.error());
        } else {
            fail(evidence, ManifestCandidateCheck::JournalIdentity,
                 header.error());
        }
        return;
    }
    if (header.value().journal_identity != slot.journal_identity) {
        fail(evidence, ManifestCandidateCheck::JournalIdentity,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::JournalCorrupt,
                            L"journal identity does not match manifest"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalIdentity);
    const auto base_digest = compute_base_identity_digest(slot.base_identity);
    if (!base_digest.ok() || header.value().image_uuid != slot.image_uuid ||
        header.value().epoch != slot.generation.journal.epoch ||
        !same_geometry(header.value().geometry, slot.geometry) ||
        header.value().base_checkpoint_generation !=
            slot.generation.base_generation ||
        header.value().base_identity_digest != base_digest.value() ||
        header.value().feature_flags != slot.feature_flags) {
        fail(evidence, ManifestCandidateCheck::JournalHeader,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::JournalCorrupt,
                            L"journal header does not match manifest"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalHeader);
    const auto committed = slot.generation.journal.committed_length;
    if (committed > options.limits.max_journal_committed_bytes ||
        committed < 2 * kJournalAlignment ||
        committed % kJournalAlignment != 0 ||
        resources.journal_windows_identity.size < committed) {
        fail(evidence, ManifestCandidateCheck::JournalCommittedLength,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::JournalCorrupt,
                            L"journal committed length is unavailable"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalCommittedLength);
    const auto& commit_descriptor = slot.generation.journal.commit;
    if (commit_descriptor.length != kFixedRecordBytes ||
        commit_descriptor.offset > committed ||
        commit_descriptor.length > committed - commit_descriptor.offset) {
        fail(evidence, ManifestCandidateCheck::JournalCommit,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(PersistenceErrorCode::JournalCorrupt,
                            L"journal commit descriptor is out of range"));
        return;
    }
    std::array<std::byte, kFixedRecordBytes> commit_bytes{};
    read = resources.journal_file->read_exact(
        context, commit_descriptor.offset, commit_bytes);
    if (!read.ok()) {
        fail(evidence, ManifestCandidateCheck::JournalCommit, read.error());
        return;
    }
    auto commit = decode_checkpoint_commit(
        commit_bytes, header.value(), commit_descriptor.offset, committed);
    bool commit_crc_ok{};
    const auto stored_commit_crc = read_u32_le(
        commit_bytes, kRecordCrcOffset, commit_crc_ok);
    if (!commit.ok() ||
        !commit_crc_ok || stored_commit_crc != commit_descriptor.crc32c ||
        commit.value().new_generation !=
            slot.generation.checkpoint_generation ||
        commit.value().journal_committed_end != committed ||
        commit.value().root != slot.generation.journal.root ||
        commit.value().new_chain_digest != commit_descriptor.chain_digest ||
        commit.value().common.checkpoint_generation !=
            slot.generation.checkpoint_generation) {
        fail(evidence, ManifestCandidateCheck::JournalCommit,
             ManifestCandidateCheckStatus::Corrupt,
             commit.ok() ? std::optional<Error>{recovery_error(
                               PersistenceErrorCode::JournalCorrupt,
                               L"journal commit does not match manifest")}
                         : std::optional<Error>{commit.error()});
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalCommit);
    JournalIndexView root_view{};
    root_view.file = resources.journal_file;
    root_view.file_identity = resources.journal_windows_identity;
    root_view.journal_identity = slot.journal_identity;
    root_view.epoch = slot.generation.journal.epoch;
    root_view.root_generation = slot.generation.checkpoint_generation;
    root_view.committed_length = committed;
    root_view.root = slot.generation.journal.root;
    const bool empty_root = root_view.root.offset == 0 &&
                            root_view.root.length == 0 &&
                            root_view.root.crc32c == 0;
    if (!empty_root) {
        auto index_reader = JournalIndexReader::create({
            options.limits.max_index_cache_bytes,
            options.limits.max_index_cache_entries});
        if (!index_reader.ok()) {
            fail(evidence, ManifestCandidateCheck::JournalRoot,
                 index_reader.error());
            return;
        }
        auto root = index_reader.value()->read_node(
            context, root_view, root_view.root.offset,
            root_view.root.crc32c);
        if (!root.ok()) {
            fail(evidence, ManifestCandidateCheck::JournalRoot,
                 root.error());
            return;
        }
        if (root.value().node.common.checkpoint_generation !=
            slot.generation.checkpoint_generation) {
            fail(evidence, ManifestCandidateCheck::JournalRoot,
                 ManifestCandidateCheckStatus::Corrupt,
                 recovery_error(
                     PersistenceErrorCode::JournalCorrupt,
                     L"journal root generation does not match"));
            return;
        }
    }
    pass(evidence, ManifestCandidateCheck::JournalRoot);
    auto digest = validate_checkpoint_digest(
        context, resources.journal_file, header.value(), commit.value(),
        options.limits);
    if (!digest.ok()) {
        fail(evidence, ManifestCandidateCheck::JournalDigest,
             digest.error());
        return;
    }
    if (digest.value() != commit.value() ||
        digest.value().new_chain_digest != commit_descriptor.chain_digest) {
        fail(evidence, ManifestCandidateCheck::JournalDigest,
             ManifestCandidateCheckStatus::Corrupt,
             recovery_error(
                 PersistenceErrorCode::JournalCorrupt,
                 L"journal checkpoint digest does not match"));
        return;
    }
    pass(evidence, ManifestCandidateCheck::JournalDigest);
    resources.journal_header = header.move_value();
    resources.journal_commit = commit.move_value();
}

RecoveryCandidateDiagnostic redact(
    const ManifestCandidateDiagnostic& source) {
    RecoveryCandidateDiagnostic result{};
    result.slot_index = source.slot_index;
    result.generation = source.generation;
    result.first_failed_check = source.first_failed_check;
    result.status = source.status;
    result.selected = source.selected;
    if (source.provenance.has_value()) {
        result.error_domain = source.provenance->domain;
        result.error_code = source.provenance->code;
        result.native_code = source.provenance->native_code;
        result.has_error_provenance = true;
    }
    result.safe_context =
        "slot=" + std::string(1, source.slot_index == 0 ? 'A' : 'B') +
        ";check=" + check_name(source.first_failed_check) +
        ";status=" + status_name(source.status);
    return result;
}

bool operational_failure(
    const ManifestCandidateDiagnostic& diagnostic) noexcept {
    return diagnostic.status == ManifestCandidateCheckStatus::AccessDenied ||
           diagnostic.status == ManifestCandidateCheckStatus::IoError ||
           diagnostic.status == ManifestCandidateCheckStatus::Cancelled ||
           diagnostic.status ==
               ManifestCandidateCheckStatus::DeadlineExceeded;
}

Error redacted_operational_error(
    const ManifestCandidateDiagnostic& diagnostic) {
    PersistenceErrorCode code = PersistenceErrorCode::IoError;
    switch (diagnostic.status) {
    case ManifestCandidateCheckStatus::AccessDenied:
        code = PersistenceErrorCode::AccessDenied;
        break;
    case ManifestCandidateCheckStatus::Cancelled:
        code = PersistenceErrorCode::Cancelled;
        break;
    case ManifestCandidateCheckStatus::DeadlineExceeded:
        code = PersistenceErrorCode::DeadlineExceeded;
        break;
    default:
        break;
    }
    return recovery_error(
        code, L"candidate validation ended with an operational error",
        diagnostic.provenance.has_value()
            ? diagnostic.provenance->native_code : 0);
}

}  // namespace

RecoveryOutcome recover_complete_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name,
    const RecoveryOptions& options) {
    RecoveryOutcome outcome{};
    outcome.diagnostics[0].slot_index = 0;
    outcome.diagnostics[1].slot_index = 1;
    if (validate_limits(options.limits) != LimitsStatus::Ok ||
        options.limits.max_bat_cache_bytes < kVhdxBatPageBytes ||
        options.limits.max_bat_cache_entries >
            options.limits.max_bat_cache_bytes / kVhdxBatPageBytes) {
        outcome.error = recovery_error(
            PersistenceErrorCode::InvalidFormat,
            L"recovery limits are invalid");
        return outcome;
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        outcome.error = live.error();
        return outcome;
    }
    if (!validate_image_component(manifest_relative_name).ok()) {
        outcome.error = recovery_error(
            PersistenceErrorCode::InvalidFormat,
            L"manifest name is invalid");
        return outcome;
    }
    auto manifest_capability = open_bundle_file_for_inspection(
        bundle_root, manifest_relative_name);
    if (!manifest_capability.ok()) {
        outcome.error = manifest_capability.error();
        return outcome;
    }
    if (manifest_capability.value().identity().size != kManifestFileBytes) {
        outcome.error = recovery_error(
            PersistenceErrorCode::ManifestCorrupt,
            L"manifest file length is not exactly 8192 bytes");
        for (std::uint8_t slot = 0; slot < 2; ++slot) {
            outcome.diagnostics[slot].first_failed_check =
                ManifestCandidateCheck::SlotDecode;
            outcome.diagnostics[slot].status =
                ManifestCandidateCheckStatus::Corrupt;
            outcome.diagnostics[slot].error_domain = ErrorDomain::persistence;
            outcome.diagnostics[slot].error_code = outcome.error->code;
            outcome.diagnostics[slot].has_error_provenance = true;
            outcome.diagnostics[slot].safe_context =
                "slot=" + std::string(1, slot == 0 ? 'A' : 'B') +
                ";check=slot_decode;status=corrupt";
        }
        return outcome;
    }
    auto manifest_file = open_validated_persistence_file(
        manifest_capability.move_value());
    if (!manifest_file.ok()) {
        outcome.error = manifest_file.error();
        return outcome;
    }
    std::array<std::byte, kManifestFileBytes> bytes{};
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        auto read = manifest_file.value()->read_exact(
            context, static_cast<std::uint64_t>(slot) * kManifestSlotBytes,
            std::span<std::byte>{bytes}.subspan(
                static_cast<std::size_t>(slot) * kManifestSlotBytes,
                kManifestSlotBytes));
        if (!read.ok()) {
            auto& diagnostic = outcome.diagnostics[slot];
            diagnostic.first_failed_check =
                ManifestCandidateCheck::SlotDecode;
            diagnostic.status = status_from_error(read.error());
            diagnostic.error_domain = read.error().domain;
            diagnostic.error_code = read.error().code;
            diagnostic.native_code = read.error().native_code;
            diagnostic.has_error_provenance = true;
            diagnostic.safe_context =
                "slot=" + std::string(1, slot == 0 ? 'A' : 'B') +
                ";check=slot_decode;status=" +
                status_name(diagnostic.status);
            outcome.error = read.error();
            return outcome;
        }
    }

    std::array<ManifestCandidateEvidence, 2> evidence{};
    std::array<CandidateResources, 2> resources{};
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        validate_candidate(
            context, bundle_root,
            std::span<const std::byte>{bytes}.subspan(
                static_cast<std::size_t>(slot) * kManifestSlotBytes,
                kManifestSlotBytes),
            slot, options, evidence[slot], resources[slot]);
    }
    auto selected = select_complete_manifest_generation(evidence);
    outcome.diagnostics = {
        redact(selected.diagnostics[0]), redact(selected.diagnostics[1])};
    for (std::size_t slot = 0; slot < resources.size(); ++slot) {
        outcome.diagnostics[slot].base_structural_read_calls =
            resources[slot].base_parse_evidence.structural_read_calls;
        outcome.diagnostics[slot].base_structural_read_bytes =
            resources[slot].base_parse_evidence.structural_read_bytes;
        outcome.diagnostics[slot].base_bat_read_calls_at_open =
            resources[slot].base_stats_at_open.bat_read_calls;
        outcome.diagnostics[slot].base_payload_read_calls_at_open =
            resources[slot].base_stats_at_open.payload_read_calls;
    }
    std::optional<CheckpointGeneration> selected_generation{};
    if (selected.selection.has_value()) {
        selected_generation =
            selected.selection->slot.generation.checkpoint_generation;
    }
    const ManifestCandidateDiagnostic* blocking_operational{};
    for (const auto& diagnostic : selected.diagnostics) {
        if (!operational_failure(diagnostic) ||
            !diagnostic.generation.has_value()) {
            continue;
        }
        if (!selected_generation.has_value() ||
            *diagnostic.generation >= *selected_generation) {
            if (blocking_operational == nullptr ||
                *diagnostic.generation >
                    *blocking_operational->generation) {
                blocking_operational = &diagnostic;
            }
        }
    }
    if (blocking_operational != nullptr) {
        for (auto& diagnostic : outcome.diagnostics) {
            diagnostic.selected = false;
        }
        outcome.error = redacted_operational_error(*blocking_operational);
        return outcome;
    }
    if (!selected.ok()) {
        outcome.error = selected.error.value_or(recovery_error(
            PersistenceErrorCode::ManifestCorrupt,
            L"manifest selection failed"));
        return outcome;
    }
    const auto selected_slot = selected.selection->slot_index;
    auto& resource = resources[selected_slot];
    const auto& slot = selected.selection->slot;
    if (!resource.base_reader || !resource.base_layout.has_value() ||
        !resource.journal_file) {
        outcome.error = recovery_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"selected candidate has no validated resources");
        return outcome;
    }

    JournalReaderOptions journal_options{};
    journal_options.view.file = resource.journal_file;
    journal_options.view.file_identity = resource.journal_windows_identity;
    journal_options.view.journal_identity = slot.journal_identity;
    journal_options.view.epoch = slot.generation.journal.epoch;
    journal_options.view.root_generation =
        slot.generation.checkpoint_generation;
    journal_options.view.committed_length =
        slot.generation.journal.committed_length;
    journal_options.view.root = slot.generation.journal.root;
    journal_options.geometry = slot.geometry;
    journal_options.index_cache = {
        options.limits.max_index_cache_bytes,
        options.limits.max_index_cache_entries};
    auto journal_reader = JournalReader::create(std::move(journal_options));
    if (!journal_reader.ok()) {
        outcome.error = journal_reader.error();
        return outcome;
    }

    CompositeBackingIdentity identity{};
    identity.image_uuid = slot.image_uuid;
    identity.geometry = slot.geometry;
    identity.checkpoint_generation =
        slot.generation.checkpoint_generation;
    identity.base_generation = slot.generation.base_generation;
    identity.base_windows_identity = resource.base_windows_identity;
    identity.base_vhdx_identity = resource.base_layout->file_identity();
    identity.virtual_disk_id = resource.base_layout->virtual_disk_id();
    identity.base_expected_length = slot.base_expected_length;
    identity.journal_windows_identity = resource.journal_windows_identity;
    identity.journal_identity = slot.journal_identity;
    identity.journal_epoch = slot.generation.journal.epoch;
    identity.journal_root_generation =
        slot.generation.checkpoint_generation;
    identity.journal_committed_length =
        slot.generation.journal.committed_length;
    identity.commit = slot.generation.journal.commit;
    identity.root = slot.generation.journal.root;
    identity.feature_flags = slot.feature_flags;
    auto composite = CompositeBackingStore::create(
        identity, journal_reader.move_value(),
        std::move(resource.base_reader));
    if (!composite.ok()) {
        outcome.error = composite.error();
        return outcome;
    }
    std::shared_ptr<const IBackingStore> store(composite.move_value());
    RecoveredImage image{};
    image.geometry = slot.geometry;
    image.image_uuid = slot.image_uuid;
    image.checkpoint_generation = slot.generation.checkpoint_generation;
    image.base_generation = slot.generation.base_generation;
    image.journal_epoch = slot.generation.journal.epoch;
    image.selected_slot = selected_slot;
    image.fallback = selected.selection->fallback;
    image.selected_manifest = slot;
    image.diagnostics = outcome.diagnostics;
    image.backing_identity = identity;
    image.backing_view = BackingView{
        slot.generation.checkpoint_generation, std::move(store)};
    outcome.image = std::move(image);
    return outcome;
}

}  // namespace myrd::persistence
